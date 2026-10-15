// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appearance.h"

#include "defaults.h"
#include "iconprovider.h"
#include "language.h"
#include "mica.h"

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
    for (const QString& path : { shellConfig, wallpaperTintCachePath(), vela::language::configPath() }) {
        const QFileInfo info(path);
        if (info.exists() && !m_watcher.files().contains(path)) {
            m_watcher.addPath(path);
        }
        if (info.dir().exists() && !m_watcher.directories().contains(info.absolutePath())) {
            m_watcher.addPath(info.absolutePath());
        }
    }
    if (!m_watching) {
        m_watching = true;
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
        connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    }
}

void Appearance::reload()
{
    QSettings shell(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
    shell.sync();
    const QString key = m_mode == Mode::Shell ? QStringLiteral("appearance/shellTheme") : QStringLiteral("appearance/appTheme");
    const bool light = shell.value(key).toString() == QLatin1String("light");
    QColor accent(shell.value(QStringLiteral("appearance/accent")).toString());
    if (!accent.isValid()) {
        accent = vela::defaults::accent();
    }
    const QColor tint = cachedWallpaperTint();
    const QColor mica = micaFromTint(tint, true, light);
    const QColor micaInactive = micaFromTint(tint, false, light);
    const Motion motion = configuredMotion();
    if (light == m_light && accent == m_accent && mica == m_mica && micaInactive == m_micaInactive) {
        if (motion.effects != m_animationEffects || motion.duration != m_animationDuration) {
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
