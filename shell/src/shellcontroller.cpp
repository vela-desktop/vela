// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shellcontroller.h"

#include "mica.h"

#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <LayerShellQt/Window>
#include <QGuiApplication>
#include <QJsonArray>
#include <QScreen>
#include <QJsonDocument>
#include <QSettings>

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QLocalSocket>
#include <QTimer>
#include <QStandardPaths>
#include <QtDebug>

#include <pwd.h>
#include <unistd.h>

namespace {

// At first start: everyday apps, if installed (Start skips the missing ones).
const QStringList defaultStartPins {
    QStringLiteral("firefox.desktop"),
    QStringLiteral("org.mozilla.firefox.desktop"),
    QStringLiteral("chromium.desktop"),
    QStringLiteral("vela-files.desktop"),
    QStringLiteral("org.kde.dolphin.desktop"),
    QStringLiteral("org.kde.konsole.desktop"),
    QStringLiteral("org.kde.kate.desktop"),
    QStringLiteral("org.kde.okular.desktop"),
    QStringLiteral("org.kde.gwenview.desktop"),
    QStringLiteral("org.kde.spectacle.desktop"),
    QStringLiteral("org.kde.kcalc.desktop"),
    QStringLiteral("org.kde.plasma-systemmonitor.desktop"),
    QStringLiteral("vela-settings.desktop"),
    QStringLiteral("systemsettings.desktop"),
};

} // namespace

ShellController::ShellController(QObject* parent)
    : QObject(parent)
    , m_startPins(QSettings().value(QStringLiteral("start/pinned"), defaultStartPins).toStringList())
{
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, &ShellController::primaryScreenChanged);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                while (socket->canReadLine()) {
                    const QByteArray line = socket->readLine().trimmed();
                    if (line == "choose-source") {
                        startChooser(socket); // answers when the user chooses
                    } else {
                        handleCommand(line);
                    }
                }
            });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                if (m_chooser == socket) {
                    m_chooser = nullptr;
                    emit chooseSourceCancelled(); // the portal gave up
                }
                // A possible command without a final newline.
                const QByteArray rest = socket->readAll().trimmed();
                if (!rest.isEmpty()) {
                    handleCommand(rest);
                }
                socket->deleteLater();
            });
        }
    });
}

QString ShellController::socketPath()
{
    // Must match vela_shell_send() in the compositor (shell.c).
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    return runtimeDir + QStringLiteral("/vela-shell-") + display + QStringLiteral(".sock");
}

QByteArray ShellController::askRunningInstance(const QByteArray& command)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(1000)) {
        return {};
    }
    socket.write(command + '\n');
    socket.waitForBytesWritten(1000);
    // The user can take as long as they want: wait for the answer or the
    // close.
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(-1)) {
            break;
        }
    }
    return socket.readLine().trimmed();
}

void ShellController::startChooser(QLocalSocket* socket)
{
    if (m_chooser && m_chooser != socket) {
        chooseSource(QString()); // a new request takes the old one's place
    }
    m_chooser = socket;
    emit chooseSourceRequested();
}

void ShellController::chooseSource(const QString& answer)
{
    QPointer<QLocalSocket> socket = m_chooser;
    m_chooser = nullptr;
    if (!socket) {
        return;
    }
    socket->write(answer.toUtf8() + '\n');
    socket->waitForBytesWritten(300);
    socket->disconnectFromServer();
}

bool ShellController::sendToRunningInstance(const QByteArray& command)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(300)) {
        return false;
    }
    socket.write(command + '\n');
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
}

// The compositor's command socket: see vela_commands_listen() (command.c).
bool ShellController::sendToCompositor(const QByteArray& command)
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock"));
    if (!socket.waitForConnected(300)) {
        qWarning("vela-shell: the compositor isn't responding (%s)", command.constData());
        return false;
    }
    socket.write(command + '\n');
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
}

void ShellController::logout()
{
    sendToCompositor("logout");
}

void ShellController::lock()
{
    sendToCompositor("lock");
}

namespace {

QDBusInterface login1()
{
    return QDBusInterface(QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"), QDBusConnection::systemBus());
}

void callLogin1(const char* method)
{
    // interactive = true: if authorization is needed, polkit asks for it.
    QDBusInterface manager = login1();
    manager.asyncCall(QLatin1String(method), true);
}

} // namespace

void ShellController::suspend()
{
    callLogin1("Suspend");
}

void ShellController::reboot()
{
    callLogin1("Reboot");
}

void ShellController::powerOff()
{
    callLogin1("PowerOff");
}

// Before suspending, the screen locks: on wakeup the desktop is never exposed.
// logind waits as long as we keep the "delay" open.
void ShellController::watchSleep()
{
    QDBusConnection::systemBus().connect(QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"), QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("PrepareForSleep"), this, SLOT(onPrepareForSleep(bool)));
    takeSleepDelay();
}

void ShellController::takeSleepDelay()
{
    if (m_sleepDelay.isValid()) {
        return;
    }
    QDBusInterface manager = login1();
    const QDBusReply<QDBusUnixFileDescriptor> reply = manager.call(QStringLiteral("Inhibit"), QStringLiteral("sleep"),
        QStringLiteral("Vela"), QCoreApplication::translate("Session", "Lock the screen before suspending"), QStringLiteral("delay"));
    if (reply.isValid()) {
        m_sleepDelay = reply.value();
    }
}

void ShellController::onPrepareForSleep(bool starting)
{
    if (!starting) {
        takeSleepDelay(); // woken up: ready for next time
        return;
    }
    lock();
    // A moment for the lock screen to appear, then suspending is allowed
    // (closing the delay).
    QTimer::singleShot(700, this, [this] { m_sleepDelay = QDBusUnixFileDescriptor(); });
}

bool ShellController::canSuspend() const
{
    QDBusInterface manager = login1();
    const QDBusReply<QString> reply = manager.call(QStringLiteral("CanSuspend"));
    return reply.isValid() && reply.value() != QLatin1String("no") && reply.value() != QLatin1String("na");
}

bool ShellController::listen()
{
    const QString path = socketPath();
    QLocalServer::removeServer(path); // socket left over from a previous run
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(path)) {
        qWarning("vela-shell: can't listen on %s: %s", qPrintable(path),
            qPrintable(m_server.errorString()));
        return false;
    }
    return true;
}

void ShellController::handleCommand(const QByteArray& command)
{
    const QList<QByteArray> parts = command.split(' ');
    if (command == "toggle-start") {
        emit toggleStartRequested();
    } else if (parts.first() == "switcher-show" && parts.size() >= 3) {
        QStringList windows;
        for (qsizetype i = 2; i < parts.size(); ++i) {
            windows << QString::fromLatin1(parts.at(i));
        }
        emit switcherShown(parts.at(1).toInt(), windows);
    } else if (parts.first() == "switcher-select" && parts.size() == 2) {
        emit switcherSelected(parts.at(1).toInt());
    } else if (command == "switcher-hide") {
        emit switcherHidden();
    } else if (command == "winx") {
        emit winXRequested();
    } else if (command == "run") {
        emit runRequested();
    } else if (command == "show-desktop") {
        emit showDesktopRequested();
    } else if (command == "quick-settings") {
        emit quickSettingsRequested();
    } else if (command == "sound-output") {
        emit soundOutputRequested();
    } else if (command == "notification-center") {
        emit notificationCenterRequested();
    } else if (command == "settings") {
        emit settingsRequested();
    } else if (command == "files") {
        emit filesRequested();
    } else if (command == "clipboard") {
        emit clipboardRequested();
    } else if (command == "snip") {
        emit snipRequested();
    } else if (command == "clipboard-clear") {
        emit clipboardClearRequested();
    } else if (parts.first() == "volume" && parts.size() == 2) {
        emit volumeKeyPressed(QString::fromLatin1(parts.at(1)));
    } else if (parts.first() == "media" && parts.size() == 2) {
        emit mediaKeyPressed(QString::fromLatin1(parts.at(1)));
    } else if (command == "task-view") {
        emit taskViewRequested();
    } else if (parts.first() == "workspaces") {
        emit workspacesReceived(command.mid(11));
    } else if (parts.first() == "snap-layouts" && parts.size() == 6) {
        // snap-layouts <window> <output> <x> <y> <from keyboard>
        emit snapLayoutsRequested(QString::fromLatin1(parts.at(1)), QString::fromUtf8(parts.at(2)), parts.at(3).toInt(),
            parts.at(4).toInt(), parts.at(5) == "1");
    } else if (parts.first() == "properties") {
        // properties ["path", ...]: the Properties window, asked for by
        // Explorer.
        QStringList paths;
        for (const QJsonValue& value : QJsonDocument::fromJson(command.mid(11)).array()) {
            paths.append(value.toString());
        }
        if (!paths.isEmpty()) {
            emit propertiesRequested(paths);
        }
    } else if (parts.first() == "share") {
        // share ["path", ...]: Share, asked for by Explorer.
        QStringList paths;
        for (const QJsonValue& value : QJsonDocument::fromJson(command.mid(6)).array()) {
            paths.append(value.toString());
        }
        if (!paths.isEmpty()) {
            emit shareRequested(paths);
        }
    } else if (parts.first() == "open-with") {
        // open-with ["path"]: "Choose another app", asked for by Explorer.
        const QJsonArray paths = QJsonDocument::fromJson(command.mid(10)).array();
        if (!paths.isEmpty()) {
            emit openWithRequested(paths.first().toString());
        }
    } else if (parts.first() == "new-shortcut") {
        // new-shortcut ["folder"]: "New > Shortcut", asked for by Explorer.
        const QJsonArray folders = QJsonDocument::fromJson(command.mid(13)).array();
        if (!folders.isEmpty()) {
            emit newShortcutRequested(folders.first().toString());
        }
    } else if (parts.first() == "accessibility") {
        emit accessibilityReceived(command.mid(14));
    } else if (parts.first() == "snap-assist") {
        emit snapAssistRequested(QString::fromUtf8(command.mid(12)));
    } else if (parts.first() == "window-menu" && parts.size() == 9) {
        // window-menu <id> <output> <x> <y> <maximized> <resizable> <from
        // keyboard> <always on top>
        emit windowMenuRequested(QString::fromLatin1(parts.at(1)), QString::fromUtf8(parts.at(2)), parts.at(3).toInt(),
            parts.at(4).toInt(), parts.at(5) == "1", parts.at(6) == "1", parts.at(7) == "1", parts.at(8) == "1");
    } else if (!command.isEmpty() && command != "ping") {
        qWarning("vela-shell: comando sconosciuto '%s'", command.constData());
    }
}

void ShellController::placeAtLeft(QWindow* window, int left)
{
    if (auto* layer = window ? LayerShellQt::Window::get(window) : nullptr) {
        const QMargins margins = layer->margins();
        if (margins.left() != left) {
            layer->setMargins(QMargins(left, margins.top(), margins.right(), margins.bottom()));
        }
    }
}

void ShellController::placeOnScreen(QWindow* window, const QString& name)
{
    if (!window) {
        return;
    }
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->name() == name) {
            window->setScreen(screen);
            if (auto* layer = LayerShellQt::Window::get(window)) {
                layer->setScreen(screen);
            }
            return;
        }
    }
}

QString ShellController::primaryScreen() const
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->name() : QString();
}

QString ShellController::userName() const
{
    // The account's full name (GECOS field), otherwise the login.
    if (const passwd* pw = getpwuid(getuid())) {
        const QString gecos = QString::fromLocal8Bit(pw->pw_gecos).section(u',', 0, 0).trimmed();
        if (!gecos.isEmpty()) {
            return gecos;
        }
        return QString::fromLocal8Bit(pw->pw_name);
    }
    return qEnvironmentVariable("USER");
}

void ShellController::sendWallpaperTint(const QString& wallpaper) const
{
    // Scaled down to a few pixels already while reading (SVGs too): then the
    // average.
    QImageReader reader(wallpaper);
    reader.setScaledSize(QSize(32, 18));
    const QImage image = reader.read().convertToFormat(QImage::Format_RGB32);
    if (image.isNull()) {
        return;
    }
    qint64 sum[3] {};
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            sum[0] += qRed(pixel);
            sum[1] += qGreen(pixel);
            sum[2] += qBlue(pixel);
        }
    }
    const qint64 count = qint64(image.width()) * image.height();
    saveWallpaperTint(QColor(int(sum[0] / count), int(sum[1] / count), int(sum[2] / count)));
    sendToCompositor(QByteArray("wallpaper-tint ") + QByteArray::number(sum[0] / count) + ' '
        + QByteArray::number(sum[1] / count) + ' ' + QByteArray::number(sum[2] / count));
}

QString ShellController::userInitial() const
{
    const QString name = userName();
    return name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper();
}

// --------------------------------------------------------- apps in Start --


void ShellController::saveStartPins()
{
    QSettings().setValue(QStringLiteral("start/pinned"), m_startPins);
    emit startPinsChanged();
}

void ShellController::pinToStart(const QString& id)
{
    if (!id.isEmpty() && !m_startPins.contains(id)) {
        m_startPins.append(id); // at the end, like Windows
        saveStartPins();
    }
}

void ShellController::unpinFromStart(const QString& id)
{
    if (m_startPins.removeAll(id) > 0) {
        saveStartPins();
    }
}

void ShellController::moveStartPinToFront(const QString& id)
{
    if (m_startPins.removeAll(id) > 0) {
        m_startPins.prepend(id);
        saveStartPins();
    }
}

void ShellController::windowAction(const QString& window, const QString& action)
{
    sendToCompositor("window " + window.toLatin1() + ' ' + action.toLatin1());
}

bool ShellController::shiftHeld() const
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock"));
    if (!socket.waitForConnected(100)) {
        return false;
    }
    socket.write("modifiers\n");
    if (!socket.waitForBytesWritten(100) || !socket.waitForReadyRead(100)) {
        return false;
    }
    constexpr int shift = 1; // WLR_MODIFIER_SHIFT
    return socket.readLine().trimmed().toInt() & shift;
}
