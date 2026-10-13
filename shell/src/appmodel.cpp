// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appmodel.h"
#include "desktopexec.h"
#include "mimeapps.h"
#include "velatool.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QtDebug>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

// The escapes the Desktop Entry spec provides for string values.
QString unescape(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c != u'\\' || i + 1 >= value.size()) {
            out.append(c);
            continue;
        }
        const QChar next = value.at(++i);
        switch (next.unicode()) {
        case 's': out.append(u' '); break;
        case 'n': out.append(u'\n'); break;
        case 't': out.append(u'\t'); break;
        case 'r': out.append(u'\r'); break;
        case '\\': out.append(u'\\'); break;
        default:
            out.append(u'\\');
            out.append(next);
            break;
        }
    }
    return out;
}

// Removes the "field codes" (%f, %U, ...) that are only needed when opening
// files.
QString cleanExec(const QString& exec)
{
    static const QRegularExpression fieldCodes(QStringLiteral("%[fFuUdDnNickvm]"));
    QString result = exec;
    result.remove(fieldCodes);
    result.replace(QStringLiteral("%%"), QStringLiteral("%"));
    return result.simplified();
}

bool listContains(const QString& list, const QString& item)
{
    const auto parts = list.split(u';', Qt::SkipEmptyParts);
    return std::any_of(parts.begin(), parts.end(), [&](const QString& part) {
        return part.compare(item, Qt::CaseInsensitive) == 0;
    });
}

// Reads a .desktop file. Returns false if it must not show in the menu.
bool parseDesktopFile(const QString& path, const QStringList& localeKeys, AppModel::Entry& entry)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    // Each group ([Desktop Entry], [Desktop Action new-window], ...) with its
    // keys.
    QHash<QString, QHash<QString, QString>> groups;
    QString group;
    QTextStream stream(&file);
    QString line;
    while (stream.readLineInto(&line)) {
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        if (line.startsWith(u'[')) {
            group = line.trimmed().mid(1).chopped(1);
            continue;
        }
        const qsizetype eq = line.indexOf(u'=');
        if (eq <= 0 || group.isEmpty()) {
            continue;
        }
        groups[group].insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    const QHash<QString, QString> values = groups.value(QStringLiteral("Desktop Entry"));

    if (values.value(QStringLiteral("Type")) != QLatin1String("Application")) {
        return false;
    }
    const auto isTrue = [&](const char* key) {
        return values.value(QLatin1String(key)).compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    };
    if (isTrue("NoDisplay") || isTrue("Hidden")) {
        return false;
    }
    const QString onlyShowIn = values.value(QStringLiteral("OnlyShowIn"));
    if (!onlyShowIn.isEmpty() && !listContains(onlyShowIn, QStringLiteral("Vela"))
        && !listContains(onlyShowIn, QStringLiteral("KDE"))) {
        return false;
    }
    if (listContains(values.value(QStringLiteral("NotShowIn")), QStringLiteral("Vela"))) {
        return false;
    }
    const QString tryExec = values.value(QStringLiteral("TryExec"));
    if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty()
        && !QFile::exists(tryExec)) {
        return false;
    }

    // The translated version first (Name[it_IT], Name[it]), then the base one.
    const auto localizedIn = [&](const QHash<QString, QString>& values, const QString& key) {
        for (const QString& locale : localeKeys) {
            const auto it = values.constFind(QStringLiteral("%1[%2]").arg(key, locale));
            if (it != values.constEnd()) {
                return unescape(*it);
            }
        }
        return unescape(values.value(key));
    };
    const auto localized = [&](const QString& key) { return localizedIn(values, key); };

    entry.name = localized(QStringLiteral("Name"));
    entry.genericName = localized(QStringLiteral("GenericName"));
    entry.comment = localized(QStringLiteral("Comment"));
    entry.keywords = localized(QStringLiteral("Keywords"));
    entry.icon = values.value(QStringLiteral("Icon"));
    entry.rawExec = unescape(values.value(QStringLiteral("Exec")));
    entry.exec = cleanExec(entry.rawExec);
    if (const DesktopExec::Parsed parsed = DesktopExec::split(entry.rawExec); parsed.ok) {
        entry.program = parsed.arguments.first().section(u'/', -1);
    }
    entry.workDir = unescape(values.value(QStringLiteral("Path")));
    entry.path = path;
    // The app's actions (the jump list "tasks"), in the given order.
    for (const QString& id : values.value(QStringLiteral("Actions")).split(u';', Qt::SkipEmptyParts)) {
        const QHash<QString, QString> action = groups.value(QStringLiteral("Desktop Action ") + id);
        AppModel::Action a { id, localizedIn(action, QStringLiteral("Name")), action.value(QStringLiteral("Icon")),
            unescape(action.value(QStringLiteral("Exec"))) };
        if (!a.name.isEmpty() && !a.exec.isEmpty()) {
            entry.actions.append(a);
        }
    }
    entry.wmClass = values.value(QStringLiteral("StartupWMClass"));
    entry.mimeTypes = values.value(QStringLiteral("MimeType")).split(u';', Qt::SkipEmptyParts);
    entry.terminal = isTrue("Terminal");
    return !entry.name.isEmpty() && !entry.exec.isEmpty();
}

} // namespace

AppModel::AppModel(QObject* parent)
    : QAbstractListModel(parent)
{
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(250);
    connect(&m_reloadTimer, &QTimer::timeout, this, &AppModel::reload);
    m_watchRetryTimer.setSingleShot(true);
    m_watchRetryTimer.setInterval(10000);
    connect(&m_watchRetryTimer, &QTimer::timeout, this, &AppModel::reload);
    // Watch the .desktop files themselves too: changing Name= or Hidden=
    // in place need not generate a directoryChanged event on Linux.
    const auto changed = [this] { m_reloadTimer.start(); };
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, changed);
}

void AppModel::updateWatchPaths(const QStringList& directories, const QSet<QString>& desktopFiles)
{
    QSet<QString> wanted;
    QHash<QString, QString> symlinkTargets;
    QSet<QString> reattachRoots;
    for (const QString& directory : directories) {
        const QString root = QDir::cleanPath(directory);
        const QFileInfo rootInfo(root);
        if (rootInfo.isSymLink()) {
            // Linux inotify follows symlinks. The lexical path can remain
            // unchanged while the watched inode switches to a new target.
            // Watch the link's parent to receive the replacement event.
            wanted.insert(rootInfo.absolutePath());
            const QString target = rootInfo.canonicalFilePath();
            symlinkTargets.insert(root, target);
            const auto previous = m_symlinkTargets.constFind(root);
            if (previous != m_symlinkTargets.cend() && *previous != target) {
                reattachRoots.insert(root);
            }
        }
        QString existing = root;
        // Some XDG application directories don't exist until the first app
        // is installed (notably per-user apps and Flatpak exports). Watching
        // the closest existing ancestor detects their creation.
        while (!QFileInfo(existing).isDir()) {
            const QString parent = QFileInfo(existing).absolutePath();
            if (parent == existing) {
                break;
            }
            existing = parent;
        }
        if (!QFileInfo(existing).isDir()) {
            continue;
        }
        wanted.insert(existing);
        if (existing != root) {
            continue;
        }
        // QFileSystemWatcher isn't recursive. Watch every subdirectory so
        // that new desktop files in nested application folders are detected.
        QDirIterator subdirs(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
            QDirIterator::Subdirectories);
        while (subdirs.hasNext()) {
            wanted.insert(subdirs.next());
        }
    }
    m_symlinkTargets = std::move(symlinkTargets);

    // Directory watches cover creations, removals and atomic replacements;
    // per-file watches cover edits that don't change directory metadata.
    wanted.unite(desktopFiles);

    // Reattach every watcher below a replaced symlink root. The paths of
    // nested directories and files are also unchanged, yet Qt may still be
    // watching inodes in the old target tree.
    const auto needsReattach = [&reattachRoots](const QString& path) {
        for (const QString& root : reattachRoots) {
            if (path == root || path.startsWith(root + u'/')) {
                return true;
            }
        }
        return false;
    };
    const QStringList watchedPaths = m_watcher.directories() + m_watcher.files();
    QSet<QString> watched;
    for (const QString& path : watchedPaths) {
        watched.insert(path);
    }
    QStringList remove;
    for (const QString& path : watchedPaths) {
        if (!wanted.contains(path) || needsReattach(path)) {
            remove.append(path);
        }
    }
    if (!remove.isEmpty()) {
        m_watcher.removePaths(remove);
    }
    QStringList add;
    for (const QString& path : std::as_const(wanted)) {
        if (!watched.contains(path) || needsReattach(path)) {
            add.append(path);
        }
    }
    // addPaths() reports rejected paths rather than throwing. A retry reload
    // also discovers installs that occurred while a required watch was absent.
    const QStringList failed = add.isEmpty() ? QStringList() : m_watcher.addPaths(add);
    QSet<QString> failedPaths;
    for (const QString& path : failed) {
        failedPaths.insert(path);
    }
    if (failedPaths != m_failedWatchPaths) {
        if (!failed.isEmpty()) {
            qWarning().noquote() << "vela-shell: cannot watch application paths:" << failed.join(", ");
        }
        m_failedWatchPaths = std::move(failedPaths);
    }
    if (m_failedWatchPaths.isEmpty()) {
        m_watchRetryTimer.stop();
    } else {
        m_watchRetryTimer.start();
    }
}

void AppModel::reload()
{
    QElapsedTimer timer;
    timer.start();

    const QString localeName = QLocale().name(); // such as it_IT
    QStringList localeKeys { localeName };
    if (const qsizetype underscore = localeName.indexOf(u'_'); underscore > 0) {
        localeKeys << localeName.left(underscore);
    }

    // The directories are in priority order: the first occurrence of an id
    // wins (such as ~/.local/share/applications overriding /usr/share/...).
    QList<Entry> entries;
    QSet<QString> seen;
    QSet<QString> desktopFiles;
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const QString& dir : dirs) {
        QDirIterator it(dir, { QStringLiteral("*.desktop") }, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            desktopFiles.insert(path);
            // The id includes the subdirectories joined by '-' (Desktop Entry
            // spec).
            QString id = QDir(dir).relativeFilePath(path);
            id.replace(u'/', u'-');
            if (seen.contains(id)) {
                continue;
            }
            seen.insert(id); // even if hidden: a local copy can hide the system one
            Entry entry;
            if (parseDesktopFile(path, localeKeys, entry)) {
                entry.id = id;
                entries.append(std::move(entry));
            }
        }
    }

    updateWatchPaths(dirs, desktopFiles);

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return collator.compare(a.name, b.name) < 0;
    });

    const int previousCount = count();
    beginResetModel();
    m_all = std::move(entries);
    m_visible = filteredIndices();
    endResetModel();
    if (previousCount != count()) {
        emit countChanged();
    }
    ++m_catalogRevision;
    emit catalogRevisionChanged();

    qInfo("vela-shell: %lld applicazioni caricate in %lld ms",
        static_cast<long long>(m_all.size()), static_cast<long long>(timer.elapsed()));
}

void AppModel::setQuery(const QString& query)
{
    if (query == m_query) {
        return;
    }
    m_query = query;
    emit queryChanged();
    applyFilter();
}

QList<int> AppModel::filteredIndices() const
{
    const QString q = m_query.trimmed();
    QList<int> visible;

    if (q.isEmpty()) {
        visible.reserve(m_all.size());
        for (int i = 0; i < m_all.size(); ++i) {
            visible.append(i);
        }
    } else {
        // A simple score: first those starting with the searched text, then
        // those with a word starting that way, then matches in descriptions
        // and keywords.
        QList<QPair<int, int>> scored; // (score, index)
        for (int i = 0; i < m_all.size(); ++i) {
            const Entry& e = m_all.at(i);
            int score = 0;
            if (e.name.startsWith(q, Qt::CaseInsensitive)) {
                score = 100;
            } else if (e.name.contains(QStringLiteral(" ") + q, Qt::CaseInsensitive)) {
                score = 80;
            } else if (e.name.contains(q, Qt::CaseInsensitive)) {
                score = 60;
            } else if (e.genericName.contains(q, Qt::CaseInsensitive)
                || e.keywords.contains(q, Qt::CaseInsensitive)) {
                score = 40;
            } else if (e.comment.contains(q, Qt::CaseInsensitive)
                || e.exec.contains(q, Qt::CaseInsensitive)) {
                score = 20;
            }
            if (score > 0) {
                scored.append({ score, i });
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
            return a.first > b.first;
        });
        visible.reserve(scored.size());
        for (const auto& [score, index] : scored) {
            visible.append(index);
        }
    }

    return visible;
}

void AppModel::applyFilter()
{
    QList<int> visible = filteredIndices();
    if (visible == m_visible) {
        return;
    }
    beginResetModel();
    m_visible = std::move(visible);
    endResetModel();
    emit countChanged();
}

int AppModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant AppModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size()) {
        return {};
    }
    const Entry& e = m_all.at(m_visible.at(index.row()));
    switch (role) {
    case IdRole: return e.id;
    case Qt::DisplayRole:
    case NameRole: return e.name;
    case IconRole: return e.icon;
    case CommentRole: return e.comment.isEmpty() ? e.genericName : e.comment;
    default: return {};
    }
}

QHash<int, QByteArray> AppModel::roleNames() const
{
    return {
        { IdRole, "appId" },
        { NameRole, "name" },
        { IconRole, "iconName" },
        { CommentRole, "comment" },
    };
}

bool AppModel::launch(int row)
{
    if (row < 0 || row >= m_visible.size()) {
        return false;
    }
    return launchEntry(m_all.at(m_visible.at(row)));
}

bool AppModel::launchId(const QString& id)
{
    for (const Entry& e : std::as_const(m_all)) {
        if (e.id == id) {
            return launchEntry(e);
        }
    }
    return false;
}

QVariantMap AppModel::entry(const QString& id) const
{
    for (const Entry& e : m_all) {
        if (e.id == id) {
            return { { QStringLiteral("name"), e.name }, { QStringLiteral("iconName"), e.icon } };
        }
    }
    return {};
}

QString AppModel::findDesktopId(const QString& appId) const
{
    if (appId.isEmpty()) {
        return {};
    }
    const auto baseId = [](const Entry& e) { return e.id.chopped(8); }; // without ".desktop"
    const auto program = [](const Entry& e) { return e.program; };
    // From the most reliable rule to the roughest. Modern apps use their
    // .desktop file name as app_id.
    const std::function<bool(const Entry&)> rules[] = {
        [&](const Entry& e) { return baseId(e).compare(appId, Qt::CaseInsensitive) == 0; },
        [&](const Entry& e) { return e.wmClass.compare(appId, Qt::CaseInsensitive) == 0; },
        [&](const Entry& e) {
            return baseId(e).section(u'.', -1).compare(appId, Qt::CaseInsensitive) == 0;
        },
        [&](const Entry& e) { return program(e).compare(appId, Qt::CaseInsensitive) == 0; },
    };
    for (const auto& matches : rules) {
        for (const Entry& e : m_all) {
            if (matches(e)) {
                return e.id;
            }
        }
    }
    return {};
}

QString AppModel::iconForAppId(const QString& appId) const
{
    const QString desktopId = findDesktopId(appId);
    const QString icon = desktopId.isEmpty() ? QString() : entry(desktopId).value(QStringLiteral("iconName")).toString();
    return icon.isEmpty() ? appId : icon; // the theme often has an icon named after the app_id
}

const AppModel::Entry* AppModel::find(const QString& id) const
{
    for (const Entry& e : m_all) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

QVariantList AppModel::actions(const QString& id) const
{
    QVariantList out;
    if (const Entry* e = find(id)) {
        for (const Action& a : e->actions) {
            out.append(QVariantMap { { QStringLiteral("id"), a.id }, { QStringLiteral("name"), a.name },
                { QStringLiteral("icon"), a.icon.isEmpty() ? e->icon : a.icon } });
        }
    }
    return out;
}

bool AppModel::launchAction(const QString& id, const QString& actionId)
{
    const Entry* e = find(id);
    if (!e) {
        return false;
    }
    for (const Action& a : e->actions) {
        if (a.id == actionId) {
            Entry copy = *e;
            copy.rawExec = a.exec;
            return launchEntry(copy);
        }
    }
    return false;
}

bool AppModel::launchWithFile(const QString& id, const QString& url)
{
    const Entry* e = find(id);
    if (!e) {
        return false;
    }
    // The file takes the place of the field codes (%f, %u...); if the app has
    // none, it goes at the end of the arguments (see launchEntry).
    return launchEntry(*e, { QUrl(url) });
}

// The default app for a file type, from mimeapps.list (mimeapps.h).
QString defaultAppFor(const QString& mime)
{
    return MimeApps::defaultFor(mime);
}

QVariantList AppModel::allApps() const
{
    QVariantList out;
    for (const Entry& entry : m_all) {
        out.append(QVariantMap { { QStringLiteral("id"), entry.id }, { QStringLiteral("name"), entry.name },
            { QStringLiteral("icon"), entry.icon } });
    }
    return out;
}

QVariantList AppModel::appsForFile(const QString& path) const
{
    static const QMimeDatabase mimes;
    const QMimeType type = mimes.mimeTypeForFile(path);
    QStringList accepted { type.name() };
    accepted << type.allAncestors();
    accepted << type.aliases();
    const QString preferred = defaultAppFor(type.name());

    QVariantList out;
    QSet<QString> names;
    for (const Entry& e : m_all) {
        const bool opens = std::any_of(e.mimeTypes.cbegin(), e.mimeTypes.cend(),
            [&](const QString& m) { return accepted.contains(m); });
        if (!opens || names.contains(e.name)) {
            continue;
        }
        names.insert(e.name);
        const QVariantMap app { { QStringLiteral("id"), e.id }, { QStringLiteral("name"), e.name },
            { QStringLiteral("icon"), e.icon }, { QStringLiteral("isDefault"), e.id == preferred } };
        if (e.id == preferred) {
            out.prepend(app);
        } else {
            out.append(app);
        }
    }
    return out;
}

bool AppModel::launchDesktopFile(const QString& path) const
{
    const QString localeName = QLocale().name();
    QStringList localeKeys { localeName, localeName.section(u'_', 0, 0) };
    Entry entry;
    if (!parseDesktopFile(path, localeKeys, entry)) {
        return false;
    }
    return launchEntry(entry);
}

QString AppModel::idForDesktopFile(const QString& path) const
{
    const QString id = QFileInfo(path).fileName();
    return find(id) ? id : QString();
}

QString AppModel::desktopFile(const QString& id) const
{
    const Entry* e = find(id);
    return e ? e->path : QString();
}

QString AppModel::name(const QString& id) const
{
    const Entry* e = find(id);
    return e ? e->name : QString();
}

QString AppModel::program(const QString& id) const
{
    const Entry* e = find(id);
    return e ? e->program : QString();
}

bool AppModel::launchEntry(const Entry& entry, const QList<QUrl>& files) const
{
    const DesktopExec::Parsed parsed = DesktopExec::split(entry.rawExec);
    if (!parsed.ok) {
        qWarning("vela-shell: invalid Exec in %s: %s", qPrintable(entry.path), qPrintable(entry.rawExec));
        return false;
    }
    const DesktopExec::Context context { entry.icon, entry.name, entry.path };
    // %f or %u with several files: one process per file. An app without field
    // codes still gets the files at the end, as before ("Open with").
    QList<QList<QUrl>> runs { files };
    if (files.size() > 1 && DesktopExec::onePerFile(parsed.arguments)) {
        runs.clear();
        for (const QUrl& file : files) {
            runs.append({ file });
        }
    }
    const QString directory = !entry.workDir.isEmpty() && QFileInfo(entry.workDir).isDir() ? entry.workDir : QDir::homePath();
    bool ok = true;
    for (const QList<QUrl>& run : std::as_const(runs)) {
        QStringList arguments = DesktopExec::expand(parsed.arguments, run, context);
        if (!DesktopExec::takesFiles(parsed.arguments)) {
            for (const QUrl& file : run) {
                arguments.append(DesktopExec::asPath(file));
            }
        }
        // The installed .desktop files use bare names: nested development
        // sessions should launch local Vela binaries rather than old packages.
        // Resolve after field-code expansion, but before terminal wrapping.
        if (!arguments.isEmpty()) {
            arguments[0] = vela::desktopExecutable(arguments.first(), QCoreApplication::applicationDirPath());
        }
        if (entry.terminal) {
            arguments = QStringList { qEnvironmentVariable("VELA_TERMINAL", QStringLiteral("konsole")),
                            QStringLiteral("-e") }
                + arguments;
        }
        if (arguments.isEmpty()) {
            return false;
        }
        qInfo("vela-shell: starting %s", qPrintable(arguments.join(u' ')));
        ok = QProcess::startDetached(arguments.first(), arguments.mid(1), directory) && ok;
    }
    return ok;
}
