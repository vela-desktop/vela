// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appearance.h"

#include "defaults.h"
#include "iconprovider.h"
#include "language.h"
#include "mica.h"
#include "scheme.h"
#include "themefile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

namespace {

// animation-effects and animation-duration in vela.conf (the compositor
// reads them too): on, at Vela's durations, unless it says otherwise.
struct Motion {
    bool effects = true;
    qreal duration = 1.0;
};

Motion configuredMotion()
{
    Motion motion;
    QFile file(vela::language::configPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return motion;
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1String("animation-effects="))) {
            const QString value = line.mid(18).trimmed();
            motion.effects
                = value != QLatin1String("no") && value != QLatin1String("0") && value != QLatin1String("false");
        } else if (line.startsWith(QLatin1String("animation-duration="))) {
            bool ok = false;
            const int percent = line.mid(19).trimmed().toInt(&ok);
            motion.duration = ok ? qBound(50, percent, 300) / 100.0 : 1.0;
        }
    }
    return motion;
}

} // namespace

Appearance::Appearance(QObject* parent, Mode mode)
    : QObject(parent)
    , m_mode(mode)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        watch();
        reload();
    });
    reload();
}

void Appearance::watch()
{
    // Whoever writes the files replaces them: the directories are watched too.
    const QString shellConfig = QSettings(QStringLiteral("Vela"), QStringLiteral("vela-shell")).fileName();
    // The chosen theme and styles and what they inherit too: editing one shows
    // at once.
    QStringList paths { shellConfig, wallpaperTintCachePath(), wallpaperColorCachePath(), vela::language::configPath() };
    paths += m_loaded.files();
    for (const QString& path : std::as_const(paths)) {
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        if (info.exists() && !m_watcher.files().contains(path)) {
            m_watcher.addPath(path);
        }
        if (info.dir().exists() && !m_watcher.directories().contains(info.absolutePath())) {
            m_watcher.addPath(info.absolutePath());
        }
    }
    // The themes' and styles' directories: a file added, removed or replaced
    // there (the chosen one may be missing yet) shows at once too.
    for (int i = 0; i < vela::themes::LayerCount; ++i) {
        for (const QString& dir : vela::themes::watchedDirectories(vela::themes::Layer(i))) {
            if (!m_watcher.directories().contains(dir)) {
                m_watcher.addPath(dir);
            }
        }
    }
    if (!m_watching) {
        m_watching = true;
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
        connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    }
}

QStringList Appearance::chosenStyles() const
{
    QStringList chosen;
    for (int i = vela::themes::Taskbar; i < vela::themes::LayerCount; ++i) {
        if (styleChosen(vela::themes::Layer(i))) {
            chosen.append(QLatin1String(vela::themes::layerInfo(vela::themes::Layer(i)).directory));
        }
    }
    return chosen;
}

QVariantMap Appearance::taskbarLayout(const QVariantMap& tokens, bool light, const QString& alignment) const
{
    const vela::themes::TaskbarLayout layout = vela::themes::taskbarLayout(tokens, light);
    return {
        { QStringLiteral("variant"), layout.variant },
        { QStringLiteral("margin"), layout.margin },
        { QStringLiteral("radius"), layout.radius },
        { QStringLiteral("floating"), layout.floating },
        { QStringLiteral("fit"), layout.fit },
        { QStringLiteral("edge"), layout.edge },
        { QStringLiteral("appsAtLeft"), layout.appsAtLeft(alignment) },
    };
}

QVariantMap Appearance::tonalOf(const QColor& source, bool light) const
{
    const vela::color::Roles roles = vela::color::tonalSpot(source.rgb(), !light);
    QVariantMap tonal;
    for (const vela::color::RoleName& role : vela::color::roleNames) {
        tonal.insert(QLatin1String(role.name), QColor::fromRgb(roles.*role.member));
    }
    return tonal;
}

QColor Appearance::schemeSource(bool light) const
{
    return vela::accent::schemeSource(m_theme, light, m_accentSource, m_chosenAccent, m_wallpaperColor);
}

void Appearance::reload()
{
    QSettings shell(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
    shell.sync();
    const QString key = m_mode == Mode::Shell ? QStringLiteral("appearance/shellTheme") : QStringLiteral("appearance/appTheme");
    const bool light = shell.value(key).toString() == QLatin1String("light");
    QColor chosen(shell.value(QStringLiteral("appearance/accent")).toString());
    if (!chosen.isValid()) {
        chosen = vela::defaults::accent();
    }
    const vela::accent::Source source
        = vela::accent::sourceFromString(shell.value(QStringLiteral("appearance/accentSource")).toString());
    const QColor wallpaper = cachedWallpaperColor();
    const QColor tint = cachedWallpaperTint();
    const QColor mica = micaFromTint(tint, true, light);
    const QColor micaInactive = micaFromTint(tint, false, light);
    const Motion motion = configuredMotion();
    vela::themes::Loaded loaded = vela::themes::loadConfigured(vela::language::configPath());
    const QStringList files = loaded.files();
    const bool filesChanged = files != m_loaded.files();
    if (filesChanged) {
        for (const QString& file : m_loaded.files()) {
            if (!files.contains(file)) {
                m_watcher.removePath(file);
            }
        }
    }
    const QVariantMap theme = loaded.merged();
    // A style chosen or dropped changes the blur look even with the same keys.
    const bool themeChanged = theme != m_theme || loaded.layers != m_loaded.layers;
    m_loaded = std::move(loaded);
    m_theme = theme;
    if (filesChanged && m_watching) {
        watch();
    }
    // The accent after the theme: it may ask for the wallpaper's.
    m_accentSource = source;
    m_chosenAccent = chosen;
    const bool wallpaperChanged = wallpaper != m_wallpaperColor;
    m_wallpaperColor = wallpaper;
    const QColor accent = vela::accent::userAccent(theme, light, source, chosen, wallpaper).color;
    // The palette only when its source or the mode changed: the quantized
    // wallpaper is cached, but a scheme is still some HCT solving.
    const QColor tonalSource = schemeSource(light);
    const bool tonalChanged = m_tonal.isEmpty() || tonalSource != m_tonalSource || light != m_light;
    if (tonalChanged) {
        m_tonalSource = tonalSource;
        m_tonal = tonalOf(tonalSource, light);
    }
    if (light == m_light && accent == m_accent && mica == m_mica && micaInactive == m_micaInactive) {
        // Only the theme, the palette, the wallpaper's color (Settings shows
        // it) or the motion: the icons stay as they are.
        if (themeChanged || tonalChanged || wallpaperChanged || motion.effects != m_animationEffects
            || motion.duration != m_animationDuration) {
            m_animationEffects = motion.effects;
            m_animationDuration = motion.duration;
            emit changed();
        }
        return;
    }
    m_animationEffects = motion.effects;
    m_animationDuration = motion.duration;
    const bool first = !m_accent.isValid();
    m_light = light;
    m_accent = accent;
    m_mica = mica;
    m_micaInactive = micaInactive;
    applyIconTheme(light); // before QML asks for the icons
    const QString mode = light ? QStringLiteral("l/") : QStringLiteral("d/");
    if (first) {
        m_iconMode = mode;
        return;
    }
    emit changed();
    // The image://icon prefix a moment later, when the icon caches
    // (KIconLoader's too, which gets the D-Bus signal) are empty: only then
    // QML asks for the icons again.
    QTimer::singleShot(300, this, [this, mode] {
        if (mode != m_iconMode) {
            m_iconMode = mode;
            emit iconModeChanged();
        }
    });
}
