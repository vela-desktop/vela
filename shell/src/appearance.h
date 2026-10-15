// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

// The look of Vela's interfaces (Vela.Controls' Style): the apps' light or
// dark mode ("Choose your mode"), accent color and Mica (see mica.h). System
// UI (the shell, the polkit dialog) follows the shell's mode instead
// (Mode::Shell). Also whether things move (animation-effects in vela.conf,
// which Theme reads: docs/visual-coherence.md §6.5).
// Updates itself when Settings change something or the wallpaper changes.
// Icons follow the mode (applyIconTheme).
class Appearance : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor mica READ mica NOTIFY changed)
    Q_PROPERTY(QColor micaInactive READ micaInactive NOTIFY changed)
    Q_PROPERTY(bool animationEffects READ animationEffects NOTIFY changed)
    // "l/" or "d/" for image://icon: changes a moment after the mode, once the
    // icon caches are emptied (applyIconTheme).
    Q_PROPERTY(QString iconMode READ iconMode NOTIFY iconModeChanged)

public:
    enum class Mode { Apps, Shell };
    explicit Appearance(QObject* parent = nullptr, Mode mode = Mode::Apps);

    bool light() const { return m_light; }
    QColor accent() const { return m_accent; }
    QColor mica() const { return m_mica; }
    QColor micaInactive() const { return m_micaInactive; }
    bool animationEffects() const { return m_animationEffects; }
    QString iconMode() const { return m_iconMode; }

    // Starts watching the files (after the first frame: startup stays fast).
    void watch();

signals:
    void changed();
    void iconModeChanged();

private:
    void reload();

    Mode m_mode;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    bool m_watching = false;
    bool m_light = false;
    QColor m_accent;
    QColor m_mica;
    QColor m_micaInactive;
    bool m_animationEffects = true;
    QString m_iconMode = QStringLiteral("d/");
};
