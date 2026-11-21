// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "processlaunch.h"
#include "preferences.h"

#include "../../common/taskbarsettings.h"
#include "../../common/privatefile.h"

extern "C" {
#include "color.h"
#include "legacy_names.h"
}

#include "batterysettings.h"
#include "clockformat.h"
#include "iconprovider.h"
#include "themefile.h"

#include <QDir>
#include <QTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QIcon>
#include <QImageReader>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTemporaryFile>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {


QSettings shellSettings()
{
    return QSettings(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
}

QString compositorConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/vela/vela.conf");
}

QString kdeConfigPath(const QString& file)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + file;
}

QString kdeValue(const QString& file, const QString& group, const QString& key)
{
    QSettings settings(kdeConfigPath(file), QSettings::IniFormat);
    return settings.value(group + u'/' + key).toString();
}

QStringList installedThemes(const QString& subdirectory, const QString& marker)
{
    QStringList themes;
    QStringList roots;
    for (const QString& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        roots.append(data + u'/' + subdirectory);
    }
    if (subdirectory == QLatin1String("icons")) {
        roots.append(QDir::homePath() + QStringLiteral("/.icons"));
    }
    for (const QString& root : roots) {
        const QDir dir(root);
        for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString name = entry.fileName();
            if (QFileInfo::exists(entry.filePath() + u'/' + marker)
                && std::none_of(name.cbegin(), name.cend(), [](QChar ch) {
                       const ushort value = ch.unicode();
                       return value < 32 || (value >= 127 && value <= 159);
                   })) {
                themes.append(name);
            }
        }
    }
    themes.removeDuplicates();
    themes.sort(Qt::CaseInsensitive);
    return themes;
}

bool writeKdeValue(const QString& file, const QString& group, const QString& key, const QString& value)
{
    const QString writer = QStandardPaths::findExecutable(QStringLiteral("kwriteconfig6"));
    if (!writer.isEmpty()) {
        return QProcess::execute(writer,
            { QStringLiteral("--notify"), QStringLiteral("--file"), file, QStringLiteral("--group"), group,
                QStringLiteral("--key"), key, value }) == 0;
    }
    QSettings settings(kdeConfigPath(file), QSettings::IniFormat);
    settings.setValue(group + u'/' + key, value);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void deleteKdeValue(const QString& file, const QString& group, const QString& key)
{
    const QString writer = QStandardPaths::findExecutable(QStringLiteral("kwriteconfig6"));
    if (!writer.isEmpty()) {
        QProcess::execute(writer,
            { QStringLiteral("--notify"), QStringLiteral("--file"), file, QStringLiteral("--group"), group,
                QStringLiteral("--key"), key, QStringLiteral("--delete") });
    } else {
        QSettings settings(kdeConfigPath(file), QSettings::IniFormat);
        settings.remove(group + u'/' + key);
        settings.sync();
    }
}

QString withFontFamily(QString value, const QString& family)
{
    QFont font;
    if (!font.fromString(value)) {
        font = QGuiApplication::font();
    }
    font.setFamily(family);
    return font.toString();
}

} // namespace

void sendToCompositor(const QByteArray& command)
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock"));
    if (socket.waitForConnected(200)) {
        socket.write(command + '\n');
        socket.waitForBytesWritten(200);
    }
}

Preferences::Preferences(QObject* parent)
    : QObject(parent)
{
    reload();
    // vela.conf changes outside of here too (the shell's quick settings).
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        watchConfig();
        reloadCompositor();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    watchConfig();
    // The theme files: a theme added, removed or edited shows in Themes
    // (themesRevision) without touching the theme in use or a preview.
    m_themesDebounce.setSingleShot(true);
    m_themesDebounce.setInterval(150);
    connect(&m_themesDebounce, &QTimer::timeout, this, [this] {
        watchThemes();
        ++m_themesRevision;
        emit themesChanged();
    });
    connect(&m_themesWatcher, &QFileSystemWatcher::fileChanged, &m_themesDebounce, qOverload<>(&QTimer::start));
    connect(&m_themesWatcher, &QFileSystemWatcher::directoryChanged, &m_themesDebounce, qOverload<>(&QTimer::start));
    watchThemes();
}

void Preferences::watchThemes()
{
    // The directories for files added and removed, the files for edits; an
    // editor that replaces a file drops it from the watcher, so add it again.
    // The themes' and every surface's styles'; while the user's own don't
    // exist yet, the directory above them, so that creating them is noticed.
    for (int i = 0; i < vela::themes::LayerCount; ++i) {
        const auto layer = vela::themes::Layer(i);
        for (const QString& dir : vela::themes::watchedDirectories(layer)) {
            if (!m_themesWatcher.directories().contains(dir)) {
                m_themesWatcher.addPath(dir);
            }
        }
        for (const QString& dir : vela::themes::directories(layer)) {
            for (const QString& file : QDir(dir).entryList({ QStringLiteral("*.conf") }, QDir::Files)) {
                const QString path = dir + u'/' + file;
                if (!m_themesWatcher.files().contains(path)) {
                    m_themesWatcher.addPath(path);
                }
            }
        }
    }
    // The pictures' folders too: one added shows in the galleries at once.
    for (const QString& dir : vela::themes::pictureDirectories()) {
        if (!m_themesWatcher.directories().contains(dir)) {
            m_themesWatcher.addPath(dir);
        }
    }
}

void Preferences::watchConfig()
{
    const QString path = compositorConfigPath();
    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    if (info.exists() && !m_watcher.files().contains(path)) {
        m_watcher.addPath(path);
    }
    if (!m_watcher.directories().contains(info.absolutePath())) {
        m_watcher.addPath(info.absolutePath());
    }
}

/** Reload persisted appearance and shell choices, normalizing absent or invalid settings. */
void Preferences::reload()
{
    QSettings settings = shellSettings();
    settings.sync();
    loadShell(settings);
    reloadCompositor();
}

void Preferences::loadShell(const QSettings& settings)
{
    QString wallpaper = settings.value(QStringLiteral("appearance/wallpaper")).toString();
    if (wallpaper.isEmpty() || !QFileInfo::exists(wallpaper)) {
        wallpaper = vela::defaults::wallpaper();
    }
    const QStringList recent = settings.value(QStringLiteral("appearance/recentWallpapers")).toStringList();
    if (wallpaper != m_wallpaper || recent != m_recentWallpapers) {
        m_wallpaper = wallpaper;
        m_recentWallpapers = recent;
        emit wallpaperChanged();
    }

    QColor accent(settings.value(QStringLiteral("appearance/accent")).toString());
    if (!accent.isValid()) {
        accent = vela::defaults::accent();
    }
    QString source = settings.value(QStringLiteral("appearance/accentSource")).toString();
    if (source != QLatin1String("wallpaper") && source != QLatin1String("chosen")) {
        source = QStringLiteral("theme");
    }
    if (accent != m_accent || source != m_accentSource) {
        m_accent = accent;
        m_accentSource = source;
        emit accentChanged();
    }

    m_taskbarAlignment = settings.value(QStringLiteral("taskbar/alignment")).toString() == QLatin1String("left")
        ? QStringLiteral("left")
        : QStringLiteral("center");
    m_taskbarPosition = vela::taskbar::position(settings);
    m_endTask = settings.value(QStringLiteral("taskbar/endTask"), true).toBool();
    m_taskView = settings.value(QStringLiteral("taskbar/taskView"), true).toBool();
    m_taskbarAllScreens = settings.value(QStringLiteral("taskbar/allScreens"), true).toBool();
    m_showDesktopCorner = settings.value(QStringLiteral("taskbar/showDesktopCorner"), true).toBool();
    m_taskbarWindowsFrom = settings.value(QStringLiteral("taskbar/windowsFrom")).toString() == QLatin1String("all")
        ? QStringLiteral("all")
        : QStringLiteral("desktop");
    m_taskbarAppsOn = settings.value(QStringLiteral("taskbar/appsOn")).toString() == QLatin1String("window")
        ? QStringLiteral("window")
        : QStringLiteral("all");
    m_taskbarClickActive = settings.value(QStringLiteral("taskbar/clickActive")).toString() == QLatin1String("next")
        ? QStringLiteral("next")
        : QStringLiteral("previews");
    m_taskbarTrayShown = settings.value(QStringLiteral("taskbar/trayShown")).toStringList();
    for (QString& key : m_taskbarTrayShown) {
        key = key.trimmed();
    }
    m_taskbarTrayShown.removeAll(QString());
    m_taskbarTrayShown.removeDuplicates();
    m_taskbarAttention = vela::taskbar::enabled(settings, QStringLiteral("taskbar/attention"));
    m_taskbarBadges = vela::taskbar::enabled(settings, QStringLiteral("taskbar/badges"));
    m_taskbarProgress = vela::taskbar::enabled(settings, QStringLiteral("taskbar/progress"));
    m_taskbarNumberHints = vela::taskbar::enabled(settings, QStringLiteral("taskbar/numberHints"));
    emit taskbarChanged();
    m_desktopIcons = settings.value(QStringLiteral("desktop/showIcons"), true).toBool();
    emit desktopChanged();
    QStringList hidden = settings.value(QStringLiteral("menus/hidden")).toStringList();
    hidden.removeAll(QString());
    if (hidden != m_hiddenMenuEntries) {
        m_hiddenMenuEntries = hidden;
        emit menusChanged();
    }
    m_startPinned = settings.value(QStringLiteral("start/showPinned"), true).toBool();
    const vela::start::Icon startIcon = vela::start::icon(settings);
    m_startIcon = startIcon.kind;
    m_startIconSource = startIcon.source;
    emit startChanged();
    m_clockDate = settings.value(QStringLiteral("taskbar/showDate"), true).toBool();
    m_clock24 = vela::clock::hour24(settings);
    m_clockSeconds = settings.value(QStringLiteral("taskbar/clockSeconds"), false).toBool();
    m_notificationCount = settings.value(QStringLiteral("taskbar/notificationCount"), true).toBool();
    emit clockChanged();
    m_doNotDisturb = settings.value(QStringLiteral("notifications/doNotDisturb"), false).toBool();
    m_silenceWhileSharing = settings.value(QStringLiteral("notifications/silenceWhileSharing"), true).toBool();
    emit doNotDisturbChanged();
    m_batteryNotify = vela::battery::notify(settings);
    m_batteryLowLevel = vela::battery::lowLevel(settings);
    m_deviceBatteryNotify = vela::battery::devices(settings);
    emit batteryChanged();
    m_clipboardHistory = settings.value(QStringLiteral("clipboard/history"), false).toBool();
    emit clipboardChanged();

    // The mode: the one chosen here (vela-shell.conf); for apps, if not set
    // yet, the one GTK and the portal already know.
    QString appTheme = settings.value(QStringLiteral("appearance/appTheme")).toString();
    if (appTheme.isEmpty()) {
        vela::process::ExternalProcess gsettings;
        gsettings.start(QStringLiteral("gsettings"),
            { QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("color-scheme") });
        appTheme = gsettings.waitForFinished(1000) && gsettings.readAllStandardOutput().contains("light")
            ? QStringLiteral("light")
            : QStringLiteral("dark");
    }
    m_appTheme = appTheme == QLatin1String("light") ? QStringLiteral("light") : QStringLiteral("dark");
    m_shellTheme = settings.value(QStringLiteral("appearance/shellTheme")).toString() == QLatin1String("light")
        ? QStringLiteral("light")
        : QStringLiteral("dark");
    emit appThemeChanged();
}

namespace {

// A vela.conf line with the current names (compositor/src/legacy_names.h).
QString modernLine(const QString& line)
{
    const qsizetype eq = line.indexOf(u'=');
    if (line.isEmpty() || line.startsWith(u'#') || eq < 0) {
        return line;
    }
    const QByteArray key = line.left(eq).toUtf8();
    const QByteArray value = line.mid(eq + 1).toUtf8();
    const char* newKey = vela_legacy_key(key.constData());
    const char* newValue = vela_legacy_value(newKey ? newKey : key.constData(), value.constData());
    if (!newKey && !newValue) {
        return line;
    }
    return QString::fromUtf8(newKey ? QByteArray(newKey) : key) + u'='
        + QString::fromUtf8(newValue ? QByteArray(newValue) : value);
}

} // namespace

/** Refresh compositor settings, including the Night Light backend toggle. */
void Preferences::reloadCompositor()
{
    // vela.conf
    m_compositor.clear();
    QFile file(compositorConfigPath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = modernLine(in.readLine());
            const qsizetype eq = line.indexOf(u'=');
            if (!line.startsWith(u'#') && eq > 0) {
                m_compositor.append({ line.left(eq), line.mid(eq + 1) });
            }
        }
    }
    auto value = [this](const QString& key, const QString& fallback) {
        const QString v = compositorValue(key);
        return v.isNull() ? fallback : v;
    };
    auto flag = [&](const QString& key, bool fallback) {
        const QString v = value(key, fallback ? QStringLiteral("yes") : QStringLiteral("no"));
        return v == QLatin1String("yes") || v == QLatin1String("1") || v == QLatin1String("true");
    };
    const QString numberKeys = value(QStringLiteral("super-number-keys"), {});
    m_superNumberKeys = numberKeys == QLatin1String("desktops") || numberKeys == QLatin1String("off")
        ? numberKeys
        : QStringLiteral("apps");
    emit taskbarChanged();
    m_screenOffMinutes = value(QStringLiteral("screen-off"), QStringLiteral("10")).toInt();
    const QString lock = value(QStringLiteral("lock-on-idle"), QStringLiteral("yes"));
    m_lockOnIdle = lock != QLatin1String("0") && lock != QLatin1String("no");
    emit idleChanged();
    m_language = value(QStringLiteral("language"), {});
    emit languageChanged();
    // Temporary user-selected credential backend (applies at next login).
    const QString backend = compositorValue(QStringLiteral("secret-backend"));
    m_secretBackendConfigured = backend == QLatin1String("gnome") || backend == QLatin1String("kwallet");
    m_secretBackend = backend == QLatin1String("gnome") ? QStringLiteral("gnome") : QStringLiteral("kwallet");
    emit secretBackendChanged();
    // As the shell reads it (themefile.h's configuredName()). Only a change
    // is announced: the Themes page keeps a preview until then.
    QString theme = value(QStringLiteral("theme"), {}).trimmed();
    if (theme.isEmpty()) {
        theme = vela::themes::defaultTheme();
    }
    // Each surface's own style: "" for none.
    QVariantMap styles;
    for (int i = vela::themes::Taskbar; i < vela::themes::LayerCount; ++i) {
        const auto& info = vela::themes::layerInfo(vela::themes::Layer(i));
        styles.insert(QLatin1String(info.directory), value(QLatin1String(info.key), {}).trimmed());
    }
    if (theme != m_visualTheme || styles != m_surfaceStyles) {
        m_visualTheme = theme;
        m_surfaceStyles = styles;
        emit visualThemeChanged();
    }

    m_layouts.clear();
    const QStringList layouts = value(QStringLiteral("keyboard-layout"), {}).split(u',', Qt::SkipEmptyParts);
    const QStringList variants = value(QStringLiteral("keyboard-variant"), {}).split(u',');
    for (qsizetype i = 0; i < layouts.size(); ++i) {
        m_layouts.append(QVariantMap { { QStringLiteral("layout"), layouts[i].trimmed() },
            { QStringLiteral("variant"), i < variants.size() ? variants[i].trimmed() : QString() } });
    }
    m_repeatDelay = value(QStringLiteral("keyboard-repeat-delay"), QStringLiteral("400")).toInt();
    m_repeatRate = value(QStringLiteral("keyboard-repeat-rate"), QStringLiteral("30")).toInt();
    emit keyboardChanged();

    m_nightLight = flag(QStringLiteral("night-light"), false);
    {
        const QByteArray temperature = value(QStringLiteral("night-light-temperature"), {}).toUtf8();
        const QByteArray strength = value(QStringLiteral("night-light-strength"), {}).toUtf8();
        m_nightTemperature = vela_night_temperature(temperature.constData(), strength.constData());
    }
    m_nightNvidiaFix = flag(QStringLiteral("night-light-nvidia-fix"), false);
    m_nightSchedule = value(QStringLiteral("night-light-schedule"), QStringLiteral("no"));
    m_nightFrom = value(QStringLiteral("night-light-from"), QStringLiteral("21:00"));
    m_nightTo = value(QStringLiteral("night-light-to"), QStringLiteral("07:00"));
    m_tearing = flag(QStringLiteral("tearing"), true);
    m_vrr = value(QStringLiteral("variable-refresh"), QStringLiteral("games"));
    // "yes" is a compatibility alias for "auto"; there is no force-on mode.
    const QString scanoutMode = value(QStringLiteral("direct-scanout"), QStringLiteral("auto"));
    m_directScanout = scanoutMode == QLatin1String("no") ? QStringLiteral("no") : QStringLiteral("auto");
    m_colorFilter = flag(QStringLiteral("color-filters"), false);
    m_colorFilterKind = value(QStringLiteral("color-filter"), QStringLiteral("grayscale"));
    m_colorFilterShortcut = flag(QStringLiteral("color-filters-shortcut"), false);
    m_magnifierStep = value(QStringLiteral("magnifier-step"), QStringLiteral("100")).toInt();
    m_stickyKeys = flag(QStringLiteral("sticky-keys"), false);
    m_animationEffects = flag(QStringLiteral("animation-effects"), true);
    m_animationDuration = qBound(50, value(QStringLiteral("animation-duration"), QStringLiteral("100")).toInt(), 300);
    m_mouseSpeed = std::clamp(value(QStringLiteral("mouse-speed"), QStringLiteral("10")).toInt(), 1, 20);
    m_mousePrecision = flag(QStringLiteral("mouse-precision"), true);
    m_mouseLeftHanded = value(QStringLiteral("mouse-primary-button"), QStringLiteral("left")) == QLatin1String("right");
    m_wheelLines = std::clamp(value(QStringLiteral("mouse-scroll-lines"), QStringLiteral("3")).toInt(), 1, 20);
    m_touchpad = flag(QStringLiteral("touchpad"), true);
    m_touchpadWithMouse = flag(QStringLiteral("touchpad-with-mouse"), true);
    m_touchpadSpeed = std::clamp(value(QStringLiteral("touchpad-speed"), QStringLiteral("10")).toInt(), 1, 20);
    m_touchpadTap = flag(QStringLiteral("touchpad-tap"), true);
    m_touchpadNatural = flag(QStringLiteral("touchpad-natural-scroll"), true);
    m_threeFingers = value(QStringLiteral("touchpad-three-fingers"), QStringLiteral("app"));
    m_fourFingers = value(QStringLiteral("touchpad-four-fingers"), QStringLiteral("desktop"));
    emit inputChanged();
    queryCompositor();
    emit nightLightChanged();
    emit accessibilityChanged();
    if (!m_saveError.isEmpty()) {
        publishSaveError();
    }
}

QString Preferences::compositorValue(const QString& key) const
{
    // The last line of a key, as the compositor and the shell read it.
    for (auto it = m_compositor.crbegin(); it != m_compositor.crend(); ++it) {
        if (it->first == key) {
            return it->second;
        }
    }
    return {};
}

void Preferences::queryCompositor()
{
    // Query the actual compositor process: Settings may not inherit its
    // environment (VELA_SCANOUT=0 is a session-level override).
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    const QString socketPath = runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock");
    auto query = [&socketPath](const QByteArray& command) -> QJsonObject {
        QLocalSocket socket;
        socket.connectToServer(socketPath);
        if (!socket.waitForConnected(200)) {
            return {};
        }
        socket.write(command + '\n');
        if (!socket.waitForBytesWritten(200)) {
            return {};
        }
        QByteArray reply;
        while (!reply.contains('\n') && socket.waitForReadyRead(200)) {
            reply += socket.readAll();
        }
        return QJsonDocument::fromJson(reply.trimmed()).object();
    };

    const QJsonObject state = query("accessibility");
    if (!state.isEmpty()) {
        m_magnifier = state[QStringLiteral("magnifier")].toBool();
        m_hasTouchpad = state[QStringLiteral("touchpad")].toBool();
        m_sunset = state[QStringLiteral("sunset")].toString();
        m_sunrise = state[QStringLiteral("sunrise")].toString();
    }
    const QJsonObject policy = query("scanout-policy");
    if (!policy.isEmpty()) {
        m_scanoutOverridden = policy[QStringLiteral("envDisabled")].toBool();
    }
}

bool Preferences::setShell(const QString& key, const QVariant& value)
{
    return saveShell({ { key, value } });
}

bool Preferences::saveShell(const QList<QPair<QString, QVariant>>& changes)
{
    return saveShell([&changes](QSettings& staged) {
        for (const auto& [key, value] : changes) {
            staged.setValue(key, value);
        }
    });
}

bool Preferences::saveShell(const std::function<void(QSettings&)>& edit)
{
    QSettings current = shellSettings();
    current.sync();
    const QString path = current.fileName();
    const auto failed = [this, &path] {
        // Restore from the current persisted choices, including changes
        // another writer made since the UI last loaded. Never load defaults
        // over the shown values when the file itself cannot be read.
        const QFileInfo info(path);
        if (info.isFile() && info.isReadable()) {
            QSettings saved = shellSettings();
            saved.sync();
            (void)saved.allKeys();
            if (saved.status() == QSettings::NoError) {
                loadShell(saved);
            }
        }
        m_shellSaveFailed = true;
        publishSaveError();
        return false;
    };
    (void)current.allKeys();
    if (current.status() != QSettings::NoError) {
        return failed();
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return failed();
    }
    // Cooperate with other QSettings writers while reading and replacing
    // this file. Do not wait on a busy writer on the GUI thread.
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock()) {
        return failed();
    }
    QByteArray original;
    QFile source(path);
    if (source.exists()) {
        if (!source.open(QIODevice::ReadOnly)) {
            return failed();
        }
        original = source.readAll();
        if (source.error() != QFileDevice::NoError) {
            return failed();
        }
        source.close();
    }

    // QSettings cannot discard pending writes. Serialize into a unique
    // temporary file instead, so a failed write has no pending mutations in
    // the shared cache for vela-shell.conf, even after the path is repaired.
    QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/vela-settings-XXXXXX.ini"));
    if (!temporary.open() || temporary.write(original) != original.size() || !temporary.flush()) {
        return failed();
    }
    temporary.close();
    {
        QSettings staged(temporary.fileName(), QSettings::IniFormat);
        (void)staged.allKeys(); // reject malformed input before changing it
        if (staged.status() != QSettings::NoError) {
            return failed();
        }
        edit(staged);
        staged.sync();
        if (staged.status() != QSettings::NoError) {
            return failed();
        }
    }
    QFile serialized(temporary.fileName());
    if (!serialized.open(QIODevice::ReadOnly)) {
        return failed();
    }
    QByteArray data = serialized.readAll();
    if (serialized.error() != QFileDevice::NoError) {
        return failed();
    }
    // QSettings caches by size and timestamp. Ensure equal-size changes
    // made within one timestamp tick are still seen by existing readers.
    if (data.size() == original.size()) {
        data.append('\n');
    }
    QSaveFile destination(path);
    if (!vela::openPrivateFile(destination) || destination.write(data) != data.size() || !destination.commit()) {
        return failed();
    }
    // This instance never staged writes to the real file, so sync only
    // refreshes the shared reader cache; the native writer lock stays ours.
    current.sync();
    m_shellSaveFailed = false;
    publishSaveError();
    return true;
}

// ---------------------------------------------------------- wallpaper --

QStringList Preferences::systemWallpapers() const
{
    // Installed wallpapers: KDE packages (a folder with
    // contents/images/<resolution>.<ext>, of which the largest is taken) and
    // loose images from GNOME and the like.
    QStringList out;
    const QStringList roots = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("wallpapers"),
        QStandardPaths::LocateDirectory)
        + QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("backgrounds"),
            QStandardPaths::LocateDirectory);
    const QStringList images { QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
        QStringLiteral("*.webp"), QStringLiteral("*.svg") };
    for (const QString& root : roots) {
        const QDir dir(root);
        for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
            if (entry.isDir()) {
                QDir contents(entry.filePath() + QStringLiteral("/contents/images"));
                QFileInfoList candidates = contents.entryInfoList(images, QDir::Files);
                if (candidates.isEmpty()) {
                    continue;
                }
                // The largest by area (names are "3840x2160.png").
                auto area = [](const QFileInfo& f) {
                    const QStringList wh = f.completeBaseName().split(u'x');
                    return wh.size() == 2 ? wh[0].toLongLong() * wh[1].toLongLong() : 0LL;
                };
                std::sort(candidates.begin(), candidates.end(),
                    [&](const QFileInfo& a, const QFileInfo& b) { return area(a) > area(b); });
                out.append(candidates.first().filePath());
            } else if (QDir::match(images, entry.fileName())) {
                out.append(entry.filePath());
            }
        }
    }
    return out;
}

void Preferences::setWallpaper(const QString& pathOrUrl)
{
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    if (path.isEmpty()) {
        return;
    }
    // Recent images, like Windows: the last five, without Vela's.
    QStringList recent = m_recentWallpapers;
    recent.removeAll(path);
    if (!path.startsWith(u':')) {
        recent.prepend(path);
    }
    while (recent.size() > 5) {
        recent.removeLast();
    }
    if (!saveShell({ { QStringLiteral("appearance/wallpaper"), path.startsWith(u':') ? QString() : path },
            { QStringLiteral("appearance/recentWallpapers"), recent } })) {
        emit wallpaperChanged();
        return;
    }
    m_wallpaper = path;
    m_recentWallpapers = recent;
    emit wallpaperChanged();
}

// --------------------------------------------------- colors and theme --

QString Preferences::systemFont() const
{
    QFont font;
    if (!font.fromString(kdeValue(QStringLiteral("kdeglobals"), QStringLiteral("General"), QStringLiteral("font")))) {
        font = QGuiApplication::font();
    }
    return font.family();
}

QStringList Preferences::systemFonts() const
{
    QStringList fonts = QFontDatabase::families();
    fonts.sort(Qt::CaseInsensitive);
    const QString current = systemFont();
    if (!current.isEmpty() && !fonts.contains(current, Qt::CaseInsensitive)) {
        fonts.prepend(current);
    }
    return fonts;
}

bool Preferences::desktopThemesAvailable() const
{
    return true;
}

void Preferences::setSystemFont(const QString& family)
{
    if (!desktopThemesAvailable() || family == systemFont() || !QFontDatabase::families().contains(family)) {
        return;
    }
    QSettings settings(kdeConfigPath(QStringLiteral("kdeglobals")), QSettings::IniFormat);
    QList<QPair<QString, QString>> changed;
    const auto writeFont = [&](const QString& group, const QString& key) {
        const QString oldValue = settings.value(group + u'/' + key).toString();
        const QString newValue = withFontFamily(oldValue, family);
        if (!writeKdeValue(QStringLiteral("kdeglobals"), group, key, newValue)) {
            for (auto it = changed.crbegin(); it != changed.crend(); ++it) {
                const QString path = it->first + u'/' + it->second;
                if (settings.contains(path)) {
                    writeKdeValue(QStringLiteral("kdeglobals"), it->first, it->second, settings.value(path).toString());
                } else {
                    deleteKdeValue(QStringLiteral("kdeglobals"), it->first, it->second);
                }
            }
            emit desktopThemesChanged();
            return false;
        }
        changed.append({ group, key });
        return true;
    };
    for (const QString& key : { QStringLiteral("font"), QStringLiteral("menuFont"), QStringLiteral("toolBarFont"),
             QStringLiteral("windowTitleFont"), QStringLiteral("taskFont") }) {
        if (!writeFont(QStringLiteral("General"), key)) {
            return;
        }
    }
    if (!writeFont(QStringLiteral("WM"), QStringLiteral("activeFont"))) {
        return;
    }
    QFont font = QGuiApplication::font();
    font.setFamily(family);
    QGuiApplication::setFont(font);
    sendToCompositor("reload-font");
    emit desktopThemesChanged();
}

QString Preferences::iconTheme() const
{
    const QString configured = kdeValue(QStringLiteral("kdeglobals"), QStringLiteral("Icons"), QStringLiteral("Theme"));
    return configured.isEmpty() ? QIcon::themeName() : configured;
}

QStringList Preferences::iconThemes() const
{
    QStringList themes = installedThemes(QStringLiteral("icons"), QStringLiteral("index.theme"));
    const QString current = iconTheme();
    if (!current.isEmpty() && !themes.contains(current)) {
        themes.prepend(current);
    }
    return themes;
}

void Preferences::setIconTheme(const QString& theme)
{
    if (!desktopThemesAvailable() || theme == iconTheme() || !iconThemes().contains(theme)) {
        return;
    }
    if (!writeKdeValue(QStringLiteral("kdeglobals"), QStringLiteral("Icons"), QStringLiteral("Theme"), theme)) {
        emit desktopThemesChanged();
        return;
    }
    applyIconTheme(m_appTheme == QLatin1String("light"));
    sendToCompositor("reload-icons");
    emit desktopThemesChanged();
}

QString Preferences::cursorTheme() const
{
    const QString configured = kdeValue(QStringLiteral("kcminputrc"), QStringLiteral("Mouse"), QStringLiteral("cursorTheme"));
    return configured.isEmpty() ? qEnvironmentVariable("XCURSOR_THEME", QStringLiteral("default")) : configured;
}

QStringList Preferences::cursorThemes() const
{
    QStringList themes = installedThemes(QStringLiteral("icons"), QStringLiteral("cursors/left_ptr"));
    const QString current = cursorTheme();
    if (!current.isEmpty() && !themes.contains(current)) {
        themes.prepend(current);
    }
    return themes;
}

void Preferences::setCursorTheme(const QString& theme)
{
    if (!desktopThemesAvailable() || theme == cursorTheme() || !cursorThemes().contains(theme)
        || !writeKdeValue(QStringLiteral("kcminputrc"), QStringLiteral("Mouse"), QStringLiteral("cursorTheme"), theme)) {
        emit desktopThemesChanged();
        return;
    }
    sendToCompositor("reload-cursor " + theme.toUtf8());
    emit desktopThemesChanged();
}

void Preferences::setAccent(const QColor& color)
{
    if (!color.isValid() || color == m_accent) {
        return;
    }
    if (setShell(QStringLiteral("appearance/accent"), color.name())) {
        m_accent = color;
    }
    emit accentChanged();
}

void Preferences::setAccentSource(const QString& source)
{
    if (source == m_accentSource
        || (source != QLatin1String("theme") && source != QLatin1String("wallpaper")
            && source != QLatin1String("chosen"))) {
        return;
    }
    if (setShell(QStringLiteral("appearance/accentSource"), source)) {
        m_accentSource = source;
    }
    emit accentChanged();
}

/** Persist the app mode and update GTK and KDE appearance tools with isolated launches. */
void Preferences::setAppTheme(const QString& theme)
{
    if (theme == m_appTheme || (theme != QLatin1String("light") && theme != QLatin1String("dark"))) {
        return;
    }
    // Commit before changing the shown mode or starting GTK/KDE helpers.
    if (!setShell(QStringLiteral("appearance/appTheme"), theme)) {
        emit appThemeChanged();
        return;
    }
    m_appTheme = theme;
    const bool light = theme == QLatin1String("light");
    // GTK, the portal and whoever follows org.freedesktop.appearance.
    vela::process::startDetached(QStringLiteral("gsettings"),
        { QStringLiteral("set"), QStringLiteral("org.gnome.desktop.interface"), QStringLiteral("color-scheme"),
            light ? QStringLiteral("prefer-light") : QStringLiteral("prefer-dark") });
    // KDE and Qt apps: the light or dark Breeze color scheme, and matching
    // icons.
    if (!QStandardPaths::findExecutable(QStringLiteral("plasma-apply-colorscheme")).isEmpty()) {
        vela::process::startDetached(QStringLiteral("plasma-apply-colorscheme"),
            { light ? QStringLiteral("BreezeLight") : QStringLiteral("BreezeDark") });
    }
    applyIconTheme(light); // ours too
    emit appThemeChanged();
}

void Preferences::setShellTheme(const QString& theme)
{
    if (theme == m_shellTheme || (theme != QLatin1String("light") && theme != QLatin1String("dark"))) {
        return;
    }
    if (setShell(QStringLiteral("appearance/shellTheme"), theme)) {
        m_shellTheme = theme;
    }
    emit appThemeChanged();
}

// ------------------------------------------------------------ taskbar --

void Preferences::setTaskbarAlignment(const QString& alignment)
{
    if (alignment != m_taskbarAlignment) {
        if (setShell(QStringLiteral("taskbar/alignment"), alignment)) {
            m_taskbarAlignment = alignment;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarPosition(const QString& position)
{
    const bool known = position == QLatin1String("bottom") || position == QLatin1String("top")
        || position == QLatin1String("left") || position == QLatin1String("right");
    if (known && position != m_taskbarPosition) {
        if (setShell(QStringLiteral("taskbar/position"), position)) {
            m_taskbarPosition = position;
        }
        emit taskbarChanged();
    }
}

void Preferences::setEndTask(bool on)
{
    if (on != m_endTask) {
        if (setShell(QStringLiteral("taskbar/endTask"), on)) {
            m_endTask = on;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarWindowsFrom(const QString& value)
{
    if ((value == QLatin1String("desktop") || value == QLatin1String("all")) && value != m_taskbarWindowsFrom) {
        if (setShell(QStringLiteral("taskbar/windowsFrom"), value)) {
            m_taskbarWindowsFrom = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarAppsOn(const QString& value)
{
    if ((value == QLatin1String("all") || value == QLatin1String("window")) && value != m_taskbarAppsOn) {
        if (setShell(QStringLiteral("taskbar/appsOn"), value)) {
            m_taskbarAppsOn = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarClickActive(const QString& value)
{
    if ((value == QLatin1String("previews") || value == QLatin1String("next")) && value != m_taskbarClickActive) {
        if (setShell(QStringLiteral("taskbar/clickActive"), value)) {
            m_taskbarClickActive = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTrayIconShown(const QString& key, bool shown)
{
    if (key.isEmpty() || m_taskbarTrayShown.contains(key) == shown) {
        return;
    }
    QStringList keys = m_taskbarTrayShown;
    if (shown) {
        keys.append(key);
    } else {
        keys.removeAll(key);
    }
    if (setShell(QStringLiteral("taskbar/trayShown"), keys)) {
        m_taskbarTrayShown = keys;
    }
    emit taskbarChanged();
}

void Preferences::setTaskbarAttention(bool value)
{
    if (value != m_taskbarAttention) {
        if (setShell(QStringLiteral("taskbar/attention"), value)) {
            m_taskbarAttention = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarBadges(bool value)
{
    if (value != m_taskbarBadges) {
        if (setShell(QStringLiteral("taskbar/badges"), value)) {
            m_taskbarBadges = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarProgress(bool value)
{
    if (value != m_taskbarProgress) {
        if (setShell(QStringLiteral("taskbar/progress"), value)) {
            m_taskbarProgress = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setSuperNumberKeys(const QString& mode)
{
    if ((mode == QLatin1String("apps") || mode == QLatin1String("desktops") || mode == QLatin1String("off"))
        && mode != m_superNumberKeys) {
        const auto previous = m_superNumberKeys;
        m_superNumberKeys = mode;
        keepOrRestore(saveCompositor({ { QStringLiteral("super-number-keys"), mode } }), m_superNumberKeys, previous);
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarNumberHints(bool value)
{
    if (value != m_taskbarNumberHints) {
        if (setShell(QStringLiteral("taskbar/numberHints"), value)) {
            m_taskbarNumberHints = value;
        }
        emit taskbarChanged();
    }
}

void Preferences::setTaskbarAllScreens(bool on)
{
    if (on != m_taskbarAllScreens) {
        if (setShell(QStringLiteral("taskbar/allScreens"), on)) {
            m_taskbarAllScreens = on;
        }
        emit taskbarChanged();
    }
}

void Preferences::setShowDesktopCorner(bool on)
{
    if (on != m_showDesktopCorner) {
        if (setShell(QStringLiteral("taskbar/showDesktopCorner"), on)) {
            m_showDesktopCorner = on;
        }
        emit taskbarChanged();
    }
}

void Preferences::setDesktopIcons(bool on)
{
    if (on != m_desktopIcons) {
        if (setShell(QStringLiteral("desktop/showIcons"), on)) {
            m_desktopIcons = on;
        }
        emit desktopChanged();
    }
}

void Preferences::setMenuEntriesHidden(const QStringList& keys, bool hidden)
{
    // On the list in the file, under its lock: keys another writer hid
    // since Settings read it stay hidden.
    QStringList committed;
    const bool saved = saveShell([&](QSettings& staged) {
        QStringList list = staged.value(QStringLiteral("menus/hidden")).toStringList();
        list.removeAll(QString());
        for (const QString& key : keys) {
            if (key.isEmpty()) {
                continue;
            }
            list.removeAll(key);
            if (hidden) {
                list.append(key);
            }
        }
        if (list.isEmpty()) {
            staged.remove(QStringLiteral("menus/hidden"));
        } else {
            staged.setValue(QStringLiteral("menus/hidden"), list);
        }
        committed = list;
    });
    if (saved) {
        m_hiddenMenuEntries = committed;
    }
    emit menusChanged(); // a switch the write refused goes back
}

void Preferences::showAllMenuEntries()
{
    // Every key in the file, those hidden elsewhere too.
    if (saveShell([](QSettings& staged) { staged.remove(QStringLiteral("menus/hidden")); })) {
        m_hiddenMenuEntries.clear();
    }
    emit menusChanged();
}

void Preferences::setStartPinned(bool on)
{
    if (on != m_startPinned) {
        if (setShell(QStringLiteral("start/showPinned"), on)) {
            m_startPinned = on;
        }
        emit startChanged();
    }
}

void Preferences::setStartIcon(const QString& kind, const QString& picture)
{
    if (kind != QLatin1String("vela") && kind != QLatin1String("distribution") && kind != QLatin1String("picture")) {
        return;
    }
    QList<QPair<QString, QVariant>> changes;
    if (kind == QLatin1String("picture") && !picture.isEmpty()) {
        const QString path = picture.startsWith(QLatin1String("file:")) ? QUrl(picture).toLocalFile() : picture;
        if (!path.startsWith(u'/') || !QFileInfo(path).isFile() || !QImageReader(path).canRead()) {
            return;
        }
        changes.append({ QStringLiteral("start/iconPicture"), path });
    }
    changes.append({ QStringLiteral("start/icon"), kind });
    if (!saveShell(changes)) {
        emit startChanged();
        return;
    }
    const vela::start::Icon icon = vela::start::icon(shellSettings());
    if (icon.kind != m_startIcon || icon.source != m_startIconSource) {
        m_startIcon = icon.kind;
        m_startIconSource = icon.source;
        emit startChanged();
    }
}

void Preferences::refreshStartIcon()
{
    const vela::start::Distribution distribution = vela::start::distribution();
    const vela::start::Icon icon = vela::start::icon(shellSettings());
    if (distribution.name != m_distribution.name || distribution.logo != m_distribution.logo
        || icon.kind != m_startIcon || icon.source != m_startIconSource) {
        m_distribution = distribution;
        m_startIcon = icon.kind;
        m_startIconSource = icon.source;
        emit startChanged();
    }
}

void Preferences::setClockDate(bool on)
{
    if (on != m_clockDate) {
        if (setShell(QStringLiteral("taskbar/showDate"), on)) {
            m_clockDate = on;
        }
        emit clockChanged();
    }
}

void Preferences::setClock24(bool on)
{
    if (on != m_clock24) {
        if (setShell(QStringLiteral("clock/hours"), on ? 24 : 12)) {
            m_clock24 = on;
        }
        emit clockChanged();
    }
}

void Preferences::setClockSeconds(bool on)
{
    if (on != m_clockSeconds) {
        if (setShell(QStringLiteral("taskbar/clockSeconds"), on)) {
            m_clockSeconds = on;
        }
        emit clockChanged();
    }
}

void Preferences::setNotificationCount(bool on)
{
    if (on != m_notificationCount) {
        if (setShell(QStringLiteral("taskbar/notificationCount"), on)) {
            m_notificationCount = on;
        }
        emit clockChanged();
    }
}

QString Preferences::timeFormat() const
{
    return vela::clock::timeFormat(m_clock24);
}

void Preferences::setTaskView(bool on)
{
    if (on != m_taskView) {
        if (setShell(QStringLiteral("taskbar/taskView"), on)) {
            m_taskView = on;
        }
        emit taskbarChanged();
    }
}

void Preferences::setDoNotDisturb(bool on)
{
    if (on != m_doNotDisturb) {
        if (setShell(QStringLiteral("notifications/doNotDisturb"), on)) {
            m_doNotDisturb = on;
        }
        emit doNotDisturbChanged();
    }
}

void Preferences::setSilenceWhileSharing(bool on)
{
    if (on != m_silenceWhileSharing) {
        if (setShell(QStringLiteral("notifications/silenceWhileSharing"), on)) {
            m_silenceWhileSharing = on;
        }
        emit doNotDisturbChanged();
    }
}

void Preferences::setBatteryNotify(bool on)
{
    if (on != m_batteryNotify) {
        if (setShell(QStringLiteral("battery/notify"), on)) {
            m_batteryNotify = on;
        }
        emit batteryChanged();
    }
}

void Preferences::setBatteryLowLevel(int percent)
{
    if (percent < vela::battery::minimumLowLevel || percent > vela::battery::maximumLowLevel) {
        return;
    }
    if (percent != m_batteryLowLevel) {
        if (setShell(QStringLiteral("battery/lowLevel"), percent)) {
            m_batteryLowLevel = percent;
        }
        emit batteryChanged();
    }
}

QVariantList Preferences::batteryLowLevels() const
{
    QVariantList levels;
    bool current = false;
    for (const int level : vela::battery::lowLevels) {
        if (!current && m_batteryLowLevel < level) {
            levels.append(m_batteryLowLevel); // set by hand in the file
        }
        current |= m_batteryLowLevel <= level;
        levels.append(level);
    }
    if (!current) {
        levels.append(m_batteryLowLevel);
    }
    return levels;
}

void Preferences::setDeviceBatteryNotify(bool on)
{
    if (on != m_deviceBatteryNotify) {
        if (setShell(QStringLiteral("battery/devices"), on)) {
            m_deviceBatteryNotify = on;
        }
        emit batteryChanged();
    }
}

// ------------------------------------------------------- compositor --

void Preferences::publishSaveError()
{
    QStringList paths;
    if (m_shellSaveFailed) {
        paths.append(shellSettings().fileName());
    }
    if (m_compositorSaveFailed) {
        paths.append(compositorConfigPath());
    }
    const QString message = paths.isEmpty() ? QString() : tr("Check that you can write %1.").arg(paths.join(u'\n'));
    if (m_saveError == message) {
        return;
    }
    m_saveError = message;
    emit saveErrorChanged();
}

void Preferences::noteSaveFailure()
{
    m_compositorSaveFailed = true;
    publishSaveError();
    if (m_reportingSaveFailure) {
        return;
    }
    const QFileInfo info(compositorConfigPath());
    if (!info.isFile() || !info.isReadable()) {
        return;
    }
    // The file is still the previous save. Memory can be older than that when
    // the watcher never attached, so read it back instead of the copy taken
    // before this attempt.
    m_reportingSaveFailure = true;
    reloadCompositor();
    m_reportingSaveFailure = false;
    m_restoredFromFile = true;
}

void Preferences::clearSaveError()
{
    m_compositorSaveFailed = false;
    publishSaveError();
}

bool Preferences::saveCompositorKeys(const QStringList& keys)
{
    QList<QPair<QString, QString>> changes;
    for (const QString& key : keys) {
        if (key == QLatin1String("screen-off")) {
            changes.append({ key, QString::number(m_screenOffMinutes) });
        } else if (key == QLatin1String("lock-on-idle")) {
            changes.append({ key, m_lockOnIdle ? QStringLiteral("yes") : QStringLiteral("no") });
        } else if (key == QLatin1String("language")) {
            changes.append({ key, m_language });
        } else if (key == QLatin1String("secret-backend")) {
            changes.append({ key, m_secretBackend });
        } else if (key == QLatin1String("theme")) {
            changes.append({ key, m_visualTheme });
        } else if (key.endsWith(QLatin1String("-style")) && m_surfaceStyles.contains(key.chopped(6))) {
            changes.append({ key, m_surfaceStyles.value(key.chopped(6)).toString() });
        } else if (key == QLatin1String("keyboard-layout") || key == QLatin1String("keyboard-variant")) {
            QStringList values;
            for (const QVariant& value : std::as_const(m_layouts)) {
                values << value.toMap()[key == QLatin1String("keyboard-layout") ? QStringLiteral("layout") : QStringLiteral("variant")].toString();
            }
            changes.append({ key, values.join(u',') });
        } else if (key == QLatin1String("keyboard-repeat-delay")) {
            changes.append({ key, QString::number(m_repeatDelay) });
        } else if (key == QLatin1String("keyboard-repeat-rate")) {
            changes.append({ key, QString::number(m_repeatRate) });
        }
    }
    return saveCompositor(changes);
}

bool Preferences::saveCompositor(const QList<QPair<QString, QString>>& changes)
{
    // The file as it is now (the compositor may have changed it), with these
    // new keys.
    const QString path = compositorConfigPath();
    QStringList lines;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            lines = QString::fromUtf8(in.readAll()).split(u'\n');
            for (QString& line : lines) {
                line = modernLine(line); // the old Italian names, if the compositor hasn't changed them already
            }
            while (!lines.isEmpty() && lines.last().isEmpty()) {
                lines.removeLast();
            }
        }
    }
    if (lines.isEmpty()) {
        lines << QStringLiteral("# Vela settings (written by the Settings app)");
    }
    for (const auto& [key, value] : changes) {
        // The last line of the key: the one the compositor and the shell read.
        bool found = false;
        for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
            if (line->startsWith(key + u'=')) {
                *line = key + u'=' + value;
                found = true;
                break;
            }
        }
        if (!found) {
            lines << key + u'=' + value;
        }
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    // Existing settings may have been created with mode 0644.
    if (!vela::openPrivateFile(file, QIODevice::WriteOnly | QIODevice::Text)) {
        noteSaveFailure();
        return false;
    }
    const QByteArray data = (lines.join(u'\n') + u'\n').toUtf8();
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        noteSaveFailure();
        return false;
    }
    if (!file.commit()) {
        noteSaveFailure();
        return false;
    }
    // The compositor rereads (and our copy of the file updates from the
    // watcher).
    clearSaveError();
    sendToCompositor("reload-config");
    return true;
}

void Preferences::setScreenOffMinutes(int minutes)
{
    if (minutes != m_screenOffMinutes) {
        const auto previous = m_screenOffMinutes;
        m_screenOffMinutes = std::max(0, minutes);
        keepOrRestore(saveCompositorKeys(QStringList { QStringLiteral("screen-off") }), m_screenOffMinutes, previous);
        emit idleChanged();
    }
}

void Preferences::setLockOnIdle(bool on)
{
    if (on != m_lockOnIdle) {
        const auto previous = m_lockOnIdle;
        m_lockOnIdle = on;
        keepOrRestore(saveCompositorKeys(QStringList { QStringLiteral("lock-on-idle") }), m_lockOnIdle, previous);
        emit idleChanged();
    }
}

void Preferences::setLanguage(const QString& language)
{
    if (language != m_language) {
        const auto previous = m_language;
        m_language = language;
        keepOrRestore(saveCompositorKeys(QStringList { QStringLiteral("language") }), m_language, previous);
        emit languageChanged();
    }
}

bool Preferences::isSecretBackendInstalled(const QString& backend) const
{
    if (backend != QLatin1String("gnome") && backend != QLatin1String("kwallet")) {
        return false;
    }
    const QString binary = backend == QLatin1String("gnome")
        ? QStringLiteral("/usr/bin/gnome-keyring-daemon") : QStringLiteral("/usr/bin/ksecretd");
    return QFileInfo(binary).isExecutable();
}

bool Preferences::secretBackendInstalled() const
{
    return isSecretBackendInstalled(m_secretBackend);
}

QString Preferences::activeSecretBackend() const
{
    return qEnvironmentVariable("VELA_SECRET_BACKEND", QStringLiteral("kwallet"));
}

QString Preferences::credentialStatus() const
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        return {};
    }
    QFile status(runtime + QStringLiteral("/vela-credential-compat.status"));
    if (!status.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(status.readLine()).trimmed();
}

void Preferences::setSecretBackend(const QString& backend)
{
    if (backend != QLatin1String("gnome") && backend != QLatin1String("kwallet")) {
        return;
    }
    // An unconfigured first login still needs to persist an explicit KWallet
    // choice, even though KWallet is the backwards-compatible fallback.
    if (backend != m_secretBackend || !m_secretBackendConfigured) {
        const auto previous = m_secretBackend;
        const bool previouslyConfigured = m_secretBackendConfigured;
        m_secretBackend = backend;
        const bool saved = saveCompositorKeys(QStringList { QStringLiteral("secret-backend") });
        if (saved) {
            m_secretBackendConfigured = true;
        } else if (!m_restoredFromFile) {
            m_secretBackend = previous;
            m_secretBackendConfigured = previouslyConfigured;
        }
        // noteSaveFailure() may have reloaded vela.conf; consume that flag
        // for this save attempt so it cannot suppress a later rollback.
        m_restoredFromFile = false;
        emit secretBackendChanged();
    }
}

void Preferences::setVisualTheme(const QString& name)
{
    // Only a name themefile.h would load: a path stays out of vela.conf.
    const QString theme = name.isEmpty() ? vela::themes::defaultTheme() : name;
    if (theme != m_visualTheme && vela::themes::validName(theme)) {
        const QString previous = m_visualTheme;
        m_visualTheme = theme;
        // The shell and the apps follow (Appearance watches vela.conf).
        keepOrRestore(saveCompositorKeys(QStringList { QStringLiteral("theme") }), m_visualTheme, previous);
        emit visualThemeChanged();
    }
}

QVariantMap Preferences::resolveTheme(const QString& name) const
{
    // The same resolution as the shell's (Appearance), without applying it:
    // the theme with the styles chosen for each surface over it.
    vela::themes::Choice choice;
    choice[vela::themes::Theme] = name;
    for (int i = vela::themes::Taskbar; i < vela::themes::LayerCount; ++i) {
        choice[i] = m_surfaceStyles.value(QLatin1String(vela::themes::layerInfo(vela::themes::Layer(i)).directory)).toString();
    }
    const vela::themes::Loaded loaded = vela::themes::load(choice);
    const vela::themes::Resolved& theme = loaded.layers[vela::themes::Theme];
    QVariantList problems;
    for (const vela::themes::Problem& problem : theme.problems) {
        problems.append(vela::themes::toVariant(problem));
    }
    return { { QStringLiteral("installed"), !theme.files.isEmpty() }, { QStringLiteral("tokens"), loaded.merged() },
        { QStringLiteral("problems"), problems },
        { QStringLiteral("layoutFromStyle"), vela::themes::setsTaskbarLayout(loaded.layers[vela::themes::Taskbar].tokens,
                                          m_shellTheme == QLatin1String("light")) } };
}

void Preferences::setSurfaceStyle(const QString& surface, const QString& style)
{
    const QString name = style.trimmed();
    if (!m_surfaceStyles.contains(surface) || m_surfaceStyles.value(surface).toString() == name
        || (!name.isEmpty() && !vela::themes::validName(name))) {
        return;
    }
    const QString previous = m_surfaceStyles.value(surface).toString();
    m_surfaceStyles.insert(surface, name);
    if (!saveCompositorKeys(QStringList { surface + QStringLiteral("-style") })) {
        if (!m_restoredFromFile) {
            m_surfaceStyles.insert(surface, previous);
        }
        m_restoredFromFile = false;
    }
    emit visualThemeChanged();
}

QVariantList Preferences::styleGallery(const QString& surface) const
{
    return vela::themes::gallery(surface);
}

QVariantMap Preferences::themeSurfacePictures(const QString& theme, const QString& surface) const
{
    return vela::themes::themeSurfacePictures(theme, surface);
}

void Preferences::resetThemes()
{
    const QString previousTheme = m_visualTheme;
    const QVariantMap previousStyles = m_surfaceStyles;
    QStringList keys { QStringLiteral("theme") };
    m_visualTheme = vela::themes::defaultTheme();
    for (auto it = m_surfaceStyles.begin(); it != m_surfaceStyles.end(); ++it) {
        it.value() = QString();
        keys.append(it.key() + QStringLiteral("-style"));
    }
    if (!saveCompositorKeys(keys)) {
        if (!m_restoredFromFile) {
            m_visualTheme = previousTheme;
            m_surfaceStyles = previousStyles;
        }
        m_restoredFromFile = false;
    }
    emit visualThemeChanged();
}

QString Preferences::displayName(const QString& surface, const QString& name) const
{
    return vela::themes::displayName(surface, name);
}

/** Create the user's theme/style directory and diagnose unavailable or failed openers. */
void Preferences::openThemesFolder(const QString& surface)
{
    const vela::themes::Layer layer = vela::themes::layerFor(surface);
    if (layer >= vela::themes::LayerCount) {
        return;
    }
    // Made, it is watched at once: the watcher keeps an eye on the directory
    // above it while it doesn't exist (watchThemes()).
    const QString dir = vela::themes::userDirectory(layer);
    if (!QDir().mkpath(dir)) {
        qWarning("vela-settings: cannot create themes folder: %s", qPrintable(dir));
        return;
    }
    const QString opener = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
    if (opener.isEmpty()) {
        qWarning("vela-settings: cannot open themes folder: xdg-open is unavailable (install xdg-utils)");
        return;
    }
    if (!vela::process::startDetached(opener, { dir })) {
        qWarning("vela-settings: failed to start xdg-open for %s", qPrintable(dir));
    }
}

void Preferences::setThemeAndStyles(const QString& theme, const QVariantMap& styles)
{
    // A theme name vela.conf had by hand and Vela can't load stays as it is;
    // the styles come back all the same.
    const QString name = theme.isEmpty() ? vela::themes::defaultTheme() : theme;
    const QString previousTheme = m_visualTheme;
    const QVariantMap previousStyles = m_surfaceStyles;
    QStringList keys;
    if (vela::themes::validName(name)) {
        m_visualTheme = name;
        keys.append(QStringLiteral("theme"));
    }
    for (auto it = m_surfaceStyles.begin(); it != m_surfaceStyles.end(); ++it) {
        const QString style = styles.value(it.key()).toString().trimmed();
        it.value() = style.isEmpty() || vela::themes::validName(style) ? style : QString();
        keys.append(it.key() + QStringLiteral("-style"));
    }
    if (!saveCompositorKeys(keys)) {
        if (!m_restoredFromFile) {
            m_visualTheme = previousTheme;
            m_surfaceStyles = previousStyles;
        }
        m_restoredFromFile = false;
    }
    emit visualThemeChanged();
}

bool Preferences::saveLayouts()
{
    return saveCompositorKeys(QStringList { QStringLiteral("keyboard-layout"), QStringLiteral("keyboard-variant") });
}

void Preferences::addKeyboardLayout(const QString& layout, const QString& variant)
{
    for (const QVariant& value : std::as_const(m_layouts)) {
        const QVariantMap entry = value.toMap();
        if (entry[QStringLiteral("layout")] == layout && entry[QStringLiteral("variant")] == variant) {
            return;
        }
    }
    m_layouts.append(QVariantMap { { QStringLiteral("layout"), layout }, { QStringLiteral("variant"), variant } });
    if (!saveLayouts()) {
        if (!m_restoredFromFile) {
            m_layouts.removeLast();
        }
        m_restoredFromFile = false;
    }
    emit keyboardChanged();
}

void Preferences::removeKeyboardLayout(int index)
{
    if (index >= 0 && index < m_layouts.size()) {
        const QVariant previous = m_layouts.at(index);
        m_layouts.removeAt(index);
        if (!saveLayouts()) {
            if (!m_restoredFromFile) {
                m_layouts.insert(index, previous);
            }
            m_restoredFromFile = false;
        }
        emit keyboardChanged();
    }
}

void Preferences::moveKeyboardLayoutUp(int index)
{
    if (index > 0 && index < m_layouts.size()) {
        m_layouts.swapItemsAt(index, index - 1);
        if (!saveLayouts()) {
            if (!m_restoredFromFile) {
                m_layouts.swapItemsAt(index, index - 1);
            }
            m_restoredFromFile = false;
        }
        emit keyboardChanged();
    }
}

void Preferences::setRepeatDelay(int ms)
{
    if (ms != m_repeatDelay) {
        const auto previous = m_repeatDelay;
        m_repeatDelay = std::clamp(ms, 100, 2000);
        keepOrRestore(saveCompositorKeys(QStringList { QStringLiteral("keyboard-repeat-delay") }), m_repeatDelay, previous);
        emit keyboardChanged();
    }
}

void Preferences::setRepeatRate(int perSecond)
{
    if (perSecond != m_repeatRate) {
        const auto previous = m_repeatRate;
        m_repeatRate = std::clamp(perSecond, 1, 100);
        keepOrRestore(saveCompositorKeys(QStringList { QStringLiteral("keyboard-repeat-rate") }), m_repeatRate, previous);
        emit keyboardChanged();
    }
}

// ------------------------------------------- night light and accessibility --

namespace {

QString yesNo(bool on)
{
    return on ? QStringLiteral("yes") : QStringLiteral("no");
}

} // namespace

void Preferences::setNightLight(bool on)
{
    if (on != m_nightLight) {
        const auto previous = m_nightLight;
        m_nightLight = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("night-light"), yesNo(on) } }), m_nightLight, previous);
        emit nightLightChanged();
    }
}

void Preferences::setNightTemperature(int kelvin)
{
    kelvin = std::clamp((kelvin + 50) / 100 * 100, VELA_NIGHT_KELVIN_MIN, VELA_NIGHT_KELVIN_MAX);
    if (kelvin != m_nightTemperature) {
        const auto previous = m_nightTemperature;
        m_nightTemperature = kelvin;
        keepOrRestore(saveCompositor({ { QStringLiteral("night-light-temperature"), QString::number(kelvin) } }), m_nightTemperature, previous);
        emit nightLightChanged();
    }
}

/** Persist the NVIDIA workaround setting and notify bindings in the UI. */
void Preferences::setNightNvidiaFix(bool on)
{
    if (on != m_nightNvidiaFix) {
        const auto previous = m_nightNvidiaFix;
        m_nightNvidiaFix = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("night-light-nvidia-fix"), yesNo(on) } }), m_nightNvidiaFix, previous);
        emit nightLightChanged();
    }
}

void Preferences::setNightSchedule(const QString& schedule)
{
    if (schedule != m_nightSchedule) {
        const auto previous = m_nightSchedule;
        m_nightSchedule = schedule;
        keepOrRestore(saveCompositor({ { QStringLiteral("night-light-schedule"), schedule } }), m_nightSchedule, previous);
        emit nightLightChanged();
    }
}

void Preferences::setNightFrom(const QString& time)
{
    if (time != m_nightFrom && QTime::fromString(time, QStringLiteral("HH:mm")).isValid()) {
        const auto previous = m_nightFrom;
        m_nightFrom = time;
        keepOrRestore(saveCompositor({ { QStringLiteral("night-light-from"), time } }), m_nightFrom, previous);
        emit nightLightChanged();
    }
}

void Preferences::setNightTo(const QString& time)
{
    if (time != m_nightTo && QTime::fromString(time, QStringLiteral("HH:mm")).isValid()) {
        const auto previous = m_nightTo;
        m_nightTo = time;
        keepOrRestore(saveCompositor({ { QStringLiteral("night-light-to"), time } }), m_nightTo, previous);
        emit nightLightChanged();
    }
}

void Preferences::setTearing(bool on)
{
    if (on != m_tearing) {
        const auto previous = m_tearing;
        m_tearing = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("tearing"), yesNo(on) } }), m_tearing, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setVrr(const QString& mode)
{
    if (mode != m_vrr) {
        const auto previous = m_vrr;
        m_vrr = mode;
        keepOrRestore(saveCompositor({ { QStringLiteral("variable-refresh"), mode } }), m_vrr, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setDirectScanout(const QString& mode)
{
    if ((mode == QLatin1String("auto") || mode == QLatin1String("no")) && mode != m_directScanout) {
        const auto previous = m_directScanout;
        m_directScanout = mode;
        keepOrRestore(saveCompositor({ { QStringLiteral("direct-scanout"), mode } }), m_directScanout, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setMagnifier(bool on)
{
    if (on != m_magnifier) {
        m_magnifier = on;
        sendToCompositor(on ? "magnifier on" : "magnifier off");
        emit accessibilityChanged();
    }
}

void Preferences::setMagnifierStep(int percent)
{
    if (percent != m_magnifierStep) {
        const auto previous = m_magnifierStep;
        m_magnifierStep = percent;
        keepOrRestore(saveCompositor({ { QStringLiteral("magnifier-step"), QString::number(percent) } }), m_magnifierStep, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setColorFilter(bool on)
{
    if (on != m_colorFilter) {
        const auto previous = m_colorFilter;
        m_colorFilter = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("color-filters"), yesNo(on) } }), m_colorFilter, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setColorFilterKind(const QString& kind)
{
    if (kind != m_colorFilterKind) {
        const auto previous = m_colorFilterKind;
        m_colorFilterKind = kind;
        keepOrRestore(saveCompositor({ { QStringLiteral("color-filter"), kind } }), m_colorFilterKind, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setColorFilterShortcut(bool on)
{
    if (on != m_colorFilterShortcut) {
        const auto previous = m_colorFilterShortcut;
        m_colorFilterShortcut = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("color-filters-shortcut"), yesNo(on) } }), m_colorFilterShortcut, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setStickyKeys(bool on)
{
    if (on != m_stickyKeys) {
        const auto previous = m_stickyKeys;
        m_stickyKeys = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("sticky-keys"), yesNo(on) } }), m_stickyKeys, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setAnimationEffects(bool on)
{
    if (on != m_animationEffects) {
        const auto previous = m_animationEffects;
        m_animationEffects = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("animation-effects"), yesNo(on) } }), m_animationEffects, previous);
        emit accessibilityChanged();
    }
}

void Preferences::setAnimationDuration(int percent)
{
    percent = qBound(50, percent, 300);
    if (percent != m_animationDuration) {
        const auto previous = m_animationDuration;
        m_animationDuration = percent;
        keepOrRestore(saveCompositor({ { QStringLiteral("animation-duration"), QString::number(percent) } }), m_animationDuration, previous);
        emit accessibilityChanged();
    }
}

// ---------------------------------------------------- mouse and touchpad --

void Preferences::setMouseSpeed(int value)
{
    value = std::clamp(value, 1, 20);
    if (value != m_mouseSpeed) {
        const auto previous = m_mouseSpeed;
        m_mouseSpeed = value;
        keepOrRestore(saveCompositor({ { QStringLiteral("mouse-speed"), QString::number(value) } }), m_mouseSpeed, previous);
        emit inputChanged();
    }
}

void Preferences::setMousePrecision(bool on)
{
    if (on != m_mousePrecision) {
        const auto previous = m_mousePrecision;
        m_mousePrecision = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("mouse-precision"), yesNo(on) } }), m_mousePrecision, previous);
        emit inputChanged();
    }
}

void Preferences::setMouseLeftHanded(bool on)
{
    if (on != m_mouseLeftHanded) {
        const auto previous = m_mouseLeftHanded;
        m_mouseLeftHanded = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("mouse-primary-button"), on ? QStringLiteral("right") : QStringLiteral("left") } }), m_mouseLeftHanded, previous);
        emit inputChanged();
    }
}

void Preferences::setWheelLines(int lines)
{
    lines = std::clamp(lines, 1, 20);
    if (lines != m_wheelLines) {
        const auto previous = m_wheelLines;
        m_wheelLines = lines;
        keepOrRestore(saveCompositor({ { QStringLiteral("mouse-scroll-lines"), QString::number(lines) } }), m_wheelLines, previous);
        emit inputChanged();
    }
}

void Preferences::setTouchpad(bool on)
{
    if (on != m_touchpad) {
        const auto previous = m_touchpad;
        m_touchpad = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad"), yesNo(on) } }), m_touchpad, previous);
        emit inputChanged();
    }
}

void Preferences::setTouchpadWithMouse(bool on)
{
    if (on != m_touchpadWithMouse) {
        const auto previous = m_touchpadWithMouse;
        m_touchpadWithMouse = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad-with-mouse"), yesNo(on) } }), m_touchpadWithMouse, previous);
        emit inputChanged();
    }
}

void Preferences::setTouchpadSpeed(int value)
{
    value = std::clamp(value, 1, 20);
    if (value != m_touchpadSpeed) {
        const auto previous = m_touchpadSpeed;
        m_touchpadSpeed = value;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad-speed"), QString::number(value) } }), m_touchpadSpeed, previous);
        emit inputChanged();
    }
}

void Preferences::setTouchpadTap(bool on)
{
    if (on != m_touchpadTap) {
        const auto previous = m_touchpadTap;
        m_touchpadTap = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad-tap"), yesNo(on) } }), m_touchpadTap, previous);
        emit inputChanged();
    }
}

void Preferences::setTouchpadNatural(bool on)
{
    if (on != m_touchpadNatural) {
        const auto previous = m_touchpadNatural;
        m_touchpadNatural = on;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad-natural-scroll"), yesNo(on) } }), m_touchpadNatural, previous);
        emit inputChanged();
    }
}

void Preferences::setThreeFingers(const QString& action)
{
    if (action != m_threeFingers) {
        const auto previous = m_threeFingers;
        m_threeFingers = action;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad-three-fingers"), action } }), m_threeFingers, previous);
        emit inputChanged();
    }
}

void Preferences::setFourFingers(const QString& action)
{
    if (action != m_fourFingers) {
        const auto previous = m_fourFingers;
        m_fourFingers = action;
        keepOrRestore(saveCompositor({ { QStringLiteral("touchpad-four-fingers"), action } }), m_fourFingers, previous);
        emit inputChanged();
    }
}

// --------------------------------------------------------------- clipboard --

void Preferences::setClipboardHistory(bool on)
{
    if (on != m_clipboardHistory) {
        if (setShell(QStringLiteral("clipboard/history"), on)) {
            m_clipboardHistory = on;
        }
        emit clipboardChanged();
    }
}

void Preferences::clearClipboard()
{
    // To the shell, on its socket.
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-shell-") + display + QStringLiteral(".sock"));
    if (socket.waitForConnected(200)) {
        socket.write("clipboard-clear\n");
        socket.waitForBytesWritten(200);
    }
}
