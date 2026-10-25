// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-input: simulates mouse and keyboard inside Vela, for automated tests.
// The compositor must be started with VELA_DEBUG_INPUT=1.
//
// Usage: vela-input ACTION [ACTION...]    (run in order)
//
//   move X Y                 moves the cursor to (X, Y), global coordinates
//   rel DX DY                moves the mouse by (DX, DY), like a real mouse
//                            (relative motion: games see it too)
//   down|up|click [BUTTON]   mouse button: left (default), right, middle,
//                            back, forward (the side buttons)
//   key COMBINATION          such as super+Left, alt+F4, Return, super (alone)
//   keydown|keyup KEY        holds / releases a key (such as alt for Alt+Tab)
//   type TEXT                types text (us layout)
//   scroll N                 the wheel: N notches, down if positive
//   sleep MS                 waits
//
// Example, dragging a window against the left edge:
//   vela-input move 640 100 down move 300 100 move 0 300 sleep 200 up

#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

static struct wl_display* display;
static struct wl_seat* seat;
static struct zwlr_virtual_pointer_manager_v1* pointerManager;
static struct zwp_virtual_keyboard_manager_v1* keyboardManager;
static struct zxdg_output_manager_v1* outputManager;

// Box containing all outputs: the virtual mouse's absolute coordinates are
// relative to it.
#define MAX_OUTPUTS 16
static struct wl_output* outputs[MAX_OUTPUTS];
static int outputCount;
static int layoutX1 = 1 << 30, layoutY1 = 1 << 30, layoutX2 = -(1 << 30), layoutY2 = -(1 << 30);
static struct {
    int x, y, w, h;
} outputBoxes[MAX_OUTPUTS];

static struct zwlr_virtual_pointer_v1* pointer;
static struct zwp_virtual_keyboard_v1* keyboard;
static struct xkb_keymap* keymap;
static struct xkb_state* xkbState;

static uint32_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ------------------------------------------------------------- registry --

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_seat_interface.name) && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    } else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
        pointerManager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
        keyboardManager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
        outputManager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
    } else if (!strcmp(interface, wl_output_interface.name) && outputCount < MAX_OUTPUTS) {
        outputs[outputCount++] = wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registryListener = { registryGlobal, registryRemove };

static void xdgPosition(void* data, struct zxdg_output_v1* o, int32_t x, int32_t y)
{
    int i = (int)(intptr_t)data;
    outputBoxes[i].x = x;
    outputBoxes[i].y = y;
}

static void xdgSize(void* data, struct zxdg_output_v1* o, int32_t w, int32_t h)
{
    int i = (int)(intptr_t)data;
    outputBoxes[i].w = w;
    outputBoxes[i].h = h;
}

static void xdgDone(void* data, struct zxdg_output_v1* o) { }
static void xdgName(void* data, struct zxdg_output_v1* o, const char* name) { }
static void xdgDescription(void* data, struct zxdg_output_v1* o, const char* d) { }

static const struct zxdg_output_v1_listener xdgOutputListener = {
    .logical_position = xdgPosition,
    .logical_size = xdgSize,
    .done = xdgDone,
    .name = xdgName,
    .description = xdgDescription,
};

// ---------------------------------------------------------------- mouse --

static int buttonCode(const char* name)
{
    if (!name || !strcmp(name, "left")) {
        return BTN_LEFT;
    }
    if (!strcmp(name, "right")) {
        return BTN_RIGHT;
    }
    if (!strcmp(name, "middle")) {
        return BTN_MIDDLE;
    }
    // The side buttons: Back and Forward in browsers and file managers.
    if (!strcmp(name, "back")) {
        return BTN_SIDE;
    }
    if (!strcmp(name, "forward")) {
        return BTN_EXTRA;
    }
    return -1;
}

static void pointerMove(int x, int y)
{
    const int width = layoutX2 - layoutX1;
    const int height = layoutY2 - layoutY1;
    x = x < layoutX1 ? layoutX1 : x >= layoutX2 ? layoutX2 - 1 : x;
    y = y < layoutY1 ? layoutY1 : y >= layoutY2 ? layoutY2 - 1 : y;
    zwlr_virtual_pointer_v1_motion_absolute(pointer, nowMs(), x - layoutX1, y - layoutY1, width, height);
    zwlr_virtual_pointer_v1_frame(pointer);
}

static void pointerMoveBy(double dx, double dy)
{
    zwlr_virtual_pointer_v1_motion(pointer, nowMs(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    zwlr_virtual_pointer_v1_frame(pointer);
}

static void pointerButton(int code, int pressed)
{
    zwlr_virtual_pointer_v1_button(pointer, nowMs(), code,
        pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
}

// ------------------------------------------------------------- keyboard --

static int setupKeyboard(void)
{
    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = "us" };
    keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        fprintf(stderr, "vela-input: can't create the keyboard layout\n");
        return 0;
    }
    xkbState = xkb_state_new(keymap);

    char* text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    const size_t size = strlen(text) + 1;
    const int fd = memfd_create("vela-input-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t)size) {
        perror("vela-input");
        return 0;
    }
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    close(fd);
    free(text);
    return 1;
}

// Finds the key producing `sym`; *shift says whether Shift is needed.
static int keycodeFor(xkb_keysym_t sym, int* shift)
{
    const xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    const xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
    for (int level = 0; level < 2; ++level) {
        for (xkb_keycode_t code = min; code <= max; ++code) {
            const xkb_keysym_t* syms;
            const int count = xkb_keymap_key_get_syms_by_level(keymap, code, 0, level, &syms);
            for (int i = 0; i < count; ++i) {
                if (syms[i] == sym) {
                    *shift = level == 1;
                    return (int)code;
                }
            }
        }
    }
    return -1;
}

static void sendKey(int xkbCode, int pressed)
{
    zwp_virtual_keyboard_v1_key(keyboard, nowMs(), xkbCode - 8, // xkb -> evdev
        pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    xkb_state_update_key(xkbState, xkbCode, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    zwp_virtual_keyboard_v1_modifiers(keyboard,
        xkb_state_serialize_mods(xkbState, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(xkbState, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(xkbState, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(xkbState, XKB_STATE_LAYOUT_EFFECTIVE));
    wl_display_roundtrip(display);
}

static xkb_keysym_t keysymFor(const char* name)
{
    static const struct {
        const char* alias;
        const char* keysym;
    } aliases[] = {
        { "super", "Super_L" }, { "logo", "Super_L" }, { "win", "Super_L" },
        { "ctrl", "Control_L" }, { "control", "Control_L" },
        { "alt", "Alt_L" }, { "shift", "Shift_L" },
        { "enter", "Return" }, { "esc", "Escape" },
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (!strcasecmp(name, aliases[i].alias)) {
            name = aliases[i].keysym;
            break;
        }
    }
    xkb_keysym_t sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) {
        sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    return sym;
}

// A single key, pressed or released.
static int pressKey(const char* name, int pressed)
{
    const xkb_keysym_t sym = keysymFor(name);
    int shift = 0;
    const int code = sym == XKB_KEY_NoSymbol ? -1 : keycodeFor(sym, &shift);
    if (code < 0) {
        fprintf(stderr, "vela-input: unknown key '%s'\n", name);
        return 0;
    }
    sendKey(code, pressed);
    return 1;
}

// "super+shift+Left": presses everything in order and releases in reverse.
static int pressCombo(const char* combo)
{
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", combo);
    int codes[16];
    int count = 0;
    for (char* part = strtok(buffer, "+"); part && count < 16; part = strtok(NULL, "+")) {
        const xkb_keysym_t sym = keysymFor(part);
        int shift = 0;
        const int code = sym == XKB_KEY_NoSymbol ? -1 : keycodeFor(sym, &shift);
        if (code < 0) {
            fprintf(stderr, "vela-input: unknown key '%s'\n", part);
            return 0;
        }
        codes[count++] = code;
    }
    for (int i = 0; i < count; ++i) {
        sendKey(codes[i], 1);
    }
    for (int i = count - 1; i >= 0; --i) {
        sendKey(codes[i], 0);
    }
    return 1;
}

static int typeText(const char* text)
{
    int shiftCode = -1;
    int unused;
    shiftCode = keycodeFor(XKB_KEY_Shift_L, &unused);
    for (const char* c = text; *c; ++c) {
        const xkb_keysym_t sym = xkb_utf32_to_keysym((unsigned char)*c);
        int shift = 0;
        const int code = keycodeFor(sym, &shift);
        if (code < 0) {
            fprintf(stderr, "vela-input: can't type '%c'\n", *c);
            return 0;
        }
        if (shift) {
            sendKey(shiftCode, 1);
        }
        sendKey(code, 1);
        sendKey(code, 0);
        if (shift) {
            sendKey(shiftCode, 0);
        }
    }
    return 1;
}

// ----------------------------------------------------------------- main --

// N wheel notches (15 units each, like libinput), down if N > 0.
static void pointerScroll(int steps)
{
    zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
    zwlr_virtual_pointer_v1_axis_discrete(
        pointer, nowMs(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(15 * steps), steps);
    zwlr_virtual_pointer_v1_frame(pointer);
}

static void usage(const char* program)
{
    fprintf(stderr,
        "Usage: %s ACTION [ACTION...]\n"
        "  move X Y | down [left|right|middle|back|forward] | up [...] | click [...]\n"
        "  key COMBO (e.g. super+Left) | keydown|keyup KEY | type TEXT | scroll N | sleep MS\n",
        program);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-input: no Wayland session (WAYLAND_DISPLAY)\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!pointerManager || !keyboardManager || !seat) {
        fprintf(stderr, "vela-input: virtual pointer and keyboard not available "
                        "(start Vela with VELA_DEBUG_INPUT=1)\n");
        return 1;
    }
    if (!outputManager || outputCount == 0) {
        fprintf(stderr, "vela-input: no outputs\n");
        return 1;
    }
    for (int i = 0; i < outputCount; ++i) {
        struct zxdg_output_v1* xdg = zxdg_output_manager_v1_get_xdg_output(outputManager, outputs[i]);
        zxdg_output_v1_add_listener(xdg, &xdgOutputListener, (void*)(intptr_t)i);
    }
    wl_display_roundtrip(display);
    for (int i = 0; i < outputCount; ++i) {
        if (outputBoxes[i].x < layoutX1) layoutX1 = outputBoxes[i].x;
        if (outputBoxes[i].y < layoutY1) layoutY1 = outputBoxes[i].y;
        if (outputBoxes[i].x + outputBoxes[i].w > layoutX2) layoutX2 = outputBoxes[i].x + outputBoxes[i].w;
        if (outputBoxes[i].y + outputBoxes[i].h > layoutY2) layoutY2 = outputBoxes[i].y + outputBoxes[i].h;
    }

    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointerManager, seat);
    keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboardManager, seat);
    if (!setupKeyboard()) {
        return 1;
    }
    wl_display_roundtrip(display);

    int ok = 1;
    for (int i = 1; i < argc && ok; ++i) {
        const char* action = argv[i];
        const char* arg1 = i + 1 < argc ? argv[i + 1] : NULL;
        const char* arg2 = i + 2 < argc ? argv[i + 2] : NULL;

        if (!strcmp(action, "move") && arg1 && arg2) {
            pointerMove(atoi(arg1), atoi(arg2));
            i += 2;
        } else if (!strcmp(action, "rel") && arg1 && arg2) {
            pointerMoveBy(atof(arg1), atof(arg2));
            i += 2;
        } else if (!strcmp(action, "down") || !strcmp(action, "up") || !strcmp(action, "click")) {
            int code = buttonCode(arg1);
            if (code >= 0 && arg1) {
                ++i; // the button was given
            } else {
                code = BTN_LEFT;
            }
            if (strcmp(action, "up")) {
                pointerButton(code, 1);
            }
            if (strcmp(action, "down")) {
                pointerButton(code, 0);
            }
        } else if (!strcmp(action, "key") && arg1) {
            ok = pressCombo(arg1);
            ++i;
        } else if ((!strcmp(action, "keydown") || !strcmp(action, "keyup")) && arg1) {
            ok = pressKey(arg1, !strcmp(action, "keydown"));
            ++i;
        } else if (!strcmp(action, "scroll") && arg1) {
            pointerScroll(atoi(arg1));
            ++i;
        } else if (!strcmp(action, "type") && arg1) {
            ok = typeText(arg1);
            ++i;
        } else if (!strcmp(action, "sleep") && arg1) {
            wl_display_roundtrip(display);
            usleep((useconds_t)atoi(arg1) * 1000);
            ++i;
        } else {
            fprintf(stderr, "vela-input: invalid action '%s'\n", action);
            usage(argv[0]);
            ok = 0;
        }
        wl_display_roundtrip(display);
    }

    zwlr_virtual_pointer_v1_destroy(pointer);
    zwp_virtual_keyboard_v1_destroy(keyboard);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return ok ? 0 : 1;
}
