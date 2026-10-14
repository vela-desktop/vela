// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-x11: a test X11 app (through Xwayland), so the X11 window tests do
// not depend on installed programs.
//
// Usage: vela-x11 [--menu X,Y] [--id-file PATH] [--wine-style HEX] [WIDTH HEIGHT]
//        vela-x11 --fullscreen XID on|off
//        vela-x11 --set-wine-style XID HEX
//        vela-x11 --delete-wine-style XID
//        vela-x11 --geometry XID (root-relative X11 x y width height)
//        vela-x11 [--start-fullscreen] [WIDTH HEIGHT]
//
// Opens an orange window with class "vela.x11" and title "vela-x11", which
// closes on WM_DELETE_WINDOW (Alt+F4). --menu also opens a green 120x80
// override-redirect menu at (X, Y) on the screen, like the drop-down menus
// of X11 apps. Like most apps, it draws only when the server asks (Expose).

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
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
    int width = 300;
    int height = 200;
    int menu = 0;
    int menuX = 0;
    int menuY = 0;
    const char* idFile = NULL;
    uint32_t wineStyle = 0;
    int hasWineStyle = 0;
    int startFullscreen = 0;
    int control = (argc == 4
        && (!strcmp(argv[1], "--fullscreen") || !strcmp(argv[1], "--set-wine-style")))
        || (argc == 3 && (!strcmp(argv[1], "--delete-wine-style")
            || !strcmp(argv[1], "--geometry")));
    int arg = 1;
    if (!control) {
        while (arg < argc) {
            if (!strcmp(argv[arg], "--start-fullscreen")) {
                startFullscreen = 1;
                ++arg;
                continue;
            }
            if (arg + 1 >= argc) {
                break;
            }
            if (!strcmp(argv[arg], "--menu")
                && sscanf(argv[arg + 1], "%d,%d", &menuX, &menuY) == 2) {
                menu = 1;
            } else if (!strcmp(argv[arg], "--id-file")) {
                idFile = argv[arg + 1];
            } else if (!strcmp(argv[arg], "--wine-style")) {
                wineStyle = (uint32_t)strtoul(argv[arg + 1], NULL, 0);
                hasWineStyle = 1;
            } else {
                break;
            }
            arg += 2;
        }
        if (arg + 1 < argc) {
            width = atoi(argv[arg]);
            height = atoi(argv[arg + 1]);
        }
    }
    xcb_connection_t* connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "vela-x11: no X display (DISPLAY)\n");
        return 1;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    if (control) {
        xcb_window_t target = (xcb_window_t)strtoul(argv[2], NULL, 0);
        if (!strcmp(argv[1], "--geometry")) {
            xcb_get_geometry_reply_t* geometry =
                xcb_get_geometry_reply(connection, xcb_get_geometry(connection, target), NULL);
            xcb_translate_coordinates_reply_t* location = xcb_translate_coordinates_reply(connection,
                xcb_translate_coordinates(connection, target, screen->root, 0, 0), NULL);
            if (!geometry || !location) {
                fprintf(stderr, "vela-x11: cannot query X11 window geometry\n");
                free(geometry);
                free(location);
                xcb_disconnect(connection);
                return 1;
            }
            printf("%d %d %u %u\n", location->dst_x, location->dst_y,
                (unsigned)geometry->width, (unsigned)geometry->height);
            free(geometry);
            free(location);
            xcb_disconnect(connection);
            return 0;
        } else if (!strcmp(argv[1], "--set-wine-style")) {
            uint32_t style = (uint32_t)strtoul(argv[3], NULL, 0);
            xcb_change_property(connection, XCB_PROP_MODE_REPLACE, target,
                atom(connection, "_WINE_HWND_STYLE"), XCB_ATOM_CARDINAL, 32, 1, &style);
        } else if (!strcmp(argv[1], "--delete-wine-style")) {
            xcb_delete_property(connection, target, atom(connection, "_WINE_HWND_STYLE"));
        } else {
            xcb_client_message_event_t event = { 0 };
            event.response_type = XCB_CLIENT_MESSAGE;
            event.window = target;
            event.type = atom(connection, "_NET_WM_STATE");
            event.format = 32;
            event.data.data32[0] = !strcmp(argv[3], "on") ? 1 : 0;
            event.data.data32[1] = atom(connection, "_NET_WM_STATE_FULLSCREEN");
            event.data.data32[3] = 1; // application source
            xcb_send_event(connection, 0, screen->root,
                XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                (const char*)&event);
        }
        xcb_flush(connection);
        int failed = xcb_connection_has_error(connection);
        xcb_disconnect(connection);
        return failed != 0;
    }
    xcb_window_t window = create(connection, screen, 0, 0, width, height, 0xff8000, 0);
    if (idFile) {
        // The XID control file must not be world-writable (CodeQL).
        int fd = open(idFile, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            perror("vela-x11: create XID file");
            xcb_disconnect(connection);
            return 1;
        }
        char xid[32];
        int len = snprintf(xid, sizeof(xid), "0x%x\n", window);
        if (len <= 0 || write(fd, xid, (size_t)len) != len) {
            perror("vela-x11: write XID file");
            close(fd);
            xcb_disconnect(connection);
            return 1;
        }
        close(fd);
    }
    if (hasWineStyle) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
            atom(connection, "_WINE_HWND_STYLE"), XCB_ATOM_CARDINAL, 32, 1, &wineStyle);
    }
    if (startFullscreen) {
        xcb_atom_t fullscreen = atom(connection, "_NET_WM_STATE_FULLSCREEN");
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
            atom(connection, "_NET_WM_STATE"), XCB_ATOM_ATOM, 32, 1, &fullscreen);
    }
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
