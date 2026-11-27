// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "foldermodel.h"

#include <QClipboard>
#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QUrl>

#include <algorithm>

namespace {

// The user's folders have their own icon, like in Windows.
QString folderIcon(const QString& path)
{
    static const QHash<QString, QString> icons = [] {
        QHash<QString, QString> map;
        auto add = [&](QStandardPaths::StandardLocation location, const char* icon) {
            const QString dir = QStandardPaths::writableLocation(location);
            if (!dir.isEmpty() && dir != QDir::homePath()) {
                map.insert(dir, QLatin1String(icon));
            }
        };
        add(QStandardPaths::DesktopLocation, "user-desktop");
        add(QStandardPaths::DocumentsLocation, "folder-documents");
        add(QStandardPaths::DownloadLocation, "folder-download");
        add(QStandardPaths::PicturesLocation, "folder-pictures");
        add(QStandardPaths::MusicLocation, "folder-music");
        add(QStandardPaths::MoviesLocation, "folder-videos");
        add(QStandardPaths::TemplatesLocation, "folder-templates");
        add(QStandardPaths::PublicShareLocation, "folder-publicshare");
        map.insert(QDir::homePath(), QStringLiteral("user-home"));
        return map;
    }();
    return icons.value(path, QStringLiteral("folder"));
}

FolderModel::Entry describe(const QFileInfo& info)
{
    static const QMimeDatabase mimes;
    FolderModel::Entry e;
    e.name = info.fileName();
    e.path = info.absoluteFilePath();
    e.isLink = info.isSymLink();
    e.isDir = info.isDir();
    e.hidden = e.name.startsWith(u'.');
    e.modified = info.lastModified();
    if (e.isDir) {
        e.type = QCoreApplication::translate("Files", "File folder");
        e.icon = folderIcon(e.path);
        return e;
    }
    e.size = info.size();
    // From the name only: opening every file to find its type would be slow.
    const QMimeType mime = mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
    e.type = mime.comment();
    // The theme is consulted in the main thread (QIcon must not be used here):
    // the specific icon and, after "|", the generic fallback.
    e.icon = mime.iconName() + u'|' + mime.genericIconName();
    const QString name = mime.name();
    e.thumbnail = name.startsWith(QLatin1String("image/")) || name.startsWith(QLatin1String("video/"))
        || name == QLatin1String("application/pdf");
    return e;
}

} // namespace

QString formatSize(qint64 bytes)
{
    const QLocale locale;
    if (bytes < 1024) {
        return QCoreApplication::translate("Files", "%1 bytes").arg(bytes);
    }
    static const char* units[] = { "KB", "MB", "GB", "TB" };
    double value = bytes / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    // In "Details" Windows writes KB without decimals ("12 KB").
    return locale.toString(value, 'f', unit == 0 ? 0 : 1) + u' ' + QLatin1String(units[unit]);
}

FolderModel::FolderModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_generation(std::make_shared<std::atomic<quint64>>(0))
{
    // "file2" before "file10", upper and lower case together: like Windows.
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(250);
    connect(&m_refresh, &QTimer::timeout, this, &FolderModel::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (m_search.isEmpty()) {
            m_refresh.start();
        }
    });
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &FolderModel::readClipboard);
}

FolderModel::~FolderModel()
{
    m_generation->fetch_add(1); // the job in progress stops
}

int FolderModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QHash<int, QByteArray> FolderModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { PathRole, "path" },
        { UrlRole, "url" },
        { IsDirRole, "isDir" },
        { IsLinkRole, "isLink" },
        { HiddenRole, "hidden" },
        { SizeRole, "size" },
        { SizeTextRole, "sizeText" },
        { ModifiedRole, "modified" },
        { ModifiedTextRole, "modifiedText" },
        { TypeRole, "type" },
        { IconRole, "iconName" },
        { ThumbnailRole, "hasThumbnail" },
        { SelectedRole, "selected" },
        { LocationRole, "location" },
        { CutRole, "isCut" },
    };
}

QVariant FolderModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) {
        return {};
    }
    const Entry& e = m_items.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case UrlRole: return QUrl::fromLocalFile(e.path).toString();
    case IsDirRole: return e.isDir;
    case IsLinkRole: return e.isLink;
    case HiddenRole: return e.hidden;
    case SizeRole: return e.size;
    case SizeTextRole: return e.isDir ? QString() : formatSize(e.size);
    case ModifiedRole: return e.modified;
    case ModifiedTextRole: return QLocale().toString(e.modified, QStringLiteral("dd/MM/yyyy HH:mm"));
    case TypeRole: return e.type;
    case IconRole: {
        // "image-png|image-x-generic": the first one the theme has
        // (remembered).
        static QHash<QString, QString> resolved;
        if (const auto it = resolved.constFind(e.icon); it != resolved.cend()) {
            return it.value();
        }
        const QString specific = e.icon.section(u'|', 0, 0);
        const QString generic = e.icon.section(u'|', 1, 1);
        return resolved.insert(e.icon, QIcon::hasThemeIcon(specific) || generic.isEmpty() ? specific : generic).value();
    }
    case ThumbnailRole: return e.thumbnail;
    case SelectedRole: return m_selected.contains(index.row());
    case LocationRole: return QFileInfo(e.path).absolutePath();
    case CutRole: return m_cut.contains(e.path);
    }
    return {};
}

// -------------------------------------------------------------- folder --

void FolderModel::setPath(const QString& path)
{
    QString clean = QDir::cleanPath(path);
    if (clean.isEmpty()) {
        clean = QDir::homePath();
    }
    if (clean == m_path && m_search.isEmpty()) {
        return;
    }
    if (!m_watcher.directories().isEmpty()) {
        m_watcher.removePaths(m_watcher.directories());
    }
    m_path = clean;
    const bool hadSearch = !m_search.isEmpty();
    m_search.clear();
    m_exists = QFileInfo(m_path).isDir();
    if (m_exists) {
        m_watcher.addPath(m_path);
    }
    emit pathChanged();
    if (hadSearch) {
        emit searchChanged();
    }
    start(false);
}

void FolderModel::setSearch(const QString& search)
{
    if (search == m_search) {
        return;
    }
    m_search = search;
    emit searchChanged();
    start(false);
}

void FolderModel::reload()
{
    start(!m_items.isEmpty() || !m_filtered.isEmpty());
}

// The job in another thread. Results reach the main thread in batches; if
// meanwhile the folder changed (a different generation) the job stops and the
// old batches are thrown away.
void FolderModel::start(bool refresh)
{
    const quint64 generation = m_generation->fetch_add(1) + 1;
    m_refreshing = refresh;
    m_incoming.clear();
    if (!refresh) {
        beginResetModel();
        m_items.clear();
        m_filtered.clear();
        m_selected.clear();
        m_current = -1;
        endResetModel();
        emit countChanged();
        ++m_selectionVersion;
        emit selectionChanged();
        emit currentIndexChanged();
    }
    m_loading = true;
    emit loadingChanged();

    const QString path = m_path;
    const QString search = m_search;
    const bool hidden = m_showHidden;
    auto counter = m_generation;
    QPointer<FolderModel> self(this);
    QThreadPool::globalInstance()->start([=] {
        QList<Entry> batch;
        QElapsedTimer sinceFlush;
        sinceFlush.start();
        bool first = true;
        auto flush = [&](bool finished) {
            QMetaObject::invokeMethod(
                qApp,
                [self, generation, entries = std::move(batch), finished]() mutable {
                    if (self) {
                        self->receive(generation, std::move(entries), finished);
                    }
                },
                Qt::QueuedConnection);
            batch = {};
            sinceFlush.restart();
            first = false;
        };
        const QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden;
        if (search.isEmpty()) {
            QDirIterator it(path, filters);
            while (it.hasNext()) {
                if (counter->load() != generation) {
                    return;
                }
                it.next();
                batch.append(describe(it.fileInfo()));
                // A normal folder arrives in one batch; a huge one shows up at
                // once in pieces.
                if (sinceFlush.elapsed() > (first ? 60 : 150)) {
                    flush(false);
                }
            }
        } else {
            // Search: in the subfolders, names containing the text (or with *
            // and ?).
            const bool wildcard = search.contains(u'*') || search.contains(u'?');
            const QRegularExpression pattern = wildcard
                ? QRegularExpression::fromWildcard(search, Qt::CaseInsensitive, QRegularExpression::UnanchoredWildcardConversion)
                : QRegularExpression();
            QDirIterator it(path, filters, QDirIterator::Subdirectories);
            int found = 0;
            while (it.hasNext() && found < 5000) {
                if (counter->load() != generation) {
                    return;
                }
                it.next();
                const QString name = it.fileName();
                if (!hidden && it.filePath().mid(path.size()).contains(QLatin1String("/."))) {
                    continue; // inside hidden folders
                }
                if (wildcard ? pattern.match(name).hasMatch() : name.contains(search, Qt::CaseInsensitive)) {
                    batch.append(describe(it.fileInfo()));
                    ++found;
                }
                if (!batch.isEmpty() && sinceFlush.elapsed() > 150) {
                    flush(false);
                }
            }
        }
        flush(true);
    });
}

void FolderModel::receive(quint64 generation, QList<Entry> entries, bool finished)
{
    if (generation != m_generation->load()) {
        return;
    }
    if (m_refreshing) {
        m_incoming.append(std::move(entries));
        if (finished) {
            merge(std::move(m_incoming));
            m_incoming.clear();
        }
    } else {
        insertBatch(std::move(entries));
    }
    if (finished) {
        m_loading = false;
        emit loadingChanged();
    }
}

bool FolderModel::lessThan(const Entry& a, const Entry& b) const
{
    // Folders first, like Windows (also when reversed).
    if (a.isDir != b.isDir) {
        return a.isDir;
    }
    int order = 0;
    switch (m_sortColumn) {
    case 1: order = a.modified < b.modified ? -1 : a.modified > b.modified ? 1 : 0; break;
    case 2: order = m_collator.compare(a.type, b.type); break;
    case 3: order = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
    default: break;
    }
    if (order == 0) {
        order = m_collator.compare(a.name, b.name);
        if (m_sortColumn != 0) {
            return order < 0; // on equal values, by name ascending
        }
    }
    return m_sortDescending ? order > 0 : order < 0;
}

void FolderModel::insertBatch(QList<Entry> entries)
{
    QList<Entry> shown;
    for (Entry& e : entries) {
        if (e.hidden && !m_showHidden) {
            m_filtered.append(std::move(e));
        } else {
            shown.append(std::move(e));
        }
    }
    if (shown.isEmpty()) {
        return;
    }
    if (m_items.isEmpty()) {
        // The first batch: all at once, already sorted.
        std::sort(shown.begin(), shown.end(), [this](const Entry& a, const Entry& b) { return lessThan(a, b); });
        beginInsertRows({}, 0, int(shown.size()) - 1);
        m_items = std::move(shown);
        endInsertRows();
        emit countChanged();
        for (int row = 0; row < m_items.size() && !m_pendingSelect.isEmpty(); ++row) {
            checkPending(row);
        }
        return;
    }
    for (const Entry& e : std::as_const(shown)) {
        insertSorted(e);
    }
}

void FolderModel::insertSorted(const Entry& entry)
{
    const auto it = std::upper_bound(m_items.begin(), m_items.end(), entry,
        [this](const Entry& a, const Entry& b) { return lessThan(a, b); });
    const int row = int(it - m_items.begin());
    beginInsertRows({}, row, row);
    m_items.insert(row, entry);
    // The selected rows after this one move down by one.
    QSet<int> shifted;
    for (int r : std::as_const(m_selected)) {
        shifted.insert(r >= row ? r + 1 : r);
    }
    m_selected = shifted;
    if (m_current >= row) {
        ++m_current;
        emit currentIndexChanged();
    }
    endInsertRows();
    emit countChanged();
    checkPending(row);
}

void FolderModel::removeAt(int row)
{
    beginRemoveRows({}, row, row);
    m_items.removeAt(row);
    QSet<int> shifted;
    bool selectionLost = false;
    for (int r : std::as_const(m_selected)) {
        if (r == row) {
            selectionLost = true;
        } else {
            shifted.insert(r > row ? r - 1 : r);
        }
    }
    m_selected = shifted;
    if (m_current == row) {
        m_current = std::min(row, int(m_items.size()) - 1);
        emit currentIndexChanged();
    } else if (m_current > row) {
        --m_current;
        emit currentIndexChanged();
    }
    endRemoveRows();
    emit countChanged();
    if (selectionLost) {
        ++m_selectionVersion;
        emit selectionChanged();
    }
}

void FolderModel::merge(QList<Entry> fresh)
{
    QHash<QString, Entry> byPath;
    for (Entry& e : fresh) {
        byPath.insert(e.path, std::move(e));
    }
    // Gone or changed.
    for (int row = int(m_items.size()) - 1; row >= 0; --row) {
        const Entry& old = m_items.at(row);
        auto it = byPath.find(old.path);
        if (it == byPath.end() || (it->hidden && !m_showHidden)) {
            removeAt(row);
            continue;
        }
        const bool changed = it->size != old.size || it->modified != old.modified || it->type != old.type
            || it->isDir != old.isDir;
        if (changed) {
            // With sorting by date or size it may need to move.
            if (m_sortColumn != 0 || it->isDir != old.isDir) {
                removeAt(row);
                continue; // it stays in byPath: put back in the right place
            }
            m_items[row] = *it;
            emit dataChanged(index(row), index(row));
        }
        byPath.erase(it);
    }
    // Arrived.
    m_filtered.clear();
    for (const Entry& e : std::as_const(byPath)) {
        if (e.hidden && !m_showHidden) {
            m_filtered.append(e);
        } else {
            insertSorted(e);
        }
    }
    ++m_selectionVersion;
    emit selectionChanged();
}

void FolderModel::resort()
{
    const QStringList selected = selectedPaths();
    const QString current = m_current >= 0 && m_current < m_items.size() ? m_items.at(m_current).path : QString();
    QList<Entry> all = m_items + m_filtered;
    m_filtered.clear();
    QList<Entry> shown;
    for (Entry& e : all) {
        if (e.hidden && !m_showHidden) {
            m_filtered.append(std::move(e));
        } else {
            shown.append(std::move(e));
        }
    }
    std::sort(shown.begin(), shown.end(), [this](const Entry& a, const Entry& b) { return lessThan(a, b); });
    beginResetModel();
    m_items = std::move(shown);
    m_selected.clear();
    m_current = -1;
    for (int row = 0; row < m_items.size(); ++row) {
        if (selected.contains(m_items.at(row).path)) {
            m_selected.insert(row);
        }
        if (m_items.at(row).path == current) {
            m_current = row;
        }
    }
    endResetModel();
    emit countChanged();
    ++m_selectionVersion;
    emit selectionChanged();
    emit currentIndexChanged();
}

void FolderModel::setShowHidden(bool on)
{
    if (on != m_showHidden) {
        m_showHidden = on;
        emit viewChanged();
        resort();
    }
}

void FolderModel::sortBy(int column, bool descending)
{
    if (column == m_sortColumn && descending == m_sortDescending) {
        return;
    }
    m_sortColumn = std::clamp(column, 0, 3);
    m_sortDescending = descending;
    emit viewChanged();
    resort();
}

QString FolderModel::pathAt(int row) const
{
    return row >= 0 && row < m_items.size() ? m_items.at(row).path : QString();
}

bool FolderModel::isDirAt(int row) const
{
    return row >= 0 && row < m_items.size() && m_items.at(row).isDir;
}

int FolderModel::indexOf(const QString& path) const
{
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).path == path) {
            return row;
        }
    }
    return -1;
}

int FolderModel::find(const QString& prefix, int from) const
{
    const int n = int(m_items.size());
    for (int i = 1; i <= n; ++i) {
        const int row = (from + i) % n;
        if (m_items.at(row).name.startsWith(prefix, Qt::CaseInsensitive)) {
            return row;
        }
    }
    return -1;
}

// ------------------------------------------------------------ selection --

void FolderModel::emitSelection(const QSet<int>& changed)
{
    for (int row : changed) {
        if (row >= 0 && row < m_items.size()) {
            emit dataChanged(index(row), index(row), { SelectedRole });
        }
    }
    ++m_selectionVersion;
    emit selectionChanged();
}

qint64 FolderModel::selectionSize() const
{
    qint64 total = 0;
    for (int row : m_selected) {
        if (row < m_items.size() && !m_items.at(row).isDir) {
            total += m_items.at(row).size;
        }
    }
    return total;
}

void FolderModel::setCurrentIndex(int row)
{
    if (row != m_current) {
        m_current = row;
        emit currentIndexChanged();
    }
}

bool FolderModel::isSelected(int row) const
{
    return m_selected.contains(row);
}

void FolderModel::select(int row)
{
    QSet<int> changed = m_selected;
    m_selected.clear();
    if (row >= 0 && row < m_items.size()) {
        m_selected.insert(row);
        changed.insert(row);
    }
    setCurrentIndex(row);
    emitSelection(changed);
}

void FolderModel::toggle(int row)
{
    if (row < 0 || row >= m_items.size()) {
        return;
    }
    if (!m_selected.remove(row)) {
        m_selected.insert(row);
    }
    setCurrentIndex(row);
    emitSelection({ row });
}

void FolderModel::selectRange(int from, int to, bool add)
{
    QSet<int> changed = m_selected;
    if (!add) {
        m_selected.clear();
    }
    const int lo = std::max(0, std::min(from, to));
    const int hi = std::min(int(m_items.size()) - 1, std::max(from, to));
    for (int row = lo; row <= hi; ++row) {
        m_selected.insert(row);
        changed.insert(row);
    }
    setCurrentIndex(to);
    emitSelection(changed);
}

void FolderModel::selectRows(const QList<int>& rows, bool add)
{
    QSet<int> changed = m_selected;
    if (!add) {
        m_selected.clear();
    }
    for (int row : rows) {
        if (row >= 0 && row < m_items.size()) {
            m_selected.insert(row);
            changed.insert(row);
        }
    }
    emitSelection(changed);
}

void FolderModel::selectAll()
{
    selectRange(0, int(m_items.size()) - 1, false);
}

void FolderModel::clearSelection()
{
    const QSet<int> changed = m_selected;
    m_selected.clear();
    emitSelection(changed);
}

void FolderModel::invertSelection()
{
    QSet<int> inverted;
    for (int row = 0; row < m_items.size(); ++row) {
        if (!m_selected.contains(row)) {
            inverted.insert(row);
        }
    }
    m_selected = inverted;
    QSet<int> all;
    for (int row = 0; row < m_items.size(); ++row) {
        all.insert(row);
    }
    emitSelection(all);
}

void FolderModel::selectPath(const QString& path)
{
    const int row = indexOf(path);
    if (row >= 0) {
        select(row);
        emit pathAppeared(row);
    } else {
        m_pendingSelect = path;
    }
}

void FolderModel::checkPending(int row)
{
    if (!m_pendingSelect.isEmpty() && m_items.at(row).path == m_pendingSelect) {
        m_pendingSelect.clear();
        select(row);
        emit pathAppeared(row);
    }
}

QStringList FolderModel::selectedPaths() const
{
    QList<int> rows(m_selected.cbegin(), m_selected.cend());
    std::sort(rows.begin(), rows.end());
    QStringList out;
    for (int row : rows) {
        if (row < m_items.size()) {
            out.append(m_items.at(row).path);
        }
    }
    return out;
}

QStringList FolderModel::selectedUrls() const
{
    QStringList out;
    for (const QString& path : selectedPaths()) {
        out.append(QUrl::fromLocalFile(path).toString());
    }
    return out;
}

QString FolderModel::urlAt(int row) const
{
    return row >= 0 && row < m_items.size() ? QUrl::fromLocalFile(m_items.at(row).path).toString() : QString();
}

void FolderModel::readClipboard()
{
    QSet<QString> cut;
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime && mime->hasUrls()
        && (mime->data(QStringLiteral("application/x-kde-cutselection")) == "1"
            || mime->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("cut"))) {
        for (const QUrl& url : mime->urls()) {
            if (url.isLocalFile()) {
                cut.insert(url.toLocalFile());
            }
        }
    }
    if (cut == m_cut) {
        return;
    }
    m_cut = cut;
    if (!m_items.isEmpty()) {
        emit dataChanged(index(0), index(int(m_items.size()) - 1), { CutRole });
    }
}
