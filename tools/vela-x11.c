// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-x11: a test X11 app (through Xwayland), so the X11 window tests do
// not depend on installed programs.
//
// Usage: vela-x11 [--menu X,Y] [WIDTH HEIGHT]   (default 300x200)
//        vela-x11 --fullscreen XID on|off
//        vela-x11 --configure XID WIDTH HEIGHT
//        vela-x11 --flood XID (180 ConfigureRequests)
//
// Opens an orange window with class "vela.x11" and title "vela-x11", which
// closes on WM_DELETE_WINDOW (Alt+F4). --menu also opens a green 120x80
// override-redirect menu at (X, Y) on the screen, like the drop-down menus
// of X11 apps. Like most apps, it draws only when the server asks (Expose).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static xcb_atom_t atom(xcb_connection_t* connection, const char* name)
{
    xcb_intern_atom_reply_t* reply
        = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t result = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return result;
}

static xcb_window_t create(xcb_connection_t* connection, xcb_screen_t* screen, int x, int y, int width, int height,
    uint32_t color, int overrideRedirect)
{
    xcb_window_t window = xcb_generate_id(connection);
    const uint32_t values[] = { color, (uint32_t)overrideRedirect, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, (int16_t)x, (int16_t)y,
        (uint16_t)width, (uint16_t)height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    return window;
}

int main(int argc, char** argv)
{
    // Control an existing X11 window from the functional test harness. These
    // modes don't create a window of their own.
    int control = argc >= 3 && (!strcmp(argv[1], "--fullscreen")
        || !strcmp(argv[1], "--configure") || !strcmp(argv[1], "--flood"));
    int width = 300;
    int height = 200;
    int menu = 0;
    int menuX = 0;
    int menuY = 0;
    int arg = 1;
    if (!control && arg + 1 < argc && !strcmp(argv[arg], "--menu")
        && sscanf(argv[arg + 1], "%d,%d", &menuX, &menuY) == 2) {
        menu = 1;
        arg += 2;
    }
    if (!control && arg + 1 < argc) {
        width = atoi(argv[arg]);
        height = atoi(argv[arg + 1]);
    }
    xcb_connection_t* connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "vela-x11: no X display (DISPLAY)\n");
        return 1;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    if (control) {
        xcb_window_t target = (xcb_window_t)strtoul(argv[2], NULL, 0);
        if (!strcmp(argv[1], "--fullscreen") && argc == 4) {
            xcb_client_message_event_t message = { 0 };
            message.response_type = XCB_CLIENT_MESSAGE;
            message.format = 32;
            message.window = target;
            message.type = atom(connection, "_NET_WM_STATE");
            message.data.data32[0] = !strcmp(argv[3], "on") ? 1 : 0; // ADD / REMOVE
            message.data.data32[1] = atom(connection, "_NET_WM_STATE_FULLSCREEN");
            message.data.data32[3] = 1; // application request
            xcb_send_event(connection, 0, screen->root,
                XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                (const char*)&message);
        } else if (!strcmp(argv[1], "--configure") && argc == 5) {
            const uint32_t dimensions[] = { (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]) };
            xcb_configure_window(connection, target,
                XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
        } else if (!strcmp(argv[1], "--flood") && argc == 3) {
            for (int i = 0; i < 180; ++i) {
                const uint32_t dimensions[] = { 320u + (uint32_t)(i % 8), 220u + (uint32_t)(i % 3) };
                xcb_configure_window(connection, target,
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
            }
        } else {
            fprintf(stderr, "vela-x11: invalid control arguments\n");
            xcb_disconnect(connection);
            return 2;
        }
        xcb_flush(connection);
        int failed = xcb_connection_has_error(connection);
        xcb_disconnect(connection);
        return failed ? 1 : 0;
    }
    xcb_window_t window = create(connection, screen, 0, 0, width, height, 0xff8000, 0);
    const char title[] = "vela-x11";
    const char wmClass[] = "vela-x11\0vela.x11"; // instance and class
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
        sizeof(title) - 1, title);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
        sizeof(wmClass), wmClass);
    xcb_atom_t protocols = atom(connection, "WM_PROTOCOLS");
    xcb_atom_t deleteWindow = atom(connection, "WM_DELETE_WINDOW");
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, protocols, XCB_ATOM_ATOM, 32, 1, &deleteWindow);
    xcb_map_window(connection, window);
    if (menu) {
        xcb_map_window(connection, create(connection, screen, menuX, menuY, 120, 80, 0x00c000, 1));
    }
    xcb_flush(connection);

    // One graphics context per color, made on first use.
    xcb_gcontext_t orange = XCB_NONE;
    xcb_gcontext_t green = XCB_NONE;
    xcb_generic_event_t* event;
    while ((event = xcb_wait_for_event(connection))) {
        int type = event->response_type & 0x7f;
        if (type == XCB_EXPOSE) {
            const xcb_expose_event_t* expose = (const xcb_expose_event_t*)event;
            xcb_gcontext_t* gc = expose->window == window ? &orange : &green;
            if (*gc == XCB_NONE) {
                uint32_t color = expose->window == window ? 0xff8000 : 0x00c000;
                *gc = xcb_generate_id(connection);
                xcb_create_gc(connection, *gc, expose->window, XCB_GC_FOREGROUND, &color);
            }
            const xcb_rectangle_t area = { (int16_t)expose->x, (int16_t)expose->y, expose->width, expose->height };
            xcb_poly_fill_rectangle(connection, expose->window, *gc, 1, &area);
            xcb_flush(connection);
        } else if (type == XCB_CLIENT_MESSAGE) {
            const xcb_client_message_event_t* message = (const xcb_client_message_event_t*)event;
            if (message->type == protocols && message->data.data32[0] == deleteWindow) {
                free(event);
                break;
            }
        }
        free(event);
    }
    xcb_disconnect(connection);
    return 0;
}
