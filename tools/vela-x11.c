// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-x11: a test X11 app (through Xwayland), so the X11 window tests do
// not depend on installed programs.
//
// Usage: vela-x11 [--menu X,Y] [--id-file PATH] [--backdrop-id-file PATH]
//                 [--wine-style HEX] [--wine-backdrop] [WIDTH HEIGHT]
//        vela-x11 --fullscreen XID on|off
//        vela-x11 --iconify XID (ICCCM WM_CHANGE_STATE IconicState)
//        vela-x11 --state XID below|skip-taskbar|maximize on|off
//        vela-x11 --override-redirect XID on|off
//        vela-x11 --set-wine-style XID HEX
//        vela-x11 --delete-wine-style XID
//        vela-x11 --geometry XID (root-relative X11 x y width height)
//        vela-x11 --move XID X Y (request a fullscreen monitor change)
//        vela-x11 --resize XID WIDTH HEIGHT (no position request)
//        vela-x11 --root-size (current X11 root width height)
//        vela-x11 --pointer XID (X11 pointer position relative to a window)
//        vela-x11 [--start-fullscreen] [WIDTH HEIGHT]
//
// Opens an orange window with class "vela.x11" and title "vela-x11", which
// closes on WM_DELETE_WINDOW (Alt+F4). --menu also opens a green 120x80
// override-redirect menu at (X, Y) on the screen, like the drop-down menus
// of X11 apps. Like most apps, it draws only when the server asks (Expose).
// --wine-backdrop also opens Wine's black fullscreen backdrop (same class):
// override-redirect and as large as the screen; once it is shown the orange
// window asks for fullscreen and the backdrop becomes managed, asking for
// fullscreen, skip-taskbar/pager and below after mapping.

#include <fcntl.h>
#include <stdint.h>
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

static int write_xid_file(const char* path, xcb_window_t window)
{
    // O_EXCL prevents replacing an existing file or following a symlink.
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror("vela-x11: create XID file");
        return -1;
    }
    char xid[32];
    int len = snprintf(xid, sizeof(xid), "0x%x\n", window);
    int ok = len > 0 && write(fd, xid, (size_t)len) == len;
    if (!ok) {
        perror("vela-x11: write XID file");
    }
    close(fd);
    return ok ? 0 : -1;
}

static void send_state(xcb_connection_t* connection, xcb_screen_t* screen, xcb_window_t window, const char* first,
    const char* second)
{
    xcb_client_message_event_t event = { 0 };
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = window;
    event.type = atom(connection, "_NET_WM_STATE");
    event.format = 32;
    event.data.data32[0] = 1; // add
    event.data.data32[1] = atom(connection, first);
    event.data.data32[2] = second ? atom(connection, second) : XCB_ATOM_NONE;
    event.data.data32[3] = 1; // application source
    xcb_send_event(connection, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
        (const char*)&event);
}

// Wine's order: the game goes fullscreen; the backdrop is unmapped, drops
// override-redirect, is mapped again, then asks for its states.
static void wine_backdrop_managed(xcb_connection_t* connection, xcb_screen_t* screen, xcb_window_t game,
    xcb_window_t backdrop)
{
    send_state(connection, screen, game, "_NET_WM_STATE_FULLSCREEN", NULL);
    xcb_unmap_window(connection, backdrop);
    const uint32_t managed = 0;
    xcb_change_window_attributes(connection, backdrop, XCB_CW_OVERRIDE_REDIRECT, &managed);
    xcb_map_window(connection, backdrop);
    send_state(connection, screen, backdrop, "_NET_WM_STATE_FULLSCREEN", NULL);
    send_state(connection, screen, backdrop, "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER");
    send_state(connection, screen, backdrop, "_NET_WM_STATE_BELOW", NULL);
    xcb_flush(connection);
}

int main(int argc, char** argv)
{
    int width = 300;
    int height = 200;
    int menu = 0;
    int menuX = 0;
    int menuY = 0;
    const char* idFile = NULL;
    const char* backdropIdFile = NULL;
    uint32_t wineStyle = 0;
    int hasWineStyle = 0;
    int startFullscreen = 0;
    int wineBackdrop = 0;
    int control = (argc == 4
        && (!strcmp(argv[1], "--fullscreen") || !strcmp(argv[1], "--set-wine-style")
            || !strcmp(argv[1], "--override-redirect")))
        || (argc == 5 && (!strcmp(argv[1], "--state") || !strcmp(argv[1], "--move")
            || !strcmp(argv[1], "--resize")))
        || (argc == 3 && (!strcmp(argv[1], "--delete-wine-style")
            || !strcmp(argv[1], "--geometry") || !strcmp(argv[1], "--iconify")
            || !strcmp(argv[1], "--pointer")))
        || (argc == 2 && !strcmp(argv[1], "--root-size"));
    int arg = 1;
    if (!control) {
        while (arg < argc) {
            if (!strcmp(argv[arg], "--start-fullscreen")) {
                startFullscreen = 1;
                ++arg;
                continue;
            }
            if (!strcmp(argv[arg], "--wine-backdrop")) {
                wineBackdrop = 1;
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
            } else if (!strcmp(argv[arg], "--backdrop-id-file")) {
                backdropIdFile = argv[arg + 1];
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
        if (!strcmp(argv[1], "--root-size")) {
            xcb_get_geometry_reply_t* geometry =
                xcb_get_geometry_reply(connection, xcb_get_geometry(connection, screen->root), NULL);
            if (!geometry) {
                fprintf(stderr, "vela-x11: cannot query root geometry\n");
                xcb_disconnect(connection);
                return 1;
            }
            printf("%u %u\n", (unsigned)geometry->width, (unsigned)geometry->height);
            free(geometry);
            xcb_disconnect(connection);
            return 0;
        }
        xcb_window_t target = (xcb_window_t)strtoul(argv[2], NULL, 0);
        if (!strcmp(argv[1], "--pointer")) {
            // X11 client-visible pointer coordinates, for testing the
            // Wayland logical -> XWayland physical coordinate transform.
            xcb_query_pointer_reply_t* reply = xcb_query_pointer_reply(
                connection, xcb_query_pointer(connection, target), NULL);
            if (!reply) {
                fprintf(stderr, "vela-x11: X11 pointer query failed\n");
                xcb_disconnect(connection);
                return 1;
            }
            printf("%d %d %d\n", reply->win_x, reply->win_y, reply->same_screen);
            free(reply);
            xcb_disconnect(connection);
            return 0;
        }
        if (!strcmp(argv[1], "--iconify")) {
            // ICCCM: XIconifyWindow asks the window manager to change a
            // managed window to IconicState using a WM_CHANGE_STATE message.
            // A bridge like Satellite must forward this as XDG minimize.
            xcb_client_message_event_t event = { 0 };
            event.response_type = XCB_CLIENT_MESSAGE;
            event.window = target;
            event.type = atom(connection, "WM_CHANGE_STATE");
            event.format = 32;
            event.data.data32[0] = 3; // ICCCM IconicState
            xcb_void_cookie_t cookie = xcb_send_event_checked(connection, 0, screen->root,
                XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                (const char*)&event);
            xcb_generic_error_t* error = xcb_request_check(connection, cookie);
            if (error) {
                fprintf(stderr, "vela-x11: WM_CHANGE_STATE failed (X11 error %u)\n", error->error_code);
                free(error);
                xcb_disconnect(connection);
                return 1;
            }
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 0;
        }
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
        }
        if (!strcmp(argv[1], "--move") || !strcmp(argv[1], "--resize")) {
            int first = atoi(argv[3]);
            int second = atoi(argv[4]);
            uint32_t values[2] = { (uint32_t)first, (uint32_t)second };
            const uint16_t mask = !strcmp(argv[1], "--move")
                ? XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
                : XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
            xcb_void_cookie_t request = xcb_configure_window_checked(connection, target, mask, values);
            xcb_generic_error_t* error = xcb_request_check(connection, request);
            if (error) {
                fprintf(stderr, "vela-x11: configure request failed (X11 error %d)\\n", error->error_code);
                free(error);
                xcb_disconnect(connection);
                return 1;
            }
            xcb_disconnect(connection);
            return 0;
        }
        // Checked, and waited for: the server must have handled the request
        // before we disconnect (Xwayland can drop the last request of a client
        // that closes right after sending it), and a rejected one is a failure.
        xcb_void_cookie_t cookie;
        if (!strcmp(argv[1], "--set-wine-style")) {
            uint32_t style = (uint32_t)strtoul(argv[3], NULL, 0);
            cookie = xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE, target,
                atom(connection, "_WINE_HWND_STYLE"), XCB_ATOM_CARDINAL, 32, 1, &style);
        } else if (!strcmp(argv[1], "--delete-wine-style")) {
            cookie = xcb_delete_property_checked(connection, target, atom(connection, "_WINE_HWND_STYLE"));
        } else if (!strcmp(argv[1], "--override-redirect")) {
            if (strcmp(argv[3], "on") && strcmp(argv[3], "off")) {
                fprintf(stderr, "vela-x11: expected on or off\n");
                xcb_disconnect(connection);
                return 2;
            }
            uint32_t redirect = !strcmp(argv[3], "on");
            cookie = xcb_change_window_attributes_checked(connection, target, XCB_CW_OVERRIDE_REDIRECT, &redirect);
        } else {
            const int setState = !strcmp(argv[1], "--state");
            const char* requested = setState ? argv[3] : "fullscreen";
            const char* desired = setState ? argv[4] : argv[3];
            if ((strcmp(requested, "fullscreen") && strcmp(requested, "below")
                    && strcmp(requested, "skip-taskbar") && strcmp(requested, "maximize"))
                || (strcmp(desired, "on") && strcmp(desired, "off"))) {
                fprintf(stderr, "vela-x11: invalid state (expected fullscreen, maximize, below or skip-taskbar; on/off)\n");
                xcb_disconnect(connection);
                return 2;
            }
            const char* name = !strcmp(requested, "below") ? "_NET_WM_STATE_BELOW"
                : !strcmp(requested, "skip-taskbar") ? "_NET_WM_STATE_SKIP_TASKBAR"
                : !strcmp(requested, "maximize") ? "_NET_WM_STATE_MAXIMIZED_HORZ"
                : "_NET_WM_STATE_FULLSCREEN";
            xcb_client_message_event_t event = { 0 };
            event.response_type = XCB_CLIENT_MESSAGE;
            event.window = target;
            event.type = atom(connection, "_NET_WM_STATE");
            event.format = 32;
            event.data.data32[0] = !strcmp(desired, "on") ? 1 : 0;
            event.data.data32[1] = atom(connection, name);
            // ICCCM/EWMH maximize request toggles both axes atomically.
            if (!strcmp(requested, "maximize")) {
                event.data.data32[2] = atom(connection, "_NET_WM_STATE_MAXIMIZED_VERT");
            }
            event.data.data32[3] = 1; // application source
            cookie = xcb_send_event_checked(connection, 0, screen->root,
                XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                (const char*)&event);
        }
        int failed = xcb_connection_has_error(connection);
        xcb_generic_error_t* error = xcb_request_check(connection, cookie);
        if (error) {
            fprintf(stderr, "vela-x11: %s failed (X11 error %d)\n", argv[1], error->error_code);
            free(error);
            failed = 1;
        }
        if (!failed && !strcmp(argv[1], "--override-redirect")) {
            // ChangeWindowAttributes doesn't itself send an X11 notification
            // to the window manager. wlroots learns the new override-redirect
            // value from ConfigureNotify or MapNotify. Exercise a *mapped*
            // transition by issuing a real ConfigureWindow, without an X11
            // unmap/remap or a fabricated event. Restore the original size.
            xcb_get_geometry_reply_t* geometry =
                xcb_get_geometry_reply(connection, xcb_get_geometry(connection, target), NULL);
            if (!geometry || !geometry->width || geometry->width == UINT16_MAX) {
                fprintf(stderr, "vela-x11: cannot configure target after override-redirect change\n");
                failed = 1;
            } else {
                const uint32_t widths[] = { (uint32_t)geometry->width + 1, geometry->width };
                for (size_t i = 0; i < 2; ++i) {
                    xcb_void_cookie_t configure = xcb_configure_window_checked(
                        connection, target, XCB_CONFIG_WINDOW_WIDTH, &widths[i]);
                    xcb_generic_error_t* configure_error = xcb_request_check(connection, configure);
                    if (configure_error) {
                        fprintf(stderr, "vela-x11: override-redirect configure failed (X11 error %d)\n",
                            configure_error->error_code);
                        free(configure_error);
                        failed = 1;
                        break;
                    }
                }
            }
            free(geometry);
        }
        xcb_disconnect(connection);
        return failed;
    }
    xcb_window_t window = create(connection, screen, 0, 0, width, height, 0xff8000, 0);
    if (idFile && write_xid_file(idFile, window) != 0) {
        xcb_disconnect(connection);
        return 1;
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
    xcb_window_t backdrop = XCB_NONE;
    if (wineBackdrop) {
        backdrop = create(connection, screen, 0, 0, screen->width_in_pixels, screen->height_in_pixels, 0x000000, 1);
        if (backdropIdFile && write_xid_file(backdropIdFile, backdrop) != 0) {
            xcb_disconnect(connection);
            return 1;
        }
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, backdrop, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
            sizeof(wmClass), wmClass);
        xcb_map_window(connection, backdrop);
    }
    xcb_flush(connection);

    // One graphics context per color, made on first use.
    xcb_gcontext_t orange = XCB_NONE;
    xcb_gcontext_t green = XCB_NONE;
    xcb_generic_event_t* event;
    while ((event = xcb_wait_for_event(connection))) {
        int type = event->response_type & 0x7f;
        if (type == XCB_EXPOSE && backdrop != XCB_NONE
            && ((const xcb_expose_event_t*)event)->window == backdrop) {
            // Black is its background. Shown as an override-redirect window
            // once: give the compositor time to show it, then make it managed.
            if (wineBackdrop) {
                wineBackdrop = 0;
                usleep(200000);
                wine_backdrop_managed(connection, screen, window, backdrop);
            }
        } else if (type == XCB_EXPOSE) {
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
