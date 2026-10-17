// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-shell: Vela's taskbar and Start menu.
//
// Every shell piece is a "layer-shell" window: the compositor knows it's part
// of the desktop (not an app), keeps it above or below the windows and
// reserves space for it on the output.

#include "accessibility.h"
#include "appmodel.h"
#include "audio.h"
#include "audioapps.h"
#include "clipboard.h"
#include "config.h"
#include "controls.h"
#include "desktopmodel.h"
#include "fileactions.h"
#include "fileproperties.h"
#include "filethumbnails.h"
#include "foreigntoplevels.h"
#include "iconprovider.h"
#include "jumplists.h"
#include "language.h"
#include "levelmeter.h"
#include "mediakeys.h"
#include "network.h"
#include "notifications.h"
#include "servicemenus.h"
#include "shellcontroller.h"
#include "snip.h"
#include "soundkeys.h"
#include "systemactions.h"
#include "systemstatus.h"
#include "taskbarmodel.h"
#include "tray.h"
#include "wallpaperprovider.h"
#include "screenwindows.h"
#include "workspaces.h"
#include "windowcapture.h"

#include <LayerShellQt/Window>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QtDebug>
#include <QtQml/QQmlExtensionPlugin>

#include <cstdio>
#include <memory>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

namespace {

using LayerWindow = LayerShellQt::Window;

void releaseHiddenPanel(QQuickWindow* window)
{
    // Qt can retain the wl_surface and its EGL buffers after hide(). A new
    // layer role then shares that native surface with the previous rendering
    // cycle. Use a fresh surface on reopen, including its configure handshake.
    // Keep the QML window, models and LayerShellQt settings intact.
    QObject::connect(window, &QWindow::visibleChanged, window, [window](bool visible) {
        if (!visible) {
            QTimer::singleShot(0, window, [window] {
                if (!window->isVisible()) {
                    window->destroy();
                }
            });
        }
    });
}

void setupTaskbar(QQuickWindow* window, QScreen* screen)
{
    LayerWindow* layer = LayerWindow::get(window);
    layer->setScreen(screen); // one per output (ScreenWindows)
    layer->setScope(QStringLiteral("vela-taskbar"));
    layer->setLayer(LayerWindow::LayerTop);
    layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorBottom) | LayerWindow::AnchorLeft
        | LayerWindow::AnchorRight);
    layer->setExclusiveZone(window->height()); // maximized windows don't cover it
    layer->setKeyboardInteractivity(LayerWindow::KeyboardInteractivityNone);
}

void setupWallpaper(QQuickWindow* window, QScreen* screen)
{
    LayerWindow* layer = LayerWindow::get(window);
    layer->setScreen(screen); // this very output, not the one the compositor would choose
    layer->setScope(QStringLiteral("vela-wallpaper"));
    layer->setLayer(LayerWindow::LayerBackground);
    layer->setAnchors(LayerWindow::Anchors(LayerWindow::AnchorTop) | LayerWindow::AnchorBottom
        | LayerWindow::AnchorLeft | LayerWindow::AnchorRight);
    layer->setExclusiveZone(-1); // the whole output, also below the taskbar
    // Desktop icons take the keyboard when clicked (F2, Del, Ctrl+C...).
    layer->setKeyboardInteractivity(LayerWindow::KeyboardInteractivityOnDemand);
}

// The shell's other windows: one each, made layer-shell surfaces by
// loadPanel. Anchored at no edge, the compositor centers a window; an
// exclusive zone of -1 covers the whole output, also below the taskbar, and 0
// keeps out of the space the taskbar reserved.
struct Panel {
    const char* qml; // the component in Vela.Shell
    const char* scope;
    LayerWindow::Layer layer;
    LayerWindow::Anchors anchors;
    int exclusiveZone;
    LayerWindow::KeyboardInteractivity keyboard;
    QMargins margins = {};
    bool clickThrough = false; // neither clicks nor keyboard
};

constexpr LayerWindow::Anchors centered;
constexpr LayerWindow::Anchors bottom(LayerWindow::AnchorBottom);
constexpr LayerWindow::Anchors bottomLeft = bottom | LayerWindow::AnchorLeft;
constexpr LayerWindow::Anchors bottomRight = bottom | LayerWindow::AnchorRight;
constexpr LayerWindow::Anchors fill = bottom | LayerWindow::AnchorTop | LayerWindow::AnchorLeft | LayerWindow::AnchorRight;
constexpr auto layerTop = LayerWindow::LayerTop;
constexpr auto layerOverlay = LayerWindow::LayerOverlay;
constexpr auto keysNone = LayerWindow::KeyboardInteractivityNone;
constexpr auto keysOnDemand = LayerWindow::KeyboardInteractivityOnDemand;
constexpr auto keysExclusive = LayerWindow::KeyboardInteractivityExclusive;

const Panel panels[] = {
    // Above the taskbar, which reserved its space: the window touches it, so
    // the rising panel is cut there and seems to come out from behind it,
    // like on Windows 11. Centered or at the left: placeStartMenu.
    { "StartMenu", "vela-start-menu", layerTop, bottom, 0, keysOnDemand },
    // Fullscreen and transparent, the panel in the center. The keyboard stays
    // with the compositor, which handles Alt+Tab.
    { "Switcher", "vela-switcher", layerOverlay, fill, -1, keysNone },
    { "NotificationPopups", "vela-notifications", layerOverlay, bottomRight, 0, keysNone },
    // Right-click menus: the whole output, transparent, above everything; a
    // click outside closes them. Arrows, Enter and Esc.
    { "ContextMenu", "vela-context-menu", layerOverlay, fill, -1, keysOnDemand },
    // "Run", at the bottom left above the taskbar, like in Windows.
    { "RunDialog", "vela-run", layerTop, bottomLeft, 0, keysOnDemand, QMargins(12, 0, 0, 12) },
    // Questions such as "Empty Recycle Bin", and what to share on screen.
    { "ConfirmDialog", "vela-confirm", layerOverlay, centered, 0, keysOnDemand },
    { "SourceChooser", "vela-share", layerOverlay, centered, 0, keysOnDemand },
    { "PropertiesDialog", "vela-properties", layerTop, centered, 0, keysOnDemand },
    { "QuickSettings", "vela-quick-settings", layerTop, bottomRight, 0, keysOnDemand },
    { "NotificationCenter", "vela-notification-center", layerTop, bottomRight, 0, keysOnDemand },
    // All the space above the taskbar, which stays visible and clickable,
    // like in Windows.
    { "TaskView", "vela-task-view", layerTop, fill, 0, keysExclusive },
    // The desktop's name in the center of the output.
    { "DesktopOsd", "vela-desktop-osd", layerOverlay, centered, -1, keysNone, {}, true },
    // The volume: above fullscreen windows too, like Windows 11. It takes the
    // mouse (drag, mute, wheel).
    { "VolumeOsd", "vela-volume-osd", layerOverlay, bottom, 0, keysNone, QMargins(0, 0, 0, 12) },
    // The whole output, transparent: a click outside the panel closes it.
    // The keyboard for Win+Z.
    { "SnapLayouts", "vela-snap-layouts", layerOverlay, fill, -1, keysOnDemand },
    // The output's usable area, so the spaces in twelfths match the windows'.
    { "SnapAssist", "vela-snap-assist", layerTop, fill, 0, keysExclusive },
    // The distance from the left edge is decided by the button
    // (ShellController::placeAtLeft).
    { "TaskbarPreview", "vela-taskbar-preview", layerTop, bottomLeft, 0, keysNone },
    // Above the taskbar too, placed the same way; the mouse goes through.
    { "TaskbarTooltip", "vela-taskbar-tooltip", layerTop, bottomLeft, 0, keysNone, {}, true },
    { "FileDialogs", "vela-file-dialogs", layerTop, centered, 0, keysOnDemand },
    { "ClipboardPanel", "vela-clipboard", layerTop, bottomRight, 0, keysOnDemand },
};

// QML windows start invisible: they become layer-shell surfaces BEFORE
// they're shown.
QQuickWindow* loadPanel(QQmlApplicationEngine& engine, const Panel& panel)
{
    const qsizetype loaded = engine.rootObjects().size();
    engine.loadFromModule("Vela.Shell", panel.qml);
    if (engine.rootObjects().size() == loaded) {
        return nullptr;
    }
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().last());
    if (!window) {
        return nullptr;
    }
    LayerWindow* layer = LayerWindow::get(window);
    layer->setScope(QString::fromLatin1(panel.scope));
    layer->setLayer(panel.layer);
    layer->setAnchors(panel.anchors);
    layer->setExclusiveZone(panel.exclusiveZone);
    layer->setMargins(panel.margins);
    layer->setKeyboardInteractivity(panel.keyboard);
    if (panel.clickThrough) {
        window->setFlag(Qt::WindowTransparentForInput);
    }
    // Panels come and go: a hidden one gives its surface back. The taskbar
    // and the wallpapers (ScreenWindows) stay mapped all session.
    releaseHiddenPanel(window);
    return window;
}

// With the taskbar aligned left, Start opens at the bottom left.
void placeStartMenu(QQuickWindow* window, const QString& alignment)
{
    LayerWindow* layer = LayerWindow::get(window);
    const bool left = alignment == QLatin1String("left");
    layer->setAnchors(left ? bottomLeft : bottom);
    layer->setMargins(QMargins(left ? 12 : 0, 0, 0, 0));
}

} // namespace

int main(int argc, char* argv[])
{
    // "vela-shell --choose-source": the chooser xdg-desktop-portal-wlr
    // launches when an app wants to share the screen. It asks the already open
    // shell, which shows the choice, and prints the answer for the portal.
    if (argc > 1 && QByteArray(argv[1]) == "--choose-source") {
        QCoreApplication app(argc, argv);
        const QByteArray answer = ShellController::askRunningInstance("choose-source");
        if (!answer.isEmpty()) {
            std::printf("%s\n", answer.constData());
        }
        return 0;
    }

    // Text with FreeType and font hinting, like Plasma and Windows: crisper
    // than Qt Quick's default text (distance fields, no hinting), on the
    // output's real pixels even at fractional scales.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_DEFAULT_TEXT_RENDER_TYPE")) {
        QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    QGuiApplication::setQuitOnLastWindowClosed(false);
    // Before anything else: the C++ models are born already translated too.
    QQmlApplicationEngine* qmlEngine = nullptr;
    AppModel* appModel = nullptr;
    DesktopModel* desktopModel = nullptr;
    vela::language::install(QStringLiteral("vela-shell"), [&qmlEngine, &appModel, &desktopModel] {
        if (appModel) {
            appModel->reload(); // app names in the new language (Name[en], Name[it])
        }
        if (desktopModel) {
            desktopModel->refresh(); // the Recycle Bin
        }
        if (qmlEngine) {
            qmlEngine->retranslate();
        }
    });

    // "vela-shell --toggle-start": handy for shortcuts and scripts.
    const QStringList args = QGuiApplication::arguments();
    if (args.contains(QStringLiteral("--toggle-start"))) {
        return ShellController::sendToRunningInstance("toggle-start") ? 0 : 1;
    }
    if (ShellController::sendToRunningInstance("ping")) {
        qWarning("vela-shell: already running in this session");
        return 0;
    }

    if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        qWarning("vela-shell: needs a Wayland session (current platform: %s)",
            qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    QIcon::setFallbackThemeName(QStringLiteral("hicolor"));

    Config config;

    AppModel apps;
    appModel = &apps;
    apps.reload();

    ShellController shell;
    Workspaces workspaces;
    QObject::connect(&shell, &ShellController::workspacesReceived, &workspaces, &Workspaces::update);
    workspaces.query();
    Accessibility accessibility;
    QObject::connect(&shell, &ShellController::accessibilityReceived, &accessibility, &Accessibility::update);
    accessibility.query();
    Network network;
    shell.listen();
    shell.sendWallpaperTint(config.wallpaper());
    QObject::connect(&config, &Config::wallpaperChanged, &shell, [&] { shell.sendWallpaperTint(config.wallpaper()); });
    // The mode to the compositor (acrylic tint, title bars).
    auto sendTheme = [&config] {
        ShellController::sendToCompositor("theme " + config.shellTheme().toLatin1() + ' ' + config.appTheme().toLatin1());
    };
    sendTheme();
    QObject::connect(&config, &Config::themeChanged, &shell, sendTheme);
    shell.watchSleep();

    ForeignToplevelManager windows;
    TaskbarModel tasks(&apps, &windows);
    WindowCapture capture;
    NotificationServer notifications;
    notifications.registerService();
    notifications.setDoNotDisturb(config.doNotDisturb());
    QObject::connect(&config, &Config::doNotDisturbChanged, &notifications, &NotificationServer::setDoNotDisturb);
    // Sharing the screen with capture-dnd=yes: popups held back (the center keeps them).
    auto silenceForSharing = [&] { notifications.setCaptureSilenced(shell.captureSilence()); };
    QObject::connect(&shell, &ShellController::screenCapturedChanged, &notifications, silenceForSharing);
    silenceForSharing();
    TrayModel tray;
    tray.start();
    SystemActions system;
    system.setAppModel(&apps);
    QObject::connect(&system, &SystemActions::chooseAppRequested, &shell, &ShellController::openWithRequested);
    QObject::connect(&shell, &ShellController::settingsRequested, &system, [&system] { system.trigger(QStringLiteral("settings")); });
    QObject::connect(&shell, &ShellController::filesRequested, &system, [&system] { system.trigger(QStringLiteral("files")); });
    JumpLists jumps(&apps);
    DesktopModel desktop(&apps);
    QObject::connect(&desktop, &DesktopModel::chooseAppRequested, &shell, &ShellController::openWithRequested);
    desktopModel = &desktop;
    ServiceMenus serviceMenus;
    FileProperties properties;
    FileActions fileActions;
    Clipboard clipboard;
    QObject::connect(&shell, &ShellController::clipboardClearRequested, &clipboard, &Clipboard::clear);
    // History turned on or off from Settings (vela-shell.conf).
    QObject::connect(&config, &Config::clipboardChanged, &clipboard,
        [&config, &clipboard] { clipboard.setEnabled(config.clipboardHistory()); });
    SystemStatus status;
    // The sound (docs/audio.md): apps named after their .desktop file.
    vela::audio::Audio audio;
    audio.setAppResolver(audioAppResolver(apps));
    // The mix follows what plays, unless someone chose otherwise (§8.1).
    audio.applyDefaultSampleRate();
    SoundKeys sound(&audio);
    QObject::connect(&shell, &ShellController::volumeKeyPressed, &sound, &SoundKeys::press);
    MediaKeys media;
    QObject::connect(&shell, &ShellController::mediaKeyPressed, &media, &MediaKeys::press);

    QQmlApplicationEngine engine;
    qmlEngine = &engine;
    // Theme, Style (the shell's mode, accent, icons) and Effects (the blur):
    // Vela.Controls, like the apps.
    vela::controls::install(engine, Appearance::Mode::Shell)->watch();
    engine.addImageProvider(QStringLiteral("fileicon"), new IconProvider(32));
    engine.addImageProvider(QStringLiteral("wallpaper"), new WallpaperProvider);
    engine.addImageProvider(QStringLiteral("thumbnail"), new ThumbnailProvider(&capture));
    engine.addImageProvider(QStringLiteral("filethumb"), new FileThumbnailProvider);
    engine.addImageProvider(QStringLiteral("notification"), new NotificationImageProvider(&notifications));
    engine.addImageProvider(QStringLiteral("tray"), new TrayImageProvider(&tray));
    engine.addImageProvider(QStringLiteral("clipboard"), new ClipboardImageProvider(&clipboard));
    Snip snip(&engine, &capture, &clipboard, &notifications);
    engine.addImageProvider(QStringLiteral("snip"), new SnipImageProvider(&snip));
    QObject::connect(&shell, &ShellController::snipRequested, &snip, &Snip::start);
    engine.rootContext()->setContextProperty(QStringLiteral("Apps"), &apps);
    engine.rootContext()->setContextProperty(QStringLiteral("Shell"), &shell);
    engine.rootContext()->setContextProperty(QStringLiteral("Config"), &config);
    engine.rootContext()->setContextProperty(QStringLiteral("Desktops"), &workspaces);
    engine.rootContext()->setContextProperty(QStringLiteral("Tasks"), &tasks);
    engine.rootContext()->setContextProperty(QStringLiteral("Capture"), &capture);
    engine.rootContext()->setContextProperty(QStringLiteral("Notifications"), &notifications);
    engine.rootContext()->setContextProperty(QStringLiteral("Tray"), &tray);
    engine.rootContext()->setContextProperty(QStringLiteral("System"), &system);
    engine.rootContext()->setContextProperty(QStringLiteral("Jumps"), &jumps);
    engine.rootContext()->setContextProperty(QStringLiteral("Desktop"), &desktop);
    engine.rootContext()->setContextProperty(QStringLiteral("ServiceMenus"), &serviceMenus);
    engine.rootContext()->setContextProperty(QStringLiteral("Properties"), &properties);
    engine.rootContext()->setContextProperty(QStringLiteral("FileActions"), &fileActions);
    engine.rootContext()->setContextProperty(QStringLiteral("Clip"), &clipboard);
    engine.rootContext()->setContextProperty(QStringLiteral("Snip"), &snip);
    engine.rootContext()->setContextProperty(QStringLiteral("Status"), &status);
    engine.rootContext()->setContextProperty(QStringLiteral("Audio"), &audio);
    engine.rootContext()->setContextProperty(QStringLiteral("Sound"), &sound);
    // The apps' level bars in the quick settings mixer (docs/audio.md §7.1).
    qmlRegisterType<vela::audio::LevelMeter>("Vela.Audio", 1, 0, "LevelMeter");
    engine.rootContext()->setContextProperty(QStringLiteral("Access"), &accessibility);
    engine.rootContext()->setContextProperty(QStringLiteral("Network"), &network);

    QQuickWindow* startMenu = nullptr;
    for (const Panel& panel : panels) {
        QQuickWindow* window = loadPanel(engine, panel);
        if (!window) {
            qCritical("vela-shell: can't load the QML interface (%s)", panel.qml);
            return 1;
        }
        if (qstrcmp(panel.qml, "StartMenu") == 0) {
            startMenu = window;
        }
    }
    placeStartMenu(startMenu, config.taskbarAlignment());
    QObject::connect(&config, &Config::taskbarChanged, startMenu, [&config, startMenu] {
        placeStartMenu(startMenu, config.taskbarAlignment());
    });
    // Wallpapers on every output; the taskbar too, like in Windows (or only on
    // the main one, if the user wants that).
    ScreenWindows wallpapers(&engine, "Wallpaper", setupWallpaper);
    ScreenWindows taskbars(&engine, "Taskbar", setupTaskbar);
    taskbars.setPrimaryOnly(!config.taskbarAllScreens());
    QObject::connect(&config, &Config::taskbarChanged, &taskbars,
        [&config, &taskbars] { taskbars.setPrimaryOnly(!config.taskbarAllScreens()); });
    if (!wallpapers.start() || !taskbars.start()) {
        return 1;
    }

    return app.exec();
}
