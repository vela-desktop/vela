// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "processlaunch.h"
#include "servicemenus.h"
#include "desktopexec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QMimeDatabase>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QtDebug>

namespace {

using Group = QHash<QString, QString>;

QHash<QString, Group> readGroups(const QString& path)
{
    QHash<QString, Group> groups;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return groups;
    }
    QString group;
    QTextStream stream(&file);
    QString line;
    while (stream.readLineInto(&line)) {
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        if (line.startsWith(u'[')) {
            group = line.trimmed().mid(1).chopped(1);
        } else if (const qsizetype eq = line.indexOf(u'='); eq > 0 && !group.isEmpty()) {
            groups[group].insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
        }
    }
    return groups;
}

QString localized(const Group& group, const QString& key)
{
    const QString locale = QLocale().name();
    for (const QString& name : { QStringLiteral("%1[%2]").arg(key, locale),
             QStringLiteral("%1[%2]").arg(key, locale.section(u'_', 0, 0)), key }) {
        if (group.contains(name)) {
            return group.value(name);
        }
    }
    return {};
}

QString quote(QString text)
{
    text.replace(u'\'', QStringLiteral("'\\''"));
    return u'\'' + text + u'\'';
}

// A file is of a type (or its parents), with KDE's wildcards.
bool matches(const QString& pattern, const QMimeType& type, bool isDir)
{
    if (pattern == QLatin1String("all/all")) {
        return true;
    }
    if (pattern == QLatin1String("all/allfiles")) {
        return !isDir;
    }
    if (pattern.endsWith(QLatin1String("/*"))) {
        const QString prefix = pattern.chopped(1);
        if (type.name().startsWith(prefix)) {
            return true;
        }
        const QStringList ancestors = type.allAncestors();
        return std::any_of(ancestors.cbegin(), ancestors.cend(), [&](const QString& a) { return a.startsWith(prefix); });
    }
    return type.inherits(pattern);
}

} // namespace

ServiceMenus::ServiceMenus(QObject* parent)
    : QObject(parent)
{
}

void ServiceMenus::load()
{
    m_loaded = true;
    // The user's first: the same name hides the system one.
    QSet<QString> seen;
    QStringList dirs;
    for (const QString& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        dirs << data + QStringLiteral("/kio/servicemenus") << data + QStringLiteral("/kservices5/ServiceMenus");
    }
    for (const QString& dir : std::as_const(dirs)) {
        for (const QFileInfo& info : QDir(dir).entryInfoList({ QStringLiteral("*.desktop") }, QDir::Files)) {
            if (seen.contains(info.fileName())) {
                continue;
            }
            seen.insert(info.fileName());
            const QHash<QString, Group> groups = readGroups(info.absoluteFilePath());
            const Group main = groups.value(QStringLiteral("Desktop Entry"));
            // Conditions Vela can't evaluate (a D-Bus call saying whether to
            // show it): better not to show an entry that might not work.
            // X-KDE-AuthorizeAction instead is for locked-down systems
            // (kiosk): normally granted, as in KDE.
            if (main.contains(QStringLiteral("X-KDE-ShowIfDBusCall"))
                || main.value(QStringLiteral("Hidden")) == QLatin1String("true")) {
                continue;
            }
            const QString protocols = main.value(QStringLiteral("X-KDE-Protocols"));
            if (!protocols.isEmpty() && !protocols.split(u',').contains(QStringLiteral("file"))) {
                continue;
            }
            Action base;
            base.mimeTypes = main.value(QStringLiteral("MimeType")).split(u';', Qt::SkipEmptyParts);
            base.submenu = localized(main, QStringLiteral("X-KDE-Submenu"));
            base.minUrls = main.value(QStringLiteral("X-KDE-MinNumberOfUrls"), QStringLiteral("1")).toInt();
            base.maxUrls = main.value(QStringLiteral("X-KDE-MaxNumberOfUrls"), QStringLiteral("-1")).toInt();
            for (const QString& n : main.value(QStringLiteral("X-KDE-RequiredNumberOfUrls")).split(u',', Qt::SkipEmptyParts)) {
                base.requiredUrls.append(n.trimmed().toInt());
            }
            if (base.mimeTypes.isEmpty()) {
                continue;
            }
            for (const QString& id : main.value(QStringLiteral("Actions")).split(u';', Qt::SkipEmptyParts)) {
                if (id == QLatin1String("_SEPARATOR_")) {
                    continue;
                }
                const Group group = groups.value(QStringLiteral("Desktop Action ") + id);
                Action action = base;
                action.name = localized(group, QStringLiteral("Name"));
                action.icon = group.value(QStringLiteral("Icon"));
                action.exec = group.value(QStringLiteral("Exec"));
                if (!action.name.isEmpty() && !action.exec.isEmpty()) {
                    m_actions.append(action);
                }
            }
        }
    }
    qInfo("vela-shell: %lld KDE service menu entries", static_cast<long long>(m_actions.size()));
}

QVariantList ServiceMenus::actionsFor(const QStringList& paths)
{
    if (!m_loaded) {
        load();
    }
    QVariantList out;
    if (paths.isEmpty()) {
        return out;
    }
    static const QMimeDatabase mimes;
    QList<QPair<QMimeType, bool>> types;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        types.append({ mimes.mimeTypeForFile(info), info.isDir() });
    }
    const int count = int(paths.size());
    for (qsizetype i = 0; i < m_actions.size(); ++i) {
        const Action& a = m_actions.at(i);
        if (count < a.minUrls || (a.maxUrls >= 0 && count > a.maxUrls)
            || (!a.requiredUrls.isEmpty() && !a.requiredUrls.contains(count))) {
            continue;
        }
        // It must apply to all the chosen files.
        const bool fits = std::all_of(types.cbegin(), types.cend(), [&](const QPair<QMimeType, bool>& t) {
            return std::any_of(a.mimeTypes.cbegin(), a.mimeTypes.cend(),
                [&](const QString& pattern) { return matches(pattern, t.first, t.second); });
        });
        if (fits) {
            out.append(QVariantMap { { QStringLiteral("id"), int(i) }, { QStringLiteral("text"), a.name },
                { QStringLiteral("icon"), a.icon }, { QStringLiteral("submenu"), a.submenu } });
        }
    }
    return out;
}

void ServiceMenus::run(int id, const QStringList& paths) const
{
    if (id < 0 || id >= m_actions.size() || paths.isEmpty()) {
        return;
    }
    const Action& a = m_actions.at(id);
    const QString directory = QFileInfo(paths.first()).absolutePath();
    const DesktopExec::Parsed parsed = DesktopExec::split(a.exec);
    if (!parsed.ok) {
        qWarning("vela-shell: service menu with invalid Exec: %s", qPrintable(a.exec));
        return;
    }
    QList<QUrl> files;
    for (const QString& path : paths) {
        files.append(QUrl::fromLocalFile(path));
    }

    // Like the spec (desktopexec.h): the program with its arguments, one
    // process per file if it asks for %f or %u.
    if (!parsed.shellSyntax) {
        const DesktopExec::Context context { a.icon, a.name, QString() };
        QList<QList<QUrl>> runs { files };
        if (files.size() > 1 && DesktopExec::onePerFile(parsed.arguments)) {
            runs.clear();
            for (const QUrl& file : std::as_const(files)) {
                runs.append({ file });
            }
        }
        for (const QList<QUrl>& run : std::as_const(runs)) {
            const QStringList arguments = DesktopExec::expand(parsed.arguments, run, context);
            qInfo("vela-shell: service menu: %s", qPrintable(arguments.join(u' ')));
            vela::process::startDetached(arguments.first(), arguments.mid(1), directory);
        }
        return;
    }

    // A line with shell syntax (pipes, $VAR...): not allowed by the spec, but
    // KIO runs it with /bin/sh and some KDE service menus rely on it. Done the
    // same way, with the files always quoted.
    QStringList quotedFiles;
    QStringList quotedUrls;
    for (const QString& path : paths) {
        quotedFiles << quote(path);
        quotedUrls << quote(QUrl::fromLocalFile(path).toString());
    }
    QString command;
    for (qsizetype i = 0; i < a.exec.size(); ++i) {
        const QChar c = a.exec.at(i);
        if (c != u'%' || i + 1 >= a.exec.size()) {
            command += c;
            continue;
        }
        switch (a.exec.at(++i).unicode()) {
        case 'f': command += quotedFiles.first(); break;
        case 'F': command += quotedFiles.join(u' '); break;
        case 'u': command += quotedUrls.first(); break;
        case 'U': command += quotedUrls.join(u' '); break;
        case 'i': command += a.icon.isEmpty() ? QString() : QStringLiteral("--icon ") + quote(a.icon); break;
        case 'c': command += quote(a.name); break;
        case '%': command += u'%'; break;
        default: break; // %k, %d...: deprecated or meaningless here
        }
    }
    qInfo("vela-shell: service menu (shell): %s", qPrintable(command));
    vela::process::startDetached(QStringLiteral("/bin/sh"), { QStringLiteral("-c"), command }, directory);
}
