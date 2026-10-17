// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktopmodel.h"

#include "appmodel.h"
#include "../../common/filetransferrequest.h"
#include "systemactions.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QImageReader>
#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QPointer>
#include <QThreadPool>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QtDebug>

#include <algorithm>
#include <cmath>

namespace {

constexpr qreal margin = 6.0; // from the output edge, like in Windows
const QString trashPath = QStringLiteral("trash:/");

QString trashFilesDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash/files");
}

QString positionKey(const QString& name)
{
    return QStringLiteral("desktop-positions/") + QString::fromLatin1(QUrl::toPercentEncoding(name));
}

// A .desktop shortcut's name and icon, in the user's language.
void readDesktopFile(const QString& path, QString& name, QString& icon)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QString locale = QLocale().name();
    const QStringList keys { QStringLiteral("Name[%1]").arg(locale),
        QStringLiteral("Name[%1]").arg(locale.section(u'_', 0, 0)), QStringLiteral("Name") };
    QHash<QString, QString> values;
    bool inMain = false;
    QTextStream stream(&file);
    QString line;
    while (stream.readLineInto(&line)) {
        if (line.startsWith(u'[')) {
            inMain = line.trimmed() == QLatin1String("[Desktop Entry]");
        } else if (inMain && line.contains(u'=')) {
            values.insert(line.section(u'=', 0, 0).trimmed(), line.section(u'=', 1).trimmed());
        }
    }
    for (const QString& key : keys) {
        if (!values.value(key).isEmpty()) {
            name = values.value(key);
            break;
        }
    }
    icon = values.value(QStringLiteral("Icon"), icon);
}

// Images (the shell makes the thumbnail) and files whose thumbnail another app
// already put in the shared cache (videos, PDFs...).
bool hasThumbnail(const QFileInfo& info, const QMimeType& mime)
{
    static const QList<QByteArray> readable = QImageReader::supportedMimeTypes();
    if (readable.contains(mime.name().toLatin1())) {
        return true;
    }
    const QByteArray uri = QUrl::fromLocalFile(info.absoluteFilePath()).toEncoded();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex())
        + QStringLiteral(".png");
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/thumbnails/");
    for (const char* dir : { "normal/", "large/", "x-large/" }) {
        if (QFileInfo::exists(root + QLatin1String(dir) + name)) {
            return true;
        }
    }
    return false;
}

bool runDetached(const QString& program, const QStringList& arguments, const QString& directory)
{
    qInfo("vela-shell: %s %s", qPrintable(program), qPrintable(arguments.join(u' ')));
    return QProcess::startDetached(program, arguments, directory);
}

// Both desktop Ctrl+V and drag/drop must go through FileOps in vela-files.
// A tracked Explorer window owns the transfer and stays open until completion;
// launching detached cp/mv loses progress, cancellation and error reporting.
bool launchFileTransfer(const QStringList& sources, const QString& directory, vela::files::TransferAction action)
{
    if (sources.isEmpty()) {
        return false;
    }
    QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("vela-files"));
    if (!QFileInfo(executable).isExecutable()) {
        executable = QStringLiteral("vela-files"); // development / separate install prefix
    }
    const bool launched = runDetached(executable, vela::files::transferArguments(action, directory, sources), directory);
    if (!launched) {
        qWarning("vela-shell: unable to start the tracked file transfer in vela-files");
    }
    return launched;
}

} // namespace

DesktopModel::DesktopModel(AppModel* apps, QObject* parent)
    : QAbstractListModel(parent)
    , m_apps(apps)
{
    m_dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QDir().mkpath(m_dir);

    QSettings settings;
    m_iconSize = std::clamp(settings.value(QStringLiteral("desktop/iconSize"), 1).toInt(), 0, 2);
    m_sortMode = std::clamp(settings.value(QStringLiteral("desktop/sortMode"), 0).toInt(), 0, 3);
    m_autoArrange = settings.value(QStringLiteral("desktop/autoArrange"), false).toBool();
    m_alignToGrid = settings.value(QStringLiteral("desktop/alignToGrid"), true).toBool();
    m_showIcons = settings.value(QStringLiteral("desktop/showIcons"), true).toBool();

    // Changes come in bursts (a copy, a save): reread only once, shortly
    // after.
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(100);
    connect(&m_reloadTimer, &QTimer::timeout, this, &DesktopModel::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    m_watcher.addPath(m_dir);
    QDir().mkpath(trashFilesDir());
    m_watcher.addPath(trashFilesDir()); // the Recycle Bin full or empty

    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &DesktopModel::clipboardChanged);
    reload();
}

int DesktopModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() || !m_showIcons ? 0 : int(m_items.size());
}

QVariant DesktopModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) {
        return {};
    }
    const Item& item = m_items.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case PathRole: return item.path;
    case UrlRole: return item.isTrash ? item.path : QUrl::fromLocalFile(item.path).toString();
    case IconRole: return item.icon;
    case IsDirRole: return item.isDir;
    case IsAppRole: return item.isApp;
    case IsTrashRole: return item.isTrash;
    case TypeRole: return item.type;
    case ThumbnailRole: return item.hasThumbnail;
    case ModifiedRole: return double(item.modified.isValid() ? item.modified.toSecsSinceEpoch() : 0);
    case XRole: return index.row() < m_layout.size() ? m_layout.at(index.row()).x() : 0.0;
    case YRole: return index.row() < m_layout.size() ? m_layout.at(index.row()).y() : 0.0;
    default: return {};
    }
}

QHash<int, QByteArray> DesktopModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { PathRole, "path" },
        { UrlRole, "url" },
        { IconRole, "iconName" },
        { IsDirRole, "isDir" },
        { IsAppRole, "isApp" },
        { IsTrashRole, "isTrash" },
        { TypeRole, "typeName" },
        { ModifiedRole, "modified" },
        { ThumbnailRole, "hasThumbnail" },
        { XRole, "cellX" },
        { YRole, "cellY" },
    };
}

// ------------------------------------------------------------ the view --

int DesktopModel::iconPixels() const
{
    static constexpr int sizes[] { 96, 48, 32 };
    return sizes[m_iconSize];
}

int DesktopModel::cellWidth() const
{
    static constexpr int widths[] { 124, 80, 76 };
    return widths[m_iconSize];
}

int DesktopModel::cellHeight() const
{
    static constexpr int heights[] { 142, 94, 78 };
    return heights[m_iconSize];
}

void DesktopModel::setIconSize(int size)
{
    size = std::clamp(size, 0, 2);
    if (size != m_iconSize) {
        m_iconSize = size;
        QSettings().setValue(QStringLiteral("desktop/iconSize"), size);
        emit viewChanged();
        layout();
    }
}

void DesktopModel::setAutoArrange(bool on)
{
    if (on == m_autoArrange) {
        return;
    }
    m_autoArrange = on;
    QSettings settings;
    settings.setValue(QStringLiteral("desktop/autoArrange"), on);
    if (on) {
        settings.remove(QStringLiteral("desktop-positions")); // all in a row
        for (Item& item : m_items) {
            item.position = QPointF(-1, -1);
        }
    }
    emit viewChanged();
    layout();
}

void DesktopModel::setAlignToGrid(bool on)
{
    if (on != m_alignToGrid) {
        m_alignToGrid = on;
        QSettings().setValue(QStringLiteral("desktop/alignToGrid"), on);
        emit viewChanged();
        layout();
    }
}

void DesktopModel::setShowIcons(bool on)
{
    if (on != m_showIcons) {
        beginResetModel();
        m_showIcons = on;
        endResetModel();
        QSettings().setValue(QStringLiteral("desktop/showIcons"), on);
        emit viewChanged();
    }
}

void DesktopModel::setAreaWidth(qreal width)
{
    if (!qFuzzyCompare(width, m_areaWidth)) {
        m_areaWidth = width;
        emit areaChanged();
        layout();
    }
}

void DesktopModel::setAreaHeight(qreal height)
{
    if (!qFuzzyCompare(height, m_areaHeight)) {
        m_areaHeight = height;
        emit areaChanged();
        layout();
    }
}

int DesktopModel::rows() const
{
    return std::max(1, int((m_areaHeight - 2 * margin) / cellHeight()));
}

QPointF DesktopModel::cellPosition(int column, int row) const
{
    return QPointF(margin + column * cellWidth(), margin + row * cellHeight());
}

void DesktopModel::sortBy(int mode)
{
    m_sortMode = std::clamp(mode, 0, 3);
    QSettings settings;
    settings.setValue(QStringLiteral("desktop/sortMode"), m_sortMode);
    settings.remove(QStringLiteral("desktop-positions")); // sorted: all in a row
    emit viewChanged();
    reload();
}

void DesktopModel::refresh()
{
    reload();
}

void DesktopModel::reload()
{
    QList<Item> items;
    static const QMimeDatabase mimes;

    // The Recycle Bin, always first, like in Windows.
    Item trash;
    trash.name = QCoreApplication::translate("Desktop", "Recycle Bin");
    trash.path = trashPath;
    trash.isTrash = true;
    trash.icon = trashEmpty() ? QStringLiteral("user-trash") : QStringLiteral("user-trash-full");
    trash.type = QCoreApplication::translate("Desktop", "Recycle Bin");

    QList<Item> files;
    const QFileInfoList entries = QDir(m_dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QFileInfo& info : entries) {
        Item item;
        item.name = info.fileName();
        item.path = info.absoluteFilePath();
        item.isDir = info.isDir();
        item.size = info.size();
        item.modified = info.lastModified();
        const QMimeType mime = mimes.mimeTypeForFile(info);
        item.type = mime.comment();
        item.icon = item.isDir ? QStringLiteral("folder") : mime.iconName();
        if (!item.isDir && !QIcon::hasThemeIcon(item.icon)) {
            item.icon = mime.genericIconName();
        }
        item.hasThumbnail = !item.isDir && hasThumbnail(info, mime);
        if (!item.isDir && info.suffix() == QLatin1String("desktop")) {
            item.isApp = true;
            item.name = info.completeBaseName();
            readDesktopFile(item.path, item.name, item.icon);
        }
        files.append(item);
    }

    // Sort by: name, size, type, date; folders before files.
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const int mode = m_sortMode;
    std::stable_sort(files.begin(), files.end(), [&](const Item& a, const Item& b) {
        if (a.isDir != b.isDir) {
            return a.isDir;
        }
        switch (mode) {
        case 1:
            if (a.size != b.size) {
                return a.size < b.size;
            }
            break;
        case 2:
            if (int c = collator.compare(a.type, b.type); c != 0) {
                return c < 0;
            }
            break;
        case 3:
            if (a.modified != b.modified) {
                return a.modified > b.modified;
            }
            break;
        default:
            break;
        }
        return collator.compare(a.name, b.name) < 0;
    });
    items.append(trash);
    items.append(files);

    // Positions chosen by hand (by file name).
    QSettings settings;
    for (Item& item : items) {
        const QVariant saved = settings.value(positionKey(item.isTrash ? item.path : QFileInfo(item.path).fileName()));
        item.position = saved.isValid() && !m_autoArrange ? saved.toPointF() : QPointF(-1, -1);
    }

    // Same files as before: data is updated without redoing the icons (a
    // rename in progress isn't lost).
    const bool sameFiles = items.size() == m_items.size()
        && std::equal(items.cbegin(), items.cend(), m_items.cbegin(),
            [](const Item& a, const Item& b) { return a.path == b.path; });
    if (sameFiles) {
        m_items = items;
        layout();
        if (!m_items.isEmpty() && m_showIcons) {
            emit dataChanged(index(0), index(int(m_items.size()) - 1));
        }
    } else {
        beginResetModel();
        m_items = items;
        layout(false);
        endResetModel();
    }

    if (!m_pendingSelect.isEmpty() && QFileInfo::exists(m_pendingSelect)) {
        const QString path = std::exchange(m_pendingSelect, QString());
        emit created(path, m_pendingRename);
    }
}

void DesktopModel::layout(bool notify)
{
    if (m_items.isEmpty() || m_areaHeight <= 0) {
        return;
    }
    const int rowCount = rows();
    const qreal cw = cellWidth();
    const qreal ch = cellHeight();
    QSet<qint64> used; // occupied cells: column * 1000 + row
    const auto cellOf = [&](const QPointF& p) {
        return std::pair(std::max(0, int(std::lround((p.x() - margin) / cw))),
            std::clamp(int(std::lround((p.y() - margin) / ch)), 0, rowCount - 1));
    };
    // The free cell closest to the wanted one.
    const auto nearestFree = [&](int column, int row) {
        for (int radius = 0; radius < 200; ++radius) {
            for (int dc = -radius; dc <= radius; ++dc) {
                for (int dr = -radius; dr <= radius; ++dr) {
                    if (std::max(std::abs(dc), std::abs(dr)) != radius) {
                        continue;
                    }
                    const int c = column + dc;
                    const int r = row + dr;
                    if (c >= 0 && r >= 0 && r < rowCount && !used.contains(qint64(c) * 1000 + r)) {
                        return std::pair(c, r);
                    }
                }
            }
        }
        return std::pair(column, row);
    };

    QList<QPointF> positions(m_items.size(), QPointF(-1, -1));
    // Those placed by hand first, then the others in a row by columns.
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        const QPointF saved = m_items.at(i).position;
        if (saved.x() < 0) {
            continue;
        }
        if (m_alignToGrid) {
            const auto [c, r] = cellOf(saved);
            const auto [fc, fr] = nearestFree(c, r);
            used.insert(qint64(fc) * 1000 + fr);
            positions[i] = cellPosition(fc, fr);
        } else {
            const auto [c, r] = cellOf(saved);
            used.insert(qint64(c) * 1000 + r);
            positions[i] = QPointF(std::clamp(saved.x(), 0.0, std::max(0.0, m_areaWidth - cw)),
                std::clamp(saved.y(), 0.0, std::max(0.0, m_areaHeight - ch)));
        }
    }
    int column = 0;
    int row = 0;
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        if (positions[i].x() >= 0) {
            continue;
        }
        while (used.contains(qint64(column) * 1000 + row)) {
            if (++row >= rowCount) {
                row = 0;
                ++column;
            }
        }
        used.insert(qint64(column) * 1000 + row);
        positions[i] = cellPosition(column, row);
    }

    // The positions to show; the saved ones stay in m_items as a request.
    m_layout = positions;
    if (notify && m_showIcons) {
        emit dataChanged(index(0), index(int(m_items.size()) - 1), { XRole, YRole });
    }
}

void DesktopModel::savePosition(const QString& name, const QPointF& position)
{
    QSettings().setValue(positionKey(name), position);
}

void DesktopModel::moveTo(const QString& path, qreal x, qreal y)
{
    if (m_autoArrange) {
        layout(); // back in a row
        return;
    }
    for (Item& item : m_items) {
        if (item.path == path) {
            item.position = QPointF(std::max(0.0, x), std::max(0.0, y));
            savePosition(item.isTrash ? item.path : QFileInfo(item.path).fileName(), item.position);
            break;
        }
    }
    layout();
    // With the grid the saved position becomes the chosen cell's.
    for (qsizetype i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).path == path && m_alignToGrid) {
            m_items[i].position = m_layout.at(i);
            savePosition(m_items.at(i).isTrash ? path : QFileInfo(path).fileName(), m_layout.at(i));
        }
    }
}

// ----------------------------------------------------------- files --

void DesktopModel::open(const QStringList& paths)
{
    for (const QString& path : paths) {
        if (path == trashPath) {
            runDetached(velaFilesExecutable(), { trashFilesDir() }, m_dir);
        } else if (path.endsWith(QLatin1String(".desktop")) && m_apps->launchDesktopFile(path)) {
            continue;
        } else {
            const QFileInfo info(path);
            if (info.isDir()) {
                const QString files = velaFilesExecutable();
                if (files.isEmpty() || !runDetached(files, { path }, m_dir))
                    qWarning("vela-shell: couldn't open folder %s in Vela Files", qPrintable(path));
            } else if (info.isFile()) {
                const QString app = defaultAppFor(QMimeDatabase().mimeTypeForFile(info).name());
                if (app.isEmpty() || !m_apps->launchWithFile(app, QUrl::fromLocalFile(path).toString()))
                    emit chooseAppRequested(path);
            } else {
                qWarning("vela-shell: no handler for %s", qPrintable(path));
            }
        }
    }
}

QString DesktopModel::uniqueName(const QString& name) const
{
    return uniqueNameIn(m_dir, name);
}

QString DesktopModel::uniqueNameIn(const QString& directory, const QString& name)
{
    const QDir dir(directory);
    if (!dir.exists(name)) {
        return name;
    }
    // "New folder (2)", "photo (2).jpg", like in Windows.
    const QFileInfo info(name);
    const bool hasSuffix = !info.suffix().isEmpty() && !info.completeBaseName().isEmpty();
    const QString base = hasSuffix ? info.completeBaseName() : name;
    const QString suffix = hasSuffix ? u'.' + info.suffix() : QString();
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(suffix);
        if (!dir.exists(candidate)) {
            return candidate;
        }
    }
}

bool DesktopModel::rename(const QString& path, const QString& newName)
{
    const QString name = newName.trimmed();
    const QFileInfo info(path);
    if (name.isEmpty() || name.contains(u'/') || name == QLatin1String(".") || name == QLatin1String("..")
        || name == info.fileName()) {
        return false;
    }
    const QString target = info.absoluteDir().filePath(name);
    if (QFileInfo::exists(target) || !QFile::rename(path, target)) {
        return false;
    }
    // The position follows the file.
    QSettings settings;
    const QVariant position = settings.value(positionKey(info.fileName()));
    if (position.isValid()) {
        settings.remove(positionKey(info.fileName()));
        settings.setValue(positionKey(name), position);
    }
    UndoStep step;
    step.label = QCoreApplication::translate("Desktop", "Rename");
    step.moves.append({ target, path });
    pushUndo(step);
    reload();
    return true;
}

void DesktopModel::trash(const QStringList& paths)
{
    UndoStep step;
    step.label = QCoreApplication::translate("Desktop", "Delete");
    for (const QString& path : paths) {
        if (path == trashPath) {
            continue;
        }
        QString inTrash;
        if (QFile::moveToTrash(path, &inTrash)) {
            step.trashed.append(inTrash);
            step.originals.append(path);
        } else {
            qWarning("vela-shell: can't move %s to the Recycle Bin", qPrintable(path));
        }
    }
    if (!step.trashed.isEmpty()) {
        pushUndo(step);
    }
    reload();
}

bool DesktopModel::trashEmpty() const
{
    return QDir(trashFilesDir()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
}

void DesktopModel::emptyTrash()
{
    QPointer<DesktopModel> self(this);
    QThreadPool::globalInstance()->start([self] {
        const QStringList failed = vela::trash::emptyHome();
        if (!failed.isEmpty()) {
            qWarning("vela-shell: could not empty Recycle Bin entries: %s", qPrintable(failed.join(u", ")));
        }
        QMetaObject::invokeMethod(qApp, [self] {
            if (self) self->reload();
        }, Qt::QueuedConnection);
    });
}

void DesktopModel::pushUndo(UndoStep step)
{
    m_undo.append(std::move(step));
    if (m_undo.size() > 20) {
        m_undo.removeFirst();
    }
    emit undoChanged();
}

QString DesktopModel::undoText() const
{
    return m_undo.isEmpty() ? QString() : m_undo.last().label;
}

void DesktopModel::undo()
{
    if (m_undo.isEmpty()) {
        return;
    }
    const UndoStep step = m_undo.takeLast();
    emit undoChanged();
    for (const auto& [current, original] : step.moves) {
        if (!QFileInfo::exists(original)) {
            QFile::rename(current, original);
        }
    }
    for (const QString& path : step.created) {
        QFile::moveToTrash(path);
    }
    for (qsizetype i = 0; i < step.trashed.size(); ++i) {
        // From the Recycle Bin (…/Trash/files/name) back to its place, without
        // the record in …/Trash/info.
        const QString inTrash = step.trashed.at(i);
        if (!QFileInfo::exists(step.originals.at(i)) && QFile::rename(inTrash, step.originals.at(i))) {
            const QFileInfo info(inTrash);
            QFile::remove(info.absoluteDir().absoluteFilePath(
                QStringLiteral("../info/") + info.fileName() + QStringLiteral(".trashinfo")));
        }
    }
    reload();
}

QString DesktopModel::createFolder()
{
    const QString name = uniqueName(QCoreApplication::translate("Desktop", "New folder"));
    const QString path = QDir(m_dir).filePath(name);
    if (!QDir(m_dir).mkdir(name)) {
        return {};
    }
    UndoStep step;
    step.label = QCoreApplication::translate("Desktop", "New");
    step.created.append(path);
    pushUndo(step);
    m_pendingSelect = path;
    m_pendingRename = true;
    reload();
    return path;
}

QString DesktopModel::createFile(const QString& templatePath)
{
    QString name;
    if (templatePath.isEmpty()) {
        name = uniqueName(QCoreApplication::translate("Desktop", "New Text Document.txt"));
    } else {
        name = uniqueName(QFileInfo(templatePath).fileName());
    }
    const QString path = QDir(m_dir).filePath(name);
    bool ok = false;
    if (templatePath.isEmpty()) {
        QFile file(path);
        ok = file.open(QIODevice::WriteOnly);
    } else {
        ok = QFile::copy(templatePath, path);
    }
    if (!ok) {
        return {};
    }
    UndoStep step;
    step.label = QCoreApplication::translate("Desktop", "New");
    step.created.append(path);
    pushUndo(step);
    m_pendingSelect = path;
    m_pendingRename = true;
    reload();
    return path;
}

QVariantList DesktopModel::templates() const
{
    QVariantList out;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation);
    if (dir.isEmpty() || QDir(dir) == QDir::home()) {
        return out;
    }
    static const QMimeDatabase mimes;
    for (const QFileInfo& info : QDir(dir).entryInfoList(QDir::Files, QDir::Name)) {
        out.append(QVariantMap { { QStringLiteral("name"), info.completeBaseName() },
            { QStringLiteral("icon"), mimes.mimeTypeForFile(info).iconName() },
            { QStringLiteral("path"), info.absoluteFilePath() } });
    }
    return out;
}

// --------------------------------------------------------- clipboard --

namespace {

void putFiles(const QStringList& paths, bool cut)
{
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString& path : paths) {
        if (path == trashPath) {
            continue;
        }
        const QUrl url = QUrl::fromLocalFile(path);
        urls.append(url);
        gnome += '\n' + url.toEncoded();
    }
    if (urls.isEmpty()) {
        return;
    }
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    // Like Dolphin and Nautilus: they say whether it's "cut" or "copy".
    mime->setData(QStringLiteral("application/x-kde-cutselection"), cut ? "1" : "0");
    mime->setData(QStringLiteral("x-special/gnome-copied-files"), gnome);
    QGuiApplication::clipboard()->setMimeData(mime);
}

} // namespace

void DesktopModel::copy(const QStringList& paths)
{
    putFiles(paths, false);
}

void DesktopModel::cut(const QStringList& paths)
{
    putFiles(paths, true);
}

bool DesktopModel::canPaste() const
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        return false;
    }
    const QList<QUrl> urls = mime->urls();
    return std::any_of(urls.cbegin(), urls.cend(), [](const QUrl& url) { return url.isLocalFile(); });
}

void DesktopModel::paste()
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls()) {
        return;
    }
    const bool cut = mime->data(QStringLiteral("application/x-kde-cutselection")) == "1"
        || mime->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("cut");
    QStringList sources;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile() || !QFileInfo::exists(url.toLocalFile())) {
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        if (cut && info.absoluteDir() == QDir(m_dir)) {
            continue; // already on the desktop
        }
        sources.append(info.absoluteFilePath());
    }
    if (launchFileTransfer(sources, m_dir,
            cut ? vela::files::TransferAction::Move : vela::files::TransferAction::Copy)
        && cut) {
        QGuiApplication::clipboard()->clear(); // only clear if handed to vela-files
    }
}

void DesktopModel::copyAsPath(const QStringList& paths)
{
    QStringList quoted;
    for (const QString& path : paths) {
        if (path != trashPath) {
            quoted.append(u'"' + path + u'"');
        }
    }
    QGuiApplication::clipboard()->setText(quoted.join(u'\n'));
}

void DesktopModel::compress(const QStringList& paths, const QString& format)
{
    QStringList names;
    for (const QString& path : paths) {
        if (path != trashPath) {
            names.append(QFileInfo(path).fileName());
        }
    }
    if (names.isEmpty()) {
        return;
    }
    // The archive takes the name of the first item, like in Windows.
    const QFileInfo first(paths.first());
    const QString base = first.isDir() ? first.fileName() : first.completeBaseName();
    const QString archive = uniqueName(base + u'.' + format);
    // bsdtar picks the format from the extension (-a): zip, 7z, tar.
    QStringList arguments { QStringLiteral("-a"), QStringLiteral("-cf"), archive, QStringLiteral("--") };
    arguments.append(names);
    if (runDetached(QStringLiteral("bsdtar"), arguments, m_dir)) {
        UndoStep step;
        step.label = QCoreApplication::translate("Desktop", "Compress");
        step.created.append(QDir(m_dir).filePath(archive));
        pushUndo(step);
        m_pendingSelect = QDir(m_dir).filePath(archive);
        m_pendingRename = false;
    }
}

// ----------------------------------------------------------- dragging --

// Drag/drop and clipboard paste share exactly the same FileOps transport.
void DesktopModel::drop(const QStringList& urls, const QString& target, qreal x, qreal y)
{
    QStringList sources;
    for (const QString& text : urls) {
        const QUrl url(text);
        if (url.isLocalFile() && QFileInfo::exists(url.toLocalFile())) {
            sources.append(QFileInfo(url.toLocalFile()).absoluteFilePath());
        }
    }
    if (sources.isEmpty()) {
        return;
    }
    if (target == trashPath) {
        trash(sources);
        return;
    }

    const QString dir = target.isEmpty() ? m_dir : target;
    if (!QFileInfo(dir).isDir()) {
        qWarning("vela-shell: drop target is not a directory");
        return;
    }
    QStringList valid;
    for (const QString& source : std::as_const(sources)) {
        // No self-drops or recursive copies. Same-folder drops are moves,
        // hence no-ops; duplicating there is a clipboard Copy + Paste.
        if (source == dir || dir.startsWith(source + u'/')
            || QFileInfo(source).absolutePath() == QDir(dir).absolutePath()) {
            continue;
        }
        valid.append(source);
    }
    if (!launchFileTransfer(valid, dir, vela::files::TransferAction::Automatic)) {
        return;
    }
    // Retain requested icon placement for conflict-free drops onto the desktop.
    // FileOps will handle collisions itself via the common conflict dialog.
    if (target.isEmpty()) {
        int placed = 0;
        for (const QString& source : std::as_const(valid)) {
            const QString name = QFileInfo(source).fileName();
            if (QFileInfo::exists(QDir(dir).filePath(name))) {
                continue;
            }
            if (!m_autoArrange) {
                savePosition(name, QPointF(std::max(0.0, x), std::max(0.0, y + placed * cellHeight())));
                ++placed;
            }
            if (m_pendingSelect.isEmpty()) {
                m_pendingSelect = QDir(dir).filePath(name);
                m_pendingRename = false;
            }
        }
    }
}
