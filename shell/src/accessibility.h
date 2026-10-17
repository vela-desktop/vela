// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

// Night light, color filters, magnifier and sticky keys as the shell sees
// them: the compositor applies them (compositor/src/a11y.c) and sends the
// state as JSON on every change ("accessibility <json>"); quick settings turn
// them on and off from here. Caffeine (keep awake, compositor/src/lock.c)
// travels with them.
class Accessibility : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool nightLight READ nightLight WRITE setNightLight NOTIFY changed)
    Q_PROPERTY(bool colorFilter READ colorFilter WRITE setColorFilter NOTIFY changed)
    Q_PROPERTY(bool magnifier READ magnifier WRITE setMagnifier NOTIFY changed)
    Q_PROPERTY(bool stickyKeys READ stickyKeys WRITE setStickyKeys NOTIFY changed)
    Q_PROPERTY(bool caffeine READ caffeine WRITE setCaffeine NOTIFY changed)
    // Seconds left, updated with the state: 0 when off, -1 when on with no
    // deadline (read-only; the shell never sends it).
    Q_PROPERTY(int caffeineSecs READ caffeineSecs NOTIFY changed)
    Q_PROPERTY(int caffeineDurationSecs READ caffeineDurationSecs NOTIFY changed)

public:
    explicit Accessibility(QObject* parent = nullptr);

    bool nightLight() const { return m_nightLight; }
    void setNightLight(bool on);
    bool colorFilter() const { return m_colorFilter; }
    void setColorFilter(bool on);
    bool magnifier() const { return m_magnifier; }
    void setMagnifier(bool on);
    bool stickyKeys() const { return m_stickyKeys; }
    void setStickyKeys(bool on);
    bool caffeine() const { return m_caffeine; }
    void setCaffeine(bool on);
    // Caffeine for a while (minutes, whole or fractional): optimistic like
    // the toggle, the compositor confirms and counts down.
    Q_INVOKABLE void setCaffeineMinutes(double minutes);
    int caffeineSecs() const { return m_caffeineSecs; }
    int caffeineDurationSecs() const { return m_caffeineDurationSecs; }

    // The state sent by the compositor.
    void update(const QByteArray& json);
    // Asks the compositor for it (when the shell starts).
    void query();

signals:
    void changed();

private:
    void resetCaffeineCountdown();

    bool m_nightLight = false;
    bool m_colorFilter = false;
    bool m_magnifier = false;
    bool m_stickyKeys = false;
    bool m_caffeine = false;
    int m_caffeineSecs = 0;
    int m_caffeineDurationSecs = 0;
    int m_caffeineStartSecs = 0;
    QElapsedTimer m_caffeineElapsed;
    QTimer m_caffeineTicker;
};
