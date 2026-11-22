// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace vela::trash {
// Empty Vela Files' FreeDesktop home trash without external gio/ktrash tools.
// Returns entries that could not be deleted; never follows symlinks.
QStringList empty(const QString& trashDirectory); // also used by the isolated test
QStringList emptyHome();
}

// Locate Vela Files both when installed and when running from a build tree.
QString velaFilesExecutable();

// The menu entries that open a piece of the system (Win+X, clock, taskbar...),
// mapped to the Linux programs doing the same thing (docs/renderer.md §14.11).
// An entry without an installed program is "unavailable" and the menu shows it
// disabled.
class SystemActions : public QObject {
    Q_OBJECT

public:
    explicit SystemActions(QObject* parent = nullptr);

    // Names: installed-apps, mobility, power, events, system, network,
    // terminal, terminal-admin, task-manager,
    // settings, files, datetime, display, notification-settings,
    // taskbar-settings, personalize, updates-settings.
    Q_INVOKABLE bool available(const QString& name) const;
    Q_INVOKABLE bool trigger(const QString& name) const;
    // A laptop (a battery that isn't a mouse's or the like).
    Q_INVOKABLE bool isLaptop() const;

    // "Open file location": the folder in the file manager, with the file
    // selected.
    Q_INVOKABLE void showInFolder(const QString& pathOrUrl) const;
    Q_INVOKABLE void openUrl(const QString& url) const;
    Q_INVOKABLE void openTerminal(const QString& directory) const;
    // The Desktop folder (or home, if there is none).
    Q_INVOKABLE QString desktopDirectory() const;

    // "Uninstall": with the app's manager (Discover, Flatpak or the package
    // manager in a terminal, where it's confirmed like in Windows).
    Q_INVOKABLE bool canUninstall(const QString& desktopFile) const;
    Q_INVOKABLE void uninstall(const QString& desktopFile) const;

private:
    QString command(const QString& name) const;
};

// The preferred terminal and the command lines to use it.
QString terminalProgram();
