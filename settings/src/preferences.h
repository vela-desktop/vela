// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "defaults.h"

#include <QColor>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QObject>
#include <QStringList>
#include <QVariantList>

// The user's choices, in the two files holding them:
// ~/.config/Vela/vela-shell.conf for the shell (wallpaper, accent, mode,
// taskbar, notifications), which the shell rereads as soon as it changes;
// ~/.config/vela/vela.conf for the compositor (inactivity, keyboard, night
// light, accessibility, tearing), which rereads it on the "reload-config"
// command. The compositor writes vela.conf too (quick settings turn on night
// light, filters and sticky keys): only the changed keys are rewritten, on the
// file just reread, and it's watched. Also, the apps' light or dark mode goes
// to KDE (colors, icons) and GTK (gsettings).
class Preferences : public QObject {
    Q_OBJECT
    // --- Personalization ---
    Q_PROPERTY(QString wallpaper READ wallpaper NOTIFY wallpaperChanged)
    Q_PROPERTY(QStringList recentWallpapers READ recentWallpapers NOTIFY wallpaperChanged)
    Q_PROPERTY(QStringList systemWallpapers READ systemWallpapers CONSTANT)
    Q_PROPERTY(QString defaultWallpaper READ defaultWallpaper CONSTANT) // Vela's
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY accentChanged)
    Q_PROPERTY(QString appTheme READ appTheme WRITE setAppTheme NOTIFY appThemeChanged)
    // Vela's mode (taskbar, menus, panels): "dark" or "light".
    Q_PROPERTY(QString shellTheme READ shellTheme WRITE setShellTheme NOTIFY appThemeChanged)
    // Settings itself is light (it follows the apps' mode).
    Q_PROPERTY(bool light READ light NOTIFY appThemeChanged)
    Q_PROPERTY(QString taskbarAlignment READ taskbarAlignment WRITE setTaskbarAlignment NOTIFY taskbarChanged)
    Q_PROPERTY(bool endTask READ endTask WRITE setEndTask NOTIFY taskbarChanged)
    Q_PROPERTY(bool taskView READ taskView WRITE setTaskView NOTIFY taskbarChanged)
    Q_PROPERTY(bool taskbarAllScreens READ taskbarAllScreens WRITE setTaskbarAllScreens NOTIFY taskbarChanged)
    Q_PROPERTY(bool showDesktopCorner READ showDesktopCorner WRITE setShowDesktopCorner NOTIFY taskbarChanged)
    // --- Notifications ---
    Q_PROPERTY(bool doNotDisturb READ doNotDisturb WRITE setDoNotDisturb NOTIFY doNotDisturbChanged)
    // --- Power ---
    Q_PROPERTY(int screenOffMinutes READ screenOffMinutes WRITE setScreenOffMinutes NOTIFY idleChanged)
    Q_PROPERTY(bool lockOnIdle READ lockOnIdle WRITE setLockOnIdle NOTIFY idleChanged)
    // Vela's language (language= in vela.conf): "", "it" or "en". Empty: like
    // the system. The apps apply it themselves (shell/src/language.h).
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    // --- Keyboard ---
    // [{layout, variant}] in the chosen order; the first is the starting one.
    Q_PROPERTY(QVariantList keyboardLayouts READ keyboardLayouts NOTIFY keyboardChanged)
    Q_PROPERTY(int repeatDelay READ repeatDelay WRITE setRepeatDelay NOTIFY keyboardChanged)
    Q_PROPERTY(int repeatRate READ repeatRate WRITE setRepeatRate NOTIFY keyboardChanged)
    // --- Display: night light and games ---
    Q_PROPERTY(bool nightLight READ nightLight WRITE setNightLight NOTIFY nightLightChanged)
    // Kelvin, 1700-6500 (night-light-temperature; the older strength without it).
    Q_PROPERTY(int nightTemperature READ nightTemperature WRITE setNightTemperature NOTIFY nightLightChanged)
    Q_PROPERTY(bool nightNvidiaFix READ nightNvidiaFix WRITE setNightNvidiaFix NOTIFY nightLightChanged)
    // "no", "sunset" (from sunset to sunrise) or "hours"
    Q_PROPERTY(QString nightSchedule READ nightSchedule WRITE setNightSchedule NOTIFY nightLightChanged)
    Q_PROPERTY(QString nightFrom READ nightFrom WRITE setNightFrom NOTIFY nightLightChanged) // "21:00"
    Q_PROPERTY(QString nightTo READ nightTo WRITE setNightTo NOTIFY nightLightChanged)
    // Today's sun times (computed by the compositor), empty if unknown.
    Q_PROPERTY(QString sunset READ sunset NOTIFY nightLightChanged)
    Q_PROPERTY(QString sunrise READ sunrise NOTIFY nightLightChanged)
    Q_PROPERTY(bool tearing READ tearing WRITE setTearing NOTIFY accessibilityChanged)
    // Variable refresh rate: "no", "games" (fullscreen apps), "always".
    Q_PROPERTY(QString vrr READ vrr WRITE setVrr NOTIFY accessibilityChanged)
    Q_PROPERTY(QString directScanout READ directScanout WRITE setDirectScanout NOTIFY accessibilityChanged)
    // --- Mouse and touchpad (vela.conf) ---
    Q_PROPERTY(int mouseSpeed READ mouseSpeed WRITE setMouseSpeed NOTIFY inputChanged) // 1-20
    Q_PROPERTY(bool mousePrecision READ mousePrecision WRITE setMousePrecision NOTIFY inputChanged)
    Q_PROPERTY(bool mouseLeftHanded READ mouseLeftHanded WRITE setMouseLeftHanded NOTIFY inputChanged)
    Q_PROPERTY(int wheelLines READ wheelLines WRITE setWheelLines NOTIFY inputChanged)
    Q_PROPERTY(bool hasTouchpad READ hasTouchpad NOTIFY inputChanged) // the compositor says so
    Q_PROPERTY(bool touchpad READ touchpad WRITE setTouchpad NOTIFY inputChanged)
    Q_PROPERTY(bool touchpadWithMouse READ touchpadWithMouse WRITE setTouchpadWithMouse NOTIFY inputChanged)
    Q_PROPERTY(int touchpadSpeed READ touchpadSpeed WRITE setTouchpadSpeed NOTIFY inputChanged)
    Q_PROPERTY(bool touchpadTap READ touchpadTap WRITE setTouchpadTap NOTIFY inputChanged)
    Q_PROPERTY(bool touchpadNatural READ touchpadNatural WRITE setTouchpadNatural NOTIFY inputChanged)
    // "app", "desktop" or "no"
    Q_PROPERTY(QString threeFingers READ threeFingers WRITE setThreeFingers NOTIFY inputChanged)
    Q_PROPERTY(QString fourFingers READ fourFingers WRITE setFourFingers NOTIFY inputChanged)
    // --- Clipboard (vela-shell.conf) ---
    Q_PROPERTY(bool clipboardHistory READ clipboardHistory WRITE setClipboardHistory NOTIFY clipboardChanged)
    // --- Accessibility ---
    Q_PROPERTY(bool magnifier READ magnifier WRITE setMagnifier NOTIFY accessibilityChanged)
    Q_PROPERTY(int magnifierStep READ magnifierStep WRITE setMagnifierStep NOTIFY accessibilityChanged)
    Q_PROPERTY(bool colorFilter READ colorFilter WRITE setColorFilter NOTIFY accessibilityChanged)
    // grayscale, deuteranopia, protanopia, tritanopia
    Q_PROPERTY(QString colorFilterKind READ colorFilterKind WRITE setColorFilterKind NOTIFY accessibilityChanged)
    Q_PROPERTY(bool colorFilterShortcut READ colorFilterShortcut WRITE setColorFilterShortcut NOTIFY accessibilityChanged)
    Q_PROPERTY(bool stickyKeys READ stickyKeys WRITE setStickyKeys NOTIFY accessibilityChanged)
    Q_PROPERTY(bool animationEffects READ animationEffects WRITE setAnimationEffects NOTIFY accessibilityChanged)
    // Percent of Vela's animation durations (100: Vela's).
    Q_PROPERTY(int animationDuration READ animationDuration WRITE setAnimationDuration NOTIFY accessibilityChanged)

public:
    explicit Preferences(QObject* parent = nullptr);

    QString wallpaper() const { return m_wallpaper; }
    QString defaultWallpaper() const { return vela::defaults::wallpaper(); }
    QStringList recentWallpapers() const { return m_recentWallpapers; }
    QStringList systemWallpapers() const;
    Q_INVOKABLE void setWallpaper(const QString& pathOrUrl);

    QColor accent() const { return m_accent; }
    void setAccent(const QColor& color);

    QString appTheme() const { return m_appTheme; }
    void setAppTheme(const QString& theme);
    QString shellTheme() const { return m_shellTheme; }
    void setShellTheme(const QString& theme);
    bool light() const { return m_appTheme == QLatin1String("light"); }

    QString taskbarAlignment() const { return m_taskbarAlignment; }
    void setTaskbarAlignment(const QString& alignment);
    bool endTask() const { return m_endTask; }
    void setEndTask(bool on);
    bool taskView() const { return m_taskView; }
    void setTaskView(bool on);
    bool taskbarAllScreens() const { return m_taskbarAllScreens; }
    void setTaskbarAllScreens(bool on);
    bool showDesktopCorner() const { return m_showDesktopCorner; }
    void setShowDesktopCorner(bool on);

    bool doNotDisturb() const { return m_doNotDisturb; }
    void setDoNotDisturb(bool on);

    int screenOffMinutes() const { return m_screenOffMinutes; }
    void setScreenOffMinutes(int minutes);
    bool lockOnIdle() const { return m_lockOnIdle; }
    void setLockOnIdle(bool on);
    QString language() const { return m_language; }
    void setLanguage(const QString& language);

    QVariantList keyboardLayouts() const { return m_layouts; }
    Q_INVOKABLE void addKeyboardLayout(const QString& layout, const QString& variant);
    Q_INVOKABLE void removeKeyboardLayout(int index);
    Q_INVOKABLE void moveKeyboardLayoutUp(int index);
    int repeatDelay() const { return m_repeatDelay; }
    void setRepeatDelay(int ms);
    int repeatRate() const { return m_repeatRate; }
    void setRepeatRate(int perSecond);

    bool nightLight() const { return m_nightLight; }
    void setNightLight(bool on);
    int nightTemperature() const { return m_nightTemperature; }
    void setNightTemperature(int kelvin);
    /** Whether Night Light uses Vulkan tinting instead of the DRM gamma LUT. */
    bool nightNvidiaFix() const { return m_nightNvidiaFix; }
    void setNightNvidiaFix(bool on);
    QString nightSchedule() const { return m_nightSchedule; }
    void setNightSchedule(const QString& schedule);
    QString nightFrom() const { return m_nightFrom; }
    void setNightFrom(const QString& time);
    QString nightTo() const { return m_nightTo; }
    void setNightTo(const QString& time);
    QString sunset() const { return m_sunset; }
    QString sunrise() const { return m_sunrise; }
    bool tearing() const { return m_tearing; }
    void setTearing(bool on);
    QString vrr() const { return m_vrr; }
    void setVrr(const QString& mode);
    QString directScanout() const { return m_directScanout; }
    void setDirectScanout(const QString& mode);

    bool magnifier() const { return m_magnifier; }
    void setMagnifier(bool on);
    int magnifierStep() const { return m_magnifierStep; }
    void setMagnifierStep(int percent);
    bool colorFilter() const { return m_colorFilter; }
    void setColorFilter(bool on);
    QString colorFilterKind() const { return m_colorFilterKind; }
    void setColorFilterKind(const QString& kind);
    bool colorFilterShortcut() const { return m_colorFilterShortcut; }
    void setColorFilterShortcut(bool on);
    bool stickyKeys() const { return m_stickyKeys; }
    void setStickyKeys(bool on);
    bool animationEffects() const { return m_animationEffects; }
    void setAnimationEffects(bool on);
    int animationDuration() const { return m_animationDuration; }
    void setAnimationDuration(int percent);

    int mouseSpeed() const { return m_mouseSpeed; }
    void setMouseSpeed(int value);
    bool mousePrecision() const { return m_mousePrecision; }
    void setMousePrecision(bool on);
    bool mouseLeftHanded() const { return m_mouseLeftHanded; }
    void setMouseLeftHanded(bool on);
    int wheelLines() const { return m_wheelLines; }
    void setWheelLines(int lines);
    bool hasTouchpad() const { return m_hasTouchpad; }
    bool touchpad() const { return m_touchpad; }
    void setTouchpad(bool on);
    bool touchpadWithMouse() const { return m_touchpadWithMouse; }
    void setTouchpadWithMouse(bool on);
    int touchpadSpeed() const { return m_touchpadSpeed; }
    void setTouchpadSpeed(int value);
    bool touchpadTap() const { return m_touchpadTap; }
    void setTouchpadTap(bool on);
    bool touchpadNatural() const { return m_touchpadNatural; }
    void setTouchpadNatural(bool on);
    QString threeFingers() const { return m_threeFingers; }
    void setThreeFingers(const QString& action);
    QString fourFingers() const { return m_fourFingers; }
    void setFourFingers(const QString& action);

    bool clipboardHistory() const { return m_clipboardHistory; }
    void setClipboardHistory(bool on);
    // "Clear clipboard data": the history (except pinned items), in the shell.
    Q_INVOKABLE void clearClipboard();


    // Rereads everything (the shell or another program may have changed
    // something).
    Q_INVOKABLE void reload();

signals:
    void wallpaperChanged();
    void accentChanged();
    void appThemeChanged();
    void taskbarChanged();
    void doNotDisturbChanged();
    void idleChanged();
    void languageChanged();
    void keyboardChanged();
    void nightLightChanged();
    void inputChanged();
    void clipboardChanged();
    void accessibilityChanged();

private:
    void setShell(const QString& key, const QVariant& value);
    // vela.conf: reread, only these keys changed, then "reload-config".
    void saveCompositor(const QList<QPair<QString, QString>>& changes);
    void saveCompositorKeys(const QStringList& keys); // the current values of these keys
    QString compositorValue(const QString& key) const;
    void reloadCompositor(); // vela.conf and the compositor's state
    void queryCompositor(); // night light, magnifier...: the current state
    void watchConfig();
    void saveLayouts();

    QString m_wallpaper;
    QStringList m_recentWallpapers;
    QColor m_accent;
    QString m_appTheme;
    QString m_shellTheme;
    QString m_taskbarAlignment;
    bool m_endTask = true;
    bool m_taskView = true;
    bool m_taskbarAllScreens = true;
    bool m_showDesktopCorner = true;
    bool m_doNotDisturb = false;
    int m_screenOffMinutes = 10;
    bool m_lockOnIdle = true;
    QString m_language;
    QVariantList m_layouts;
    int m_repeatDelay = 400;
    int m_repeatRate = 30;
    bool m_nightLight = false;
    int m_nightTemperature = 4200;
    bool m_nightNvidiaFix = false;
    QString m_nightSchedule = QStringLiteral("no");
    QString m_nightFrom = QStringLiteral("21:00");
    QString m_nightTo = QStringLiteral("07:00");
    QString m_sunset;
    QString m_sunrise;
    bool m_tearing = true;
    QString m_vrr = QStringLiteral("games");
    QString m_directScanout = QStringLiteral("auto");
    bool m_magnifier = false;
    int m_magnifierStep = 100;
    bool m_colorFilter = false;
    QString m_colorFilterKind = QStringLiteral("grayscale");
    bool m_colorFilterShortcut = false;
    bool m_stickyKeys = false;
    bool m_animationEffects = true;
    int m_animationDuration = 100;
    int m_mouseSpeed = 10;
    bool m_mousePrecision = true;
    bool m_mouseLeftHanded = false;
    int m_wheelLines = 3;
    bool m_hasTouchpad = false;
    bool m_touchpad = true;
    bool m_touchpadWithMouse = true;
    int m_touchpadSpeed = 10;
    bool m_touchpadTap = true;
    bool m_touchpadNatural = true;
    QString m_threeFingers = QStringLiteral("app");
    QString m_fourFingers = QStringLiteral("desktop");
    bool m_clipboardHistory = false;
    QList<QPair<QString, QString>> m_compositor; // vela.conf, in file order
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};

// Sends a command to the compositor (vela-<display>.sock).
void sendToCompositor(const QByteArray& command);
