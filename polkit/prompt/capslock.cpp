// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "capslock.h"

#include <QGuiApplication>

#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

namespace {

void onEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*)
{
}
void onLeave(void*, wl_keyboard*, uint32_t, wl_surface*)
{
}
void onKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t)
{
}
void onRepeat(void*, wl_keyboard*, int32_t, int32_t)
{
}

} // namespace

CapsLock::CapsLock(QObject* parent)
    : QObject(parent)
{
}

CapsLock::~CapsLock()
{
    if (m_keyboard) {
        wl_keyboard_release(m_keyboard);
    }
    xkb_keymap_unref(m_keymap);
    xkb_context_unref(m_context);
}

bool CapsLock::locked(xkb_keymap* keymap, uint32_t locked)
{
    const xkb_mod_index_t index = keymap ? xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS) : XKB_MOD_INVALID;
    return index != XKB_MOD_INVALID && index < 32 && (locked & (uint32_t(1) << index));
}

bool CapsLock::start()
{
    if (m_keyboard) {
        return true;
    }
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_seat* seat = wayland ? wayland->seat() : nullptr;
    // wl_seat.get_keyboard on a seat that has never had a keyboard is a
    // protocol error, which would end the prompt: only once Qt has one.
    if (!seat || !wayland->keyboard()) {
        return false;
    }
    m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    m_keyboard = wl_seat_get_keyboard(seat);
    static const wl_keyboard_listener listener {
        CapsLock::onKeymap,
        onEnter,
        onLeave,
        onKey,
        CapsLock::onModifiers,
        onRepeat,
    };
    wl_keyboard_add_listener(m_keyboard, &listener, this);
    return true;
}

void CapsLock::onKeymap(void* data, wl_keyboard*, uint32_t format, int fd, uint32_t size)
{
    auto* self = static_cast<CapsLock*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !self->m_context) {
        close(fd);
        return;
    }
    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) {
        return;
    }
    xkb_keymap* keymap =
        xkb_keymap_new_from_string(self->m_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (keymap) {
        xkb_keymap_unref(self->m_keymap);
        self->m_keymap = keymap;
    }
}

void CapsLock::onModifiers(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t locked, uint32_t)
{
    auto* self = static_cast<CapsLock*>(data);
    const bool on = CapsLock::locked(self->m_keymap, locked);
    if (on != self->m_on) {
        self->m_on = on;
        emit self->changed(on);
    }
}
