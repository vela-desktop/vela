// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keyboard.h"

#include "alloc/alloc.h"
#include "listen.h"
#include "switcher.h"
#include "lock.h"
#include "layer.h"
#include "output.h"
#include "scene/scene.h"
#include "view.h"
#include "workspace.h"
#include "interact.h"
#include "bindings.h"
#include "a11y.h"
#include "config.h"
#include "focus.h"
#include "input.h"
#include "keymap_options.h"
#include "server.h"
#include "shell.h"
#include "super_numbers.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// The XKB names of a layout; empty: not chosen.
struct keymap_names {
    char rules[64];
    char model[64];
    char layout[256];
    char variant[256];
    char options[512];
};

static void copy(char *out, size_t size, const char *value)
{
    snprintf(out, size, "%s", value);
}

// From systemd-localed: Option "XkbLayout" "us".
static void localed_keymap(struct keymap_names *names)
{
    FILE *file = fopen("/etc/X11/xorg.conf.d/00-keyboard.conf", "re");
    if (!file) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        // The first word is "Option", then two quoted strings.
        char *word = line + strspn(line, " \t");
        if (strncmp(word, "Option", 6) != 0 || !strchr(" \t", word[6]) || word[6] == '\0') {
            continue;
        }
        char *q1 = strchr(word + 6, '"');
        char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        char *q3 = q2 ? strchr(q2 + 1, '"') : NULL;
        char *q4 = q3 ? strchr(q3 + 1, '"') : NULL;
        if (!q4) {
            continue;
        }
        *q2 = '\0';
        *q4 = '\0';
        const char *key = q1 + 1;
        const char *value = q3 + 1;
        if (strcmp(key, "XkbLayout") == 0) {
            copy(names->layout, sizeof(names->layout), value);
        } else if (strcmp(key, "XkbVariant") == 0) {
            copy(names->variant, sizeof(names->variant), value);
        } else if (strcmp(key, "XkbModel") == 0) {
            copy(names->model, sizeof(names->model), value);
        } else if (strcmp(key, "XkbOptions") == 0) {
            copy(names->options, sizeof(names->options), value);
        }
    }
    fclose(file);
}

static void keymap_from_settings(struct keymap_names *names, const struct vela_config *settings)
{
    copy(names->layout, sizeof(names->layout), vela_config_get(settings, "keyboard-layout", ""));
    copy(names->variant, sizeof(names->variant), vela_config_get(settings, "keyboard-variant", ""));
}

// The environment variable, if set; otherwise the chosen name (NULL if empty).
static const char *pick(const char *env, const char *fallback)
{
    const char *value = getenv(env);
    if (value && *value) {
        return value;
    }
    return *fallback ? fallback : NULL;
}

static struct xkb_keymap *system_keymap(struct vela_input *input, struct xkb_context *context,
    const struct vela_config *settings)
{
    struct keymap_names names = { 0 };
    keymap_from_settings(&names, settings);
    struct keymap_names system = { 0 };
    localed_keymap(&system);
    const char *origin = "Vela settings";
    if (!names.layout[0]) {
        names = system;
        origin = "localectl";
    }
    // The options are Settings' whenever vela.conf has the key, whichever
    // layout is in use (docs/keyboard.md §6).
    if (!vela_keymap_options(names.layout, system.options, vela_config_get(settings, "keyboard-options", NULL),
            names.options, sizeof(names.options))) {
        wlr_log(WLR_ERROR, "Keyboard: the XKB options are too long, none used");
    }
    const struct xkb_rule_names rules = {
        .rules = pick("XKB_DEFAULT_RULES", names.rules),
        .model = pick("XKB_DEFAULT_MODEL", names.model),
        .layout = pick("XKB_DEFAULT_LAYOUT", names.layout),
        .variant = pick("XKB_DEFAULT_VARIANT", names.variant),
        .options = pick("XKB_DEFAULT_OPTIONS", names.options),
    };
    // Logged only when it changes (a line per keyboard would be noise).
    char *logged = input->keymap_logged;
    char description[1024];
    snprintf(description, sizeof(description), "%s/%s/%s", rules.layout ? rules.layout : "",
        rules.variant ? rules.variant : "", rules.options ? rules.options : "");
    if (strcmp(description, logged) != 0) {
        copy(logged, sizeof(input->keymap_logged), description);
        wlr_log(WLR_INFO, "Keyboard: layout %s, variant %s%s%s (from %s)", rules.layout ? rules.layout : "us",
            rules.variant ? rules.variant : "-", rules.options ? ", options " : "", rules.options ? rules.options : "",
            getenv("XKB_DEFAULT_LAYOUT") ? "XKB_DEFAULT_*" : !names.layout[0] ? "default" : origin);
    }
    return xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

void vela_keyboard_apply_settings(struct vela_keyboard *keyboard, const struct vela_config *settings)
{
    // A virtual keyboard (automated tests) brings its own layout.
    if (wlr_input_device_get_virtual_keyboard(&keyboard->wlr->base)) {
        return;
    }
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = system_keymap(keyboard->input, context, settings);
    if (!keymap) {
        wlr_log(WLR_ERROR, "Invalid XKB layout, using the default one");
        const struct xkb_rule_names fallback = { .layout = "us" };
        keymap = xkb_keymap_new_from_names(context, &fallback, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    wlr_keyboard_set_keymap(keyboard->wlr, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    // Key repeat: 400 ms delay and 30 characters/s by default (close to
    // Windows').
    int delay = atoi(vela_config_get(settings, "keyboard-repeat-delay", "400"));
    int rate = atoi(vela_config_get(settings, "keyboard-repeat-rate", "30"));
    wlr_keyboard_set_repeat_info(keyboard->wlr, vela_clamp(rate, 1, 100), vela_clamp(delay, 100, 2000));
}

// ------------------------------------------------------------------- keys --

static bool is_super(xkb_keysym_t sym)
{
    return sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R;
}

static bool is_alt(xkb_keysym_t sym)
{
    return sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Meta_L || sym == XKB_KEY_Meta_R;
}

static void handle_modifiers(struct wl_listener *listener, void *data)
{
    struct vela_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    struct vela_server *server = keyboard->input->server;
    struct vela_a11y *a11y = server->a11y;
    struct wlr_keyboard *wlr = keyboard->wlr;
    // Sticky keys: whoever sends modifiers itself (virtual keyboards) doesn't
    // know about the latched ones: they are put back.
    const struct wlr_keyboard_modifiers *m = &wlr->modifiers;
    uint32_t latched = m->latched | a11y->latched;
    uint32_t locked = m->locked | a11y->locked;
    if (a11y->sticky_keys && !keyboard->restoring && (latched != m->latched || locked != m->locked)) {
        keyboard->restoring = true;
        wlr_keyboard_notify_modifiers(wlr, m->depressed, latched, locked, m->group); // calls back here
        keyboard->restoring = false;
        return;
    }
    wlr_seat_set_keyboard(server->seat, wlr);
    wlr_seat_keyboard_notify_modifiers(server->seat, &wlr->modifiers);
}

static void stop_repeat(struct vela_keyboard *keyboard)
{
    if (keyboard->repeat) {
        wl_event_source_timer_update(keyboard->repeat, 0);
    }
    keyboard->repeat_keycode = 0;
}

static int repeat_binding(void *data)
{
    struct vela_keyboard *keyboard = data;
    int rate = keyboard->wlr->repeat_info.rate;
    if (!keyboard->repeat_keycode || rate <= 0) {
        return 0;
    }
    vela_bindings_handle(keyboard->input->server, keyboard->repeat_mods, keyboard->repeat_sym);
    wl_event_source_timer_update(keyboard->repeat, vela_max(1, 1000 / rate));
    return 0;
}

static void start_repeat(struct vela_keyboard *keyboard, uint32_t keycode, xkb_keysym_t sym, uint32_t mods)
{
    if (keyboard->wlr->repeat_info.rate <= 0) {
        return;
    }
    if (!keyboard->repeat) {
        struct wl_event_loop *loop = wl_display_get_event_loop(keyboard->input->server->display);
        keyboard->repeat = wl_event_loop_add_timer(loop, repeat_binding, keyboard);
        if (!keyboard->repeat) {
            wlr_log_errno(WLR_ERROR, "Keyboard: no repeat timer, the binding runs once");
            return;
        }
    }
    keyboard->repeat_keycode = keycode;
    keyboard->repeat_sym = sym;
    keyboard->repeat_mods = mods;
    wl_event_source_timer_update(keyboard->repeat, keyboard->wlr->repeat_info.delay);
}

static int show_super_hints(void *data)
{
    struct vela_input *input = data;
    struct vela_server *server = input->server;
    struct vela_view *active = vela_views_focused(server);
    if (input->super_tap && !server->locked && !vela_focus_modal_layer(server) && !vela_input_shortcuts_inhibited(input)
        && (!active || !active->fullscreen || active->tree->node.parent != server->layers.fullscreen)) {
        input->super_hints_visible = vela_shell_send(server, "super-held");
    }
    return 0;
}

static bool handle_super_number(struct vela_input *input, unsigned index, uint32_t mods)
{
    struct vela_server *server = input->server;
    const struct vela_super_modifiers held = {
        .logo = mods & WLR_MODIFIER_LOGO,
        .ctrl = mods & WLR_MODIFIER_CTRL,
        .alt = mods & WLR_MODIFIER_ALT,
        .shift = mods & WLR_MODIFIER_SHIFT,
    };
    const enum vela_super_number_action action = vela_super_number_action(input->super_numbers, held);
    struct vela_view *active = vela_views_focused(server);
    switch (action) {
    case VELA_SUPER_NUMBER_NONE: return false;
    case VELA_SUPER_NUMBER_MOVE:
        if ((int)index < server->workspaces->count && active) {
            vela_workspaces_move_view(server, active, (int)index);
        }
        return true;
    case VELA_SUPER_NUMBER_SWITCH:
        if ((int)index < server->workspaces->count) {
            vela_workspaces_switch(server, (int)index, true);
        }
        return true;
    default: break;
    }

    struct vela_output *output = active ? vela_view_output(active) : NULL;
    struct vela_layer_surface *layer = server->focused_layer;
    if (!output && layer && strcmp(layer->wlr->namespace, "vela-taskbar-preview") == 0) {
        output = vela_layer_surface_output(layer);
    }
    if (!output) {
        output = vela_output_under_cursor(server);
    }
    char line[256];
    if (vela_format(line, sizeof(line), "taskbar-app %u %s %s", index + 1, vela_super_number_verb(action),
            output ? output->wlr->name : "")) {
        vela_shell_send(server, line);
    }
    return true;
}

static void handle_key(struct wl_listener *listener, void *data)
{
    struct vela_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct vela_input *input = keyboard->input;
    struct vela_server *server = input->server;
    struct wlr_keyboard_key_event *event = data;
    uint32_t keycode = event->keycode + 8; // libinput -> xkb
    const xkb_keysym_t *syms = NULL;
    int count = xkb_state_key_get_syms(keyboard->wlr->xkb_state, keycode, &syms);
    // The modifiers here are those held BEFORE this key.
    uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr);
    bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    // Like the apps' repeat: another key pressed, or that one released, stops
    // it.
    if (pressed || event->keycode == keyboard->repeat_keycode) {
        stop_repeat(keyboard);
    }

    // The release of a number Super took is the compositor's, even when it
    // comes during a keyboard Move: its bit goes first, or the next plain
    // press would reach the app without its release.
    const int number = vela_super_number_index(event->keycode);
    const uint16_t number_bit = number >= 0 ? (uint16_t)(1u << number) : 0;
    const bool number_released = !pressed && (keyboard->handled_numbers & number_bit);
    if (number_released) {
        keyboard->handled_numbers &= (uint16_t)~number_bit;
    }

    // Keyboard "Move"/"Size": all keys belong to the compositor.
    if (vela_interact_keyboard(server, syms, count, mods, pressed)) {
        return;
    }

    bool handled = false;
    // A focused app that keeps the shortcuts (virtual machine, remote desktop)
    // also gets the Super key alone.
    // The polkit dialog turns them off too (vela_focus_modal_layer).
    bool inhibited = vela_input_shortcuts_inhibited(input) || server->locked || vela_focus_modal_layer(server);
    vela_lock_note_activity(server->lock);
    // Sticky keys: a modifier pressed and released applies to the next key
    // (this key's modifiers, `mods`, already include it); they are let go when
    // that key is released.
    const bool sticky = vela_a11y_sticky_key(server->a11y, keyboard->wlr, syms, count, pressed);
    if (sticky) {
        input->super_tap = false;
        vela_input_hide_super_hints(input);
    }
    bool super = false;
    for (int i = 0; i < count; ++i) {
        super = super || is_super(syms[i]);
    }
    if (pressed && !super) {
        vela_input_hide_super_hints(input);
    }
    if (super && !pressed) {
        vela_input_hide_super_hints(input);
        if (input->super_keyboard == keyboard) {
            input->super_keyboard = NULL;
            if (!inhibited) {
                vela_shell_send(server, "super-released");
            }
        }
    }
    if (number >= 0) {
        if (pressed && !inhibited) {
            // Taken before the action runs: a desktop switch focuses the
            // window there now, and its enter must leave this key out
            // (vela_input_keyboard_enter).
            keyboard->handled_numbers |= number_bit;
            handled = handle_super_number(input, (unsigned)number, mods);
            if (!handled) {
                keyboard->handled_numbers &= (uint16_t)~number_bit;
            }
        } else if (number_released) {
            handled = true;
        }
    }
    for (int i = 0; i < count; ++i) {
        if (pressed) {
            if (is_super(syms[i]) && !inhibited) {
                input->super_tap = !sticky && (mods & ~WLR_MODIFIER_LOGO) == 0;
                if (input->super_numbers == VELA_SUPER_NUMBERS_APPS) {
                    input->super_keyboard = keyboard;
                    if (input->super_tap) {
                        if (!input->super_timer) {
                            input->super_timer = wl_event_loop_add_timer(server->loop, show_super_hints, input);
                        }
                        if (input->super_timer) {
                            wl_event_source_timer_update(input->super_timer, 500);
                        }
                    }
                }
                continue;
            }
            input->super_tap = false;
            if (!handled && vela_bindings_handle(server, mods, syms[i])) {
                handled = true;
                if (vela_bindings_repeats(syms[i])) {
                    start_repeat(keyboard, event->keycode, syms[i], mods);
                }
            }
            continue;
        }
        if (is_super(syms[i]) && input->super_tap) {
            input->super_tap = false;
            // The shell may need a full Qt/Wayland map round-trip. Show the
            // taskbar in the current compositor tick after a successful send,
            // without altering the game's fullscreen protocol state.
            if (vela_shell_send(server, "toggle-start")) {
                vela_fullscreen_preview_start(server);
            }
        }
        // Alt released during Alt+Tab: switch to the chosen window.
        if (is_alt(syms[i]) && vela_switcher_active(server)) {
            vela_switcher_finish(server, true);
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, keyboard->wlr);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
    }
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct vela_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    vela_keyboard_destroy(keyboard);
}

struct vela_keyboard *vela_keyboard_create(struct vela_input *input, struct wlr_keyboard *wlr)
{
    struct vela_keyboard *keyboard = VELA_NEW(struct vela_keyboard);
    keyboard->input = input;
    keyboard->wlr = wlr;
    struct vela_config settings;
    vela_config_read(&settings);
    vela_keyboard_apply_settings(keyboard, &settings);
    vela_config_finish(&settings);

    vela_listen(&wlr->events.modifiers, &keyboard->modifiers, handle_modifiers);
    vela_listen(&wlr->events.key, &keyboard->key, handle_key);
    vela_listen(&wlr->base.events.destroy, &keyboard->destroy, handle_destroy);

    wlr_seat_set_keyboard(input->server->seat, wlr);
    wl_list_insert(input->keyboards.prev, &keyboard->link);
    return keyboard;
}

void vela_keyboard_destroy(struct vela_keyboard *keyboard)
{
    if (keyboard->input->super_keyboard == keyboard) {
        keyboard->input->super_keyboard = NULL;
        keyboard->input->super_tap = false;
        vela_input_hide_super_hints(keyboard->input);
    }
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    if (keyboard->repeat) {
        wl_event_source_remove(keyboard->repeat);
    }
    free(keyboard);
}
