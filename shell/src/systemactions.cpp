// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemactions.h"
#include "appmodel.h"
#include "mimeapps.h"
#include "velatool.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QtDebug>

namespace {

bool has(const QString& program)
{
    return !QStandardPaths::findExecutable(program).isEmpty();
}

// A KDE settings module, opened alone with kcmshell6.
bool hasKcm(const QString& module)
{
    return has(QStringLiteral("kcmshell6"))
        && !QStandardPaths::locate(QStandardPaths::ApplicationsLocation, module + QStringLiteral(".desktop"))
                .isEmpty();
}

QString quote(QString text)
{
    text.replace(u'\'', QStringLiteral("'\\''"));
    return u'\'' + text + u'\'';
}

bool spawn(const QString& command, const QString& directory = QDir::homePath())
{
    qInfo("vela-shell: starting %s", qPrintable(command));
    return QProcess::startDetached(QStringLiteral("/bin/sh"), { QStringLiteral("-c"), command }, directory);
}

// Use the apps built alongside this executable before installed versions.
QString velaTool(const QString& name, const QString& buildDir)
{
    return vela::toolExecutable(name, buildDir, QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationName());
}

QString velaSettings()
{
    return velaTool(QStringLiteral("vela-settings"), QStringLiteral("settings"));
}


// A command in a terminal that stays open to show how it went.
QString inTerminal(const QString& command)
{
    const QString script = command + QCoreApplication::translate("System", "; echo; read -r -p 'Press Enter to close' _");
    return terminalProgram() + QStringLiteral(" -e sh -c ") + quote(script);
}

} // namespace

QString velaFilesExecutable()
{
    return velaTool(QStringLiteral("vela-files"), QStringLiteral("explorer"));
}

namespace vela::trash {

QStringList empty(const QString& trashDirectory)
{
    const QDir trash(trashDirectory);
    const QDir files(trash.filePath(QStringLiteral("files")));
    const QDir info(trash.filePath(QStringLiteral("info")));
    constexpr QDir::Filters entries = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    QStringList failed;

    for (const QFileInfo& entry : files.entryInfoList(entries)) {
        const bool removed = entry.isDir() && !entry.isSymLink()
            ? QDir(entry.absoluteFilePath()).removeRecursively()
            : QFile::remove(entry.absoluteFilePath());
        if (!removed) failed.append(entry.fileName());
    }
    // Retain metadata for entries that survived deletion. Also clean orphans
    // from operations interrupted before their .trashinfo could be removed.
    for (const QFileInfo& record : info.entryInfoList(entries)) {
        if (!record.fileName().endsWith(QLatin1String(".trashinfo"))) continue;
        const QString name = record.fileName().chopped(10);
        const QFileInfo entry(files.filePath(name));
        if (entry.exists() || entry.isSymLink()) continue;
        if (!QFile::remove(record.absoluteFilePath())) failed.append(record.fileName());
    }
    return failed;
}

QStringList emptyHome()
{
    // Vela Files currently presents the home trash at $XDG_DATA_HOME/Trash/files.
    return empty(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Trash"));
}

} // namespace vela::trash

QString terminalProgram()
{
    const QString chosen = qEnvironmentVariable("VELA_TERMINAL");
    if (!chosen.isEmpty()) {
        return chosen;
    }
    for (const char* candidate : { "konsole", "foot", "kitty", "alacritty", "gnome-terminal", "xterm" }) {
        if (has(QLatin1String(candidate))) {
            return QLatin1String(candidate);
        }
    }
    return QStringLiteral("xterm");
}

SystemActions::SystemActions(QObject* parent)
    : QObject(parent)
{
}

QString SystemActions::command(const QString& name) const
{
    // The first installed program among those doing the same thing.
    const auto first = [](std::initializer_list<std::pair<const char*, const char*>> candidates) {
        for (const auto& [probe, command] : candidates) {
            const QString p = QLatin1String(probe);
            if (p.startsWith(QLatin1String("kcm_")) ? hasKcm(p) : has(p)) {
                return QLatin1String(command);
            }
        }
        return QLatin1String("");
    };
    // The entries that have a page in Vela's Settings.
    static const QHash<QString, QString> settingsPages {
        { QStringLiteral("settings"), QString() },
        { QStringLiteral("display"), QStringLiteral("display") },
        { QStringLiteral("personalize"), QStringLiteral("personalization") },
        { QStringLiteral("taskbar-settings"), QStringLiteral("taskbar") },
        { QStringLiteral("notification-settings"), QStringLiteral("notifications") },
        { QStringLiteral("power"), QStringLiteral("power") },
        { QStringLiteral("mobility"), QStringLiteral("power") },
        { QStringLiteral("network"), QStringLiteral("network") },
        { QStringLiteral("datetime"), QStringLiteral("datetime") },
        { QStringLiteral("installed-apps"), QStringLiteral("installed-apps") },
        { QStringLiteral("system"), QStringLiteral("about") },
        { QStringLiteral("sound-settings"), QStringLiteral("sound") },
        { QStringLiteral("volume-mixer"), QStringLiteral("volume-mixer") },
        { QStringLiteral("bluetooth-settings"), QStringLiteral("bluetooth") },
        { QStringLiteral("updates-settings"), QStringLiteral("updates") },
        { QStringLiteral("night-light-settings"), QStringLiteral("night-light") },
        { QStringLiteral("accessibility-settings"), QStringLiteral("accessibility") },
        { QStringLiteral("colors-settings"), QStringLiteral("colors") },
    };
    if (settingsPages.contains(name)) {
        const QString program = velaSettings();
        if (!program.isEmpty()) {
            const QString page = settingsPages.value(name);
            return quote(program) + (page.isEmpty() ? QString() : QStringLiteral(" --page ") + page);
        }
    }
    if (name == QLatin1String("terminal")) {
        return terminalProgram();
    }
    if (name == QLatin1String("terminal-admin")) {
        // Like "Terminal (Admin)": a root shell, the password asked by polkit
        // (run0) or sudo.
        return terminalProgram() + (has(QStringLiteral("run0")) ? QStringLiteral(" -e run0") : QStringLiteral(" -e sudo -i"));
    }
    if (name == QLatin1String("task-manager")) {
        const QString gui = first({ { "plasma-systemmonitor", "plasma-systemmonitor" },
            { "gnome-system-monitor", "gnome-system-monitor" }, { "missioncenter", "missioncenter" } });
        if (!gui.isEmpty()) {
            return gui;
        }
        return has(QStringLiteral("btop")) ? terminalProgram() + QStringLiteral(" -e btop")
            : has(QStringLiteral("htop"))  ? terminalProgram() + QStringLiteral(" -e htop")
                                           : terminalProgram() + QStringLiteral(" -e top");
    }
    if (name == QLatin1String("events")) {
        const QString gui = first({ { "kjournaldbrowser", "kjournaldbrowser" } });
        return !gui.isEmpty() ? gui : terminalProgram() + QStringLiteral(" -e journalctl -b -e");
    }
    if (name == QLatin1String("files")) {
        // Vela's Explorer, if present.
        if (const QString files = velaFilesExecutable(); !files.isEmpty()) {
            return quote(files);
        }
        return QString(); // no browser fallback when no file manager is installed
    }
    if (name == QLatin1String("settings")) {
        return first({ { "systemsettings", "systemsettings" }, { "gnome-control-center", "gnome-control-center" } });
    }
    if (name == QLatin1String("system") || name == QLatin1String("computer")) {
        return first({ { "kinfocenter", "kinfocenter" } });
    }
    if (name == QLatin1String("devices")) {
        return first({ { "kinfocenter", "kinfocenter" } });
    }
    if (name == QLatin1String("network")) {
        return first({ { "kcm_networkmanagement", "kcmshell6 kcm_networkmanagement" },
            { "nm-connection-editor", "nm-connection-editor" } });
    }
    if (name == QLatin1String("disks")) {
        return first({ { "partitionmanager", "partitionmanager" }, { "gnome-disks", "gnome-disks" } });
    }
    if (name == QLatin1String("power") || name == QLatin1String("mobility")) {
        return first({ { "kcm_powerdevilprofilesconfig", "kcmshell6 kcm_powerdevilprofilesconfig" } });
    }
    if (name == QLatin1String("installed-apps")) {
        return first({ { "plasma-discover", "plasma-discover --mode Installed" }, { "gnome-software", "gnome-software" } });
    }
    if (name == QLatin1String("sound-settings")) {
        return first({ { "kcm_pulseaudio", "kcmshell6 kcm_pulseaudio" }, { "pavucontrol", "pavucontrol" } });
    }
    if (name == QLatin1String("datetime")) {
        return first({ { "kcm_clock", "kcmshell6 kcm_clock" } });
    }
    if (name == QLatin1String("display")) {
        // Programs speaking wlr-output-management, like Vela.
        return first({ { "nwg-displays", "nwg-displays" }, { "wdisplays", "wdisplays" } });
    }
    // Notification settings, taskbar settings, Personalize: only with Vela's
    // Settings (above).
    return {};
}

bool SystemActions::available(const QString& name) const
{
    return !command(name).isEmpty();
}

bool SystemActions::trigger(const QString& name) const
{
    const QString cmd = command(name);
    return !cmd.isEmpty() && spawn(cmd);
}

bool SystemActions::isLaptop() const
{
    const QDir supplies(QStringLiteral("/sys/class/power_supply"));
    for (const QString& entry : supplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto read = [&](const char* file) {
            QFile f(supplies.filePath(entry) + u'/' + QLatin1String(file));
            return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
        };
        // Mouse and keyboard batteries have scope "Device".
        if (read("type") == QLatin1String("Battery") && read("scope") != QLatin1String("Device")) {
            return true;
        }
    }
    return false;
}

bool SystemActions::run(const QString& input)
{
    return runOnScreen(input, {});
}

bool SystemActions::runOnScreen(const QString& input, const QString& screen)
{
    const QString text = input.trimmed();
    if (text.isEmpty()) {
        return false;
    }
    // Handle folders locally; xdg-open must never choose a browser for them.
    const QString expanded = text.startsWith(u'~') ? QDir::homePath() + text.mid(1) : text;
    const QFileInfo info(expanded);
    bool ok = false;
    if (info.isDir()) {
        const QString files = velaFilesExecutable();
        ok = !files.isEmpty() && QProcess::startDetached(files, { info.absoluteFilePath() });
    } else if (info.isFile()) {
        const QString mime = QMimeDatabase().mimeTypeForFile(info).name();
        const QString id = MimeApps::defaultFor(mime);
        if (m_apps && !id.isEmpty() && m_apps->launchWithFile(id, QUrl::fromLocalFile(info.absoluteFilePath()).toString())) {
            ok = true;
        } else {
            if (screen.isEmpty()) emit chooseAppRequested(info.absoluteFilePath());
            else emit chooseAppOnScreenRequested(info.absoluteFilePath(), screen);
            ok = true;
        }
    } else if (text.contains(QLatin1String("://"))) {
        const QUrl url(text);
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
            ok = QProcess::startDetached(QStringLiteral("xdg-open"), { text });
        else if (url.isLocalFile()) {
            const QFileInfo local(url.toLocalFile());
            if (local.isDir()) {
                const QString files = velaFilesExecutable();
                ok = !files.isEmpty() && QProcess::startDetached(files, { local.absoluteFilePath() });
            }
        }
    } else {
        ok = spawn(text);
    }
    if (ok) {
        QSettings settings;
        QStringList history = settings.value(QStringLiteral("run/history")).toStringList();
        history.removeAll(text);
        history.prepend(text);
        settings.setValue(QStringLiteral("run/history"), history.mid(0, 26));
    }
    return ok;
}

QStringList SystemActions::runHistory() const
{
    return QSettings().value(QStringLiteral("run/history")).toStringList();
}

void SystemActions::showInFolder(const QString& pathOrUrl) const
{
    const QUrl url = pathOrUrl.contains(QLatin1String("://")) ? QUrl(pathOrUrl) : QUrl::fromLocalFile(pathOrUrl);
    // Vela's Explorer opens the folder with the file selected.
    if (const QString files = velaFilesExecutable(); !files.isEmpty() && url.isLocalFile()) {
        QProcess::startDetached(files, { url.toLocalFile() });
        return;
    }
    // The file manager standard (Dolphin, Nautilus...): opens the folder and
    // selects the file.
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"), QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("ShowItems"));
    call << QStringList { url.toString() } << QString();
    if (!QDBusConnection::sessionBus().send(call) && url.isLocalFile()) {
        qWarning("vela-shell: no file manager available to show %s", qPrintable(url.toLocalFile()));
    }
}

void SystemActions::openUrl(const QString& url) const
{
    QProcess::startDetached(QStringLiteral("xdg-open"), { url });
}

void SystemActions::openTerminal(const QString& directory) const
{
    const QString dir = directory.isEmpty() ? QDir::homePath() : directory;
    const QString terminal = terminalProgram();
    // Konsole wants the directory as an option; the others start in the
    // current one.
    spawn(terminal == QLatin1String("konsole") ? terminal + QStringLiteral(" --workdir ") + quote(dir) : terminal, dir);
}

QString SystemActions::desktopDirectory() const
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return QFileInfo(desktop).isDir() ? desktop : QDir::homePath();
}

namespace {

// The command removing the package the .desktop file belongs to.
QString uninstallCommand(const QString& desktopFile)
{
    if (desktopFile.isEmpty()) {
        return {};
    }
    // Flatpak apps: .../flatpak/exports/share/applications/<id>.desktop
    if (desktopFile.contains(QLatin1String("/flatpak/exports/")) && has(QStringLiteral("flatpak"))) {
        const QString id = QFileInfo(desktopFile).completeBaseName();
        return QStringLiteral("flatpak uninstall ") + quote(id);
    }
    if (has(QStringLiteral("plasma-discover"))) {
        return {}; // Discover is used, see uninstall()
    }
    const QString file = quote(desktopFile);
    const QString root = has(QStringLiteral("run0")) ? QStringLiteral("run0 ") : QStringLiteral("sudo ");
    if (has(QStringLiteral("pacman"))) {
        return root + QStringLiteral("pacman -Rs \"$(pacman -Qqo ") + file + QStringLiteral(")\"");
    }
    if (has(QStringLiteral("dnf"))) {
        return root + QStringLiteral("dnf remove \"$(rpm -qf --qf '%{NAME}' ") + file + QStringLiteral(")\"");
    }
    if (has(QStringLiteral("zypper"))) {
        return root + QStringLiteral("zypper remove \"$(rpm -qf --qf '%{NAME}' ") + file + QStringLiteral(")\"");
    }
    if (has(QStringLiteral("apt"))) {
        return root + QStringLiteral("apt remove \"$(dpkg -S ") + file + QStringLiteral(" | cut -d: -f1)\"");
    }
    return {};
}

} // namespace

bool SystemActions::canUninstall(const QString& desktopFile) const
{
    // The user's apps (~/.local/share/applications) aren't packages.
    if (desktopFile.startsWith(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation))) {
        return false;
    }
    return has(QStringLiteral("plasma-discover")) || !uninstallCommand(desktopFile).isEmpty();
}

void SystemActions::uninstall(const QString& desktopFile) const
{
    const QString command = uninstallCommand(desktopFile);
    if (!command.isEmpty()) {
        spawn(inTerminal(command));
    } else if (has(QStringLiteral("plasma-discover"))) {
        // Like Windows with classic apps: the app's page in the store.
        const QString id = QFileInfo(desktopFile).completeBaseName();
        spawn(QStringLiteral("plasma-discover --application ") + quote(QStringLiteral("appstream://") + id));
    }
}
