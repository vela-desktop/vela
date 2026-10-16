// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A switch backed by a system service (Bluetooth's power, Wi-Fi): it shows
// what the user asked for at once, not when the service is done, which can
// take a second and which the service announces with other changes on the
// way (each one would put the switch back). The service's own value is shown
// again once it reports the one asked for, refuses it, or stays silent for
// `timeout`: a switch whose service failed turns itself back.

#include <QObject>
#include <QTimer>

#include <functional>
#include <optional>
#include <utility>

class Requested {
public:
    // `changed` when what is shown changes because of the request (asked,
    // refused, timed out); the owner emits its notify signal there.
    explicit Requested(std::function<void()> changed, int timeoutMs = 5000)
        : m_changed(std::move(changed))
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(timeoutMs);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { drop(); });
    }

    // What to show, given the service's value.
    bool value(bool actual) const { return m_wanted.value_or(actual); }
    // The value asked for and not yet reported, if any.
    std::optional<bool> wanted() const { return m_wanted; }

    void ask(bool wanted)
    {
        m_wanted = wanted;
        m_timer.start();
        m_changed();
    }
    // The service's value, whenever it is read: the request is done once it
    // matches. The owner notifies as usual after reading.
    void reported(bool actual)
    {
        if (m_wanted == actual) {
            m_wanted.reset();
            m_timer.stop();
        }
    }
    // The service said no to `wanted` (an older request's refusal is ignored).
    void refused(bool wanted)
    {
        if (m_wanted == wanted) {
            drop();
        }
    }

private:
    void drop()
    {
        m_wanted.reset();
        m_timer.stop();
        m_changed();
    }

    std::function<void()> m_changed;
    std::optional<bool> m_wanted;
    QTimer m_timer;
};
