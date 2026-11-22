// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Caps Lock's real state for the password field (docs/keyboard.md §8.2): Qt
// doesn't tell it, so a wl_keyboard of our own listens to the keymap and the
// modifiers the compositor sends this client. It hangs off our own binding
// of the seat, whose capabilities say when to take it and when to let it go:
// after the seat loses its keyboard capability (the last keyboard unplugged,
// then a mouse plugged in), the old wl_keyboard gets no more events.

#include <QObject>

#include <cstdint>

struct wl_keyboard;
struct wl_registry;
struct wl_seat;
struct xkb_context;
struct xkb_keymap;

class CapsLock : public QObject {
    Q_OBJECT

public:
    explicit CapsLock(QObject* parent = nullptr);
    ~CapsLock() override;

    // Starts listening on the application's Wayland display; false off
    // Wayland. The keyboard follows the seat from then on.
    bool start();
    bool on() const { return m_on; }

    // Whether Caps Lock is in `locked` (a wl_keyboard.modifiers mask) of
    // `keymap`, by the modifier's name.
    static bool locked(xkb_keymap* keymap, uint32_t locked);

signals:
    void changed(bool on);

private:
    static void onGlobal(void* data, wl_registry*, uint32_t name, const char* interface, uint32_t version);
    static void onCapabilities(void* data, wl_seat*, uint32_t capabilities);
    static void onKeymap(void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size);
    static void onModifiers(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t locked, uint32_t);

    wl_registry* m_registry = nullptr;
    wl_seat* m_seat = nullptr;
    wl_keyboard* m_keyboard = nullptr;
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    bool m_on = false;
};
