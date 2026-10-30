// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-settings: Vela's Settings, like Windows 11's.
//
//   vela-settings [--page <name>]
//
// A single window: if it's already open, a second call only asks it to show
// the page and come to the front (that's how the shell opens "Display
// settings", "Personalize" and the other menu entries).

#include "about.h"
#include "appmodel.h"
#include "audio.h"
#include "audioapps.h"
#include "bluetooth/bluetooth.h"
#include "brightness.h"
#include "controls.h"
#include "datetime.h"
#include "defaultapps.h"
#include "displays.h"
#include "iconprovider.h"
#include "keyboardlayouts.h"
#include "language.h"
#include "levelmeter.h"
#include "network.h"
#include "preferences.h"
#include "systemactions.h"
#include "systemstatus.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtQml/QQmlExtensionPlugin>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)
Q_IMPORT_QML_PLUGIN(Vela_Bluetooth_UiPlugin)

namespace {

QString socketPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/vela-settings-")
        + qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0")) + QStringLiteral(".sock");
}

// A window is already open: it gets the page (and the token to come to the
// front) and we exit.
bool forwardToRunningInstance(const QString& page)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(300)) {
        return false;
    }
    socket.write("page " + page.toUtf8() + ' ' + qgetenv("XDG_ACTIVATION_TOKEN") + '\n');
    socket.waitForBytesWritten(300);
    return true;
}

// Receives the "page <name> [token]" of later calls.
class PageRouter : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString initialPage READ initialPage CONSTANT)

public:
    explicit PageRouter(QString initialPage, QObject* parent = nullptr)
        : QObject(parent)
        , m_initialPage(std::move(initialPage))
    {
    }

    QString initialPage() const { return m_initialPage; }

    bool listen()
    {
        QLocalServer::removeServer(socketPath());
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket* client = m_server.nextPendingConnection()) {
                connect(client, &QLocalSocket::readyRead, this, [this, client] {
                    while (client->canReadLine()) {
                        const QList<QByteArray> words = client->readLine().trimmed().split(' ');
                        if (words.value(0) == "page") {
                            if (words.size() > 2 && !words[2].isEmpty()) {
                                qputenv("XDG_ACTIVATION_TOKEN", words[2]); // requestActivate() uses it
                            }
                            emit pageRequested(QString::fromUtf8(words.value(1)));
                        }
                    }
                });
                connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            }
        });
        return m_server.listen(socketPath());
    }

signals:
    void pageRequested(const QString& page);

private:
    QString m_initialPage;
    QLocalServer m_server;
};

} // namespace

int main(int argc, char* argv[])
{
    // Text with FreeType and font hinting, like Plasma and Windows: crisper
    // than Qt Quick's default text (distance fields, no hinting), on the
    // output's real pixels even at fractional scales.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_DEFAULT_TEXT_RENDER_TYPE")) {
        QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-settings"));
    QQmlApplicationEngine* qmlEngine = nullptr;
    vela::language::install(QStringLiteral("vela-settings"), [&qmlEngine] {
        QGuiApplication::setApplicationDisplayName(QCoreApplication::translate("Settings", "Settings"));
        if (qmlEngine) {
            qmlEngine->retranslate();
        }
    });
    QGuiApplication::setApplicationDisplayName(QCoreApplication::translate("Settings", "Settings"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    QGuiApplication::setDesktopFileName(QStringLiteral("vela-settings"));
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("preferences-system")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Vela Settings"));
    parser.addHelpOption();
    const QCommandLineOption pageOption(QStringLiteral("page"),
        QStringLiteral("The page to open: system, display, sound, notifications, power, about, bluetooth, "
                       "volume-mixer, network, personalization, background, colors, taskbar, apps, installed-apps, "
                       "default-apps, time-language, datetime, keyboard, night-light, accessibility."),
        QStringLiteral("name"));
    parser.addOption(pageOption);
    parser.process(app);
    const QString page = parser.value(pageOption);

    if (forwardToRunningInstance(page)) {
        return 0;
    }

    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    PageRouter router(page);
    router.listen();

    AppModel apps;
    apps.reload();
    SystemActions system;
    Preferences preferences;
    Displays displays;
    // Bluetooth (docs/bluetooth.md §9.1): Settings is Vela's UI, and draws
    // the questions of its own Adds.
    vela::bluetooth::Bluetooth bluetooth(true);
    // The sound (docs/audio.md): the same library as the shell's.
    vela::audio::Audio audio;
    audio.setAppResolver(audioAppResolver(apps));
    audio.setBluetooth(&bluetooth); // headsets' batteries
    Network network;
    About about;
    DateTime dateTime;
    DefaultApps defaultApps(&apps);
    KeyboardLayouts layouts;
    SystemStatus status(&bluetooth);
    Brightness brightness;

    QQmlApplicationEngine engine;
    qmlEngine = &engine;
    // Theme, Button, TextBox and Style (the apps' mode, accent, Mica, icons):
    // Vela.Controls.
    vela::controls::install(engine, Appearance::Mode::Apps)->watch();
    engine.addImageProvider(QStringLiteral("fileicon"), new IconProvider(32));
    QQmlContext* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("Router"), &router);
    context->setContextProperty(QStringLiteral("Apps"), &apps);
    context->setContextProperty(QStringLiteral("System"), &system);
    context->setContextProperty(QStringLiteral("Prefs"), &preferences);
    context->setContextProperty(QStringLiteral("Displays"), &displays);
    context->setContextProperty(QStringLiteral("Audio"), &audio);
    qmlRegisterType<vela::audio::LevelMeter>("Vela.Audio", 1, 0, "LevelMeter");
    context->setContextProperty(QStringLiteral("Network"), &network);
    qmlRegisterSingletonInstance("Vela.Bluetooth", 1, 0, "Bluetooth", &bluetooth);
    context->setContextProperty(QStringLiteral("About"), &about);
    context->setContextProperty(QStringLiteral("DateTime"), &dateTime);
    context->setContextProperty(QStringLiteral("DefaultApps"), &defaultApps);
    context->setContextProperty(QStringLiteral("Layouts"), &layouts);
    context->setContextProperty(QStringLiteral("Status"), &status);
    context->setContextProperty(QStringLiteral("Brightness"), &brightness);
    engine.loadFromModule("Vela.Settings", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    return app.exec();
}

#include "main.moc"
