// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batteryalerts.h"

#include <utility>

std::optional<BatteryAlerts::Level> BatteryAlerts::classify(const BatteryReading& reading, const Settings& settings)
{
    if (!reading.present || !(reading.system ? settings.system : settings.devices)) {
        return std::nullopt;
    }
    if (reading.charging) {
        return Level::Normal;
    }
    if (reading.system) {
        // Critical is UPower's (just before its critical action: hibernate,
        // power off); low is the user's threshold (§4.2).
        if (reading.warning == BatteryReading::Warning::Critical) {
            return Level::Critical;
        }
        if (!reading.discharging || reading.percentage < 0) {
            return std::nullopt;
        }
        return reading.percentage <= settings.systemLow ? Level::Low : Level::Normal;
    }
    switch (reading.warning) {
    case BatteryReading::Warning::Unknown: return std::nullopt;
    case BatteryReading::Warning::None: return Level::Normal;
    case BatteryReading::Warning::Low: return Level::Low;
    case BatteryReading::Warning::Critical: return Level::Critical;
    }
    return std::nullopt;
}

std::vector<BatteryAlerts::Action> BatteryAlerts::rearm(Device& device)
{
    if (device.told == Level::Normal) {
        return {};
    }
    device.told = Level::Normal;
    return { { Action::Kind::Close, Level::Normal, device.last } };
}

std::vector<BatteryAlerts::Action> BatteryAlerts::evaluate(Device& device)
{
    // Charging (or charged) rearms: the next discharge is told again.
    if (device.last.charging && device.last.present) {
        device.rejoined = false;
        return rearm(device);
    }
    const std::optional<Level> level = classify(device.last, m_settings);
    if (!level) {
        return {};
    }
    const bool rejoined = std::exchange(device.rejoined, false);
    if (*level == Level::Normal) {
        // A normal charge while connected doesn't rearm: voltage-based
        // readings bounce between low and normal (§4.4).
        return rejoined ? rearm(device) : std::vector<Action> {};
    }
    if (*level <= device.told) {
        return {};
    }
    device.told = *level;
    return { { Action::Kind::Show, *level, device.last } };
}

std::vector<BatteryAlerts::Action> BatteryAlerts::update(const BatteryReading& reading)
{
    Device& device = m_devices[reading.id];
    if (std::exchange(device.away, false)) {
        device.rejoined = true;
    }
    device.last = reading;
    return evaluate(device);
}

std::vector<BatteryAlerts::Action> BatteryAlerts::removed(const QString& id)
{
    if (auto it = m_devices.find(id); it != m_devices.end()) {
        it->away = true;
    }
    return {};
}

std::vector<BatteryAlerts::Action> BatteryAlerts::configure(const Settings& settings)
{
    const Settings old = m_settings;
    m_settings = settings;
    std::vector<Action> actions;
    for (Device& device : m_devices) {
        const bool system = device.last.system;
        const bool turnedOff = system ? old.system && !settings.system : old.devices && !settings.devices;
        const bool thresholdMoved = system && settings.system && old.systemLow != settings.systemLow;
        const bool turnedOn = system ? !old.system && settings.system : !old.devices && settings.devices;
        if (!turnedOff && !thresholdMoved && !turnedOn) {
            continue;
        }
        // Turned off: whatever is shown goes. A threshold moved under the
        // charge: the low warning goes, and comes again at the new one.
        const std::optional<Level> level = classify(device.last, settings);
        if (turnedOff || (thresholdMoved && device.told == Level::Low && level == Level::Normal)) {
            for (Action& action : rearm(device)) {
                actions.push_back(std::move(action));
            }
        }
        if (!device.away) {
            for (Action& action : evaluate(device)) {
                actions.push_back(std::move(action));
            }
        }
    }
    return actions;
}
