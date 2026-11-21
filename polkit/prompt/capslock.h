// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Caps Lock's real state for the password field (docs/keyboard.md §8.2): Qt
// doesn't tell it, so a second wl_keyboard on Qt's own wl_seat listens to
// the keymap and the modifiers the compositor sends this client.

#include <QObject>

#include <cstdint>

struct wl_keyboard;
struct xkb_context;
struct xkb_keymap;

class CapsLock : public QObject {
    Q_OBJECT

public:
    explicit CapsLock(QObject* parent = nullptr);
    ~CapsLock() override;

    // Starts listening on the application's Wayland seat; false while it
    // has no keyboard (try again when one gives a window the focus).
    bool start();
    bool on() const { return m_on; }

    // Whether Caps Lock is in `locked` (a wl_keyboard.modifiers mask) of
    // `keymap`, by the modifier's name.
    static bool locked(xkb_keymap* keymap, uint32_t locked);

signals:
    void changed(bool on);

private:
    static void onKeymap(void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size);
    static void onModifiers(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t locked, uint32_t);

    wl_keyboard* m_keyboard = nullptr;
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    bool m_on = false;
};
