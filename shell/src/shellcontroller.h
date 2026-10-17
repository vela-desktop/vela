// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDBusUnixFileDescriptor>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QObject>
#include <QString>
#include <QStringList>

// The point of contact between the shell and the rest of the system:
// - receives commands from the compositor on a Unix socket (such as
//   "toggle-start" when you press Super);
// - exposes small session details to QML.
class QWindow;

class ShellController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString userName READ userName CONSTANT)
    Q_PROPERTY(QString userInitial READ userInitial CONSTANT)
    // The Start menu state, shared by the menu window and the taskbar.
    Q_PROPERTY(bool startMenuOpen READ startMenuOpen WRITE setStartMenuOpen NOTIFY startMenuOpenChanged)
    // The apps pinned to Start (.desktop ids), in the chosen order.
    Q_PROPERTY(QStringList startPins READ startPins NOTIFY startPinsChanged)
    // The main output's name (panels asked for from the keyboard open there).
    Q_PROPERTY(QString primaryScreen READ primaryScreen NOTIFY primaryScreenChanged)

public:
    explicit ShellController(QObject* parent = nullptr);

    // The socket path, tied to the current Wayland session.
    static QString socketPath();
    // Sends a command to an already running shell. false if there is none.
    static bool sendToRunningInstance(const QByteArray& command);
    // A command with an answer (waits as long as needed): "choose-source".
    static QByteArray askRunningInstance(const QByteArray& command);

    // Screen sharing (xdg-desktop-portal-wlr): the answer for the portal,
    // "Monitor: <output>", "Window: <identifier>", or empty if the user
    // cancels.
    Q_INVOKABLE void chooseSource(const QString& answer);

    bool listen();
    // A command to the compositor, on its socket (such as "lock").
    static bool sendToCompositor(const QByteArray& command);
    // Shift held right now (the compositor knows: the keyboard may belong to
    // another app while the taskbar is clicked).
    Q_INVOKABLE bool shiftHeld() const;
    // A shell window anchored at the bottom left (the taskbar previews):
    // `left` pixels from the output's left edge.
    Q_INVOKABLE void placeAtLeft(QWindow* window, int left);
    // A (hidden) shell panel on output `name`: it appears there the next time
    // it opens.
    Q_INVOKABLE void placeOnScreen(QWindow* window, const QString& name);
    // Alt+Tab: a click on preview `index` (the compositor switches).
    Q_INVOKABLE void pickSwitcher(int index) { sendToCompositor("switcher-pick " + QByteArray::number(index)); }

    QString userName() const;
    QString primaryScreen() const;
    QString userInitial() const;

    bool startMenuOpen() const { return m_startMenuOpen; }
    void setStartMenuOpen(bool open)
    {
        if (open != m_startMenuOpen) {
            m_startMenuOpen = open;
            emit startMenuOpenChanged();
        }
    }

    Q_INVOKABLE void toggleStartMenu() { emit toggleStartRequested(); }

    QStringList startPins() const { return m_startPins; }
    Q_INVOKABLE void pinToStart(const QString& id);
    Q_INVOKABLE void unpinFromStart(const QString& id);
    Q_INVOKABLE void moveStartPinToFront(const QString& id);

    // The window menu: an action (restore, move, resize, minimize, maximize,
    // close) on the window with that identifier, or on "active".
    Q_INVOKABLE void windowAction(const QString& window, const QString& action);
    // Signs out: asks the compositor to close (apps close with it). Doesn't
    // shut down or restart the computer.
    Q_INVOKABLE void logout();
    Q_INVOKABLE void lock(); // Win+L from the shell
    // The wallpaper's average color (the Mica tint, docs/renderer.md §9.4),
    // also left in the cache for Vela's apps; invalid if it can't be read.
    QColor wallpaperTint(const QString& wallpaper) const;
    // Locks the screen before the computer suspends.
    void watchSleep();
    // The computer, through systemd-logind (polkit asks for the password if
    // needed).
    Q_INVOKABLE void suspend();
    Q_INVOKABLE void reboot();
    Q_INVOKABLE void powerOff();
    Q_INVOKABLE bool canSuspend() const;

signals:
    void toggleStartRequested();
    // Alt+Tab, driven by the compositor: windows (ext-foreign-toplevel
    // identifiers) in order of recent use and the selected one.
    void switcherShown(int selected, const QStringList& windows);
    void switcherSelected(int selected);
    void switcherHidden();
    void startMenuOpenChanged();
    void startPinsChanged();
    void primaryScreenChanged();
    void chooseSourceRequested();
    void chooseSourceCancelled();
    // Windows shortcuts arriving from the compositor.
    void winXRequested(); // Win+X
    void runRequested(); // Win+R
    void showDesktopRequested(); // Win+D
    void quickSettingsRequested(); // Win+A
    void soundOutputRequested(); // Win+Ctrl+V: quick settings' sound page
    void notificationCenterRequested(); // Win+N
    void settingsRequested(); // Win+I
    void filesRequested(); // Win+E
    void taskViewRequested(); // Win+Tab
    void workspacesReceived(const QByteArray& json); // the state of the virtual desktops
    // Snap layouts below the Maximize button (Win+Z): x, y on the output.
    void snapLayoutsRequested(const QString& window, const QString& output, int x, int y, bool keyboard);
    // After a snap: the windows to offer for the free spaces (JSON).
    void snapAssistRequested(const QString& json);
    void propertiesRequested(const QStringList& paths); // from Explorer
    void shareRequested(const QStringList& paths); // "Share", from Explorer
    void openWithRequested(const QString& path); // "Choose another app", from Explorer
    void newShortcutRequested(const QString& folder);
    void desktopTransferUndoReceived(const QByteArray& json);
    void desktopTransferReadyReceived(qint64 pid); // "New > Shortcut", from Explorer
    void accessibilityReceived(const QByteArray& json); // from the compositor
    void clipboardRequested(); // Win+V
    void snipRequested(); // Win+Shift+S, Print
    void clipboardClearRequested(); // from Settings
    // The keyboard's volume keys: "up", "down", "mute", "mic-mute".
    void volumeKeyPressed(const QString& key);
    // The keyboard's media keys: "play-pause", "pause", "stop", "next",
    // "previous".
    void mediaKeyPressed(const QString& key);
    // The window menu (right click on the title bar, Alt+Space): on which
    // output and where, in output coordinates.
    void windowMenuRequested(const QString& window, const QString& output, int x, int y, bool maximized,
        bool resizable, bool keyboard);

private Q_SLOTS:
    void onPrepareForSleep(bool starting);

private:
    void handleCommand(const QByteArray& command);
    void takeSleepDelay();
    void startChooser(QLocalSocket* socket);
    QPointer<QLocalSocket> m_chooser; // the portal waiting for the choice
    void saveStartPins();
    QDBusUnixFileDescriptor m_sleepDelay; // while it's open, logind waits before suspending

    QLocalServer m_server;
    bool m_startMenuOpen = false;
    QStringList m_startPins;
};
