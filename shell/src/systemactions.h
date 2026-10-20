// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class AppModel;

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
    void setAppModel(AppModel* apps) { m_apps = apps; }

    // Names: installed-apps, mobility, power, events, system, devices,
    // network, disks, computer, terminal, terminal-admin, task-manager,
    // settings, files, datetime, display, notification-settings,
    // taskbar-settings, personalize.
    Q_INVOKABLE bool available(const QString& name) const;
    Q_INVOKABLE bool trigger(const QString& name) const;
    // A laptop (a battery that isn't a mouse's or the like).
    Q_INVOKABLE bool isLaptop() const;

    // "Run" (Win+R): a program, a folder, a document or a URL.
    Q_INVOKABLE bool run(const QString& text);
    Q_INVOKABLE bool runOnScreen(const QString& text, const QString& screen);
    Q_INVOKABLE QStringList runHistory() const;

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

signals:
    void chooseAppRequested(const QString& path);
    void chooseAppOnScreenRequested(const QString& path, const QString& screen);

private:
    AppModel* m_apps = nullptr;
    QString command(const QString& name) const;
};

// The preferred terminal and the command lines to use it.
QString terminalProgram();
