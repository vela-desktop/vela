// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-x11-present: an X11 window that shows its frames with Present, like
// games do, for the Xwayland flip test.
//
// Usage: vela-x11-present [WIDTH HEIGHT]
//
// Presents an orange pixmap until Xwayland flips it and prints
// "orange MODE" (the Present completion mode, 1 = flip). After a line on
// stdin it presents a green pixmap with an empty update region and prints
// "green MODE": a flip shows the whole new pixmap, damaged or not.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xcb/present.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

static xcb_pixmap_t filled(xcb_connection_t* connection, xcb_screen_t* screen, xcb_window_t window, int width,
    int height, uint32_t color)
{
    xcb_pixmap_t pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(connection, screen->root_depth, pixmap, window, width, height);
    xcb_gcontext_t gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, pixmap, XCB_GC_FOREGROUND, &color);
    const xcb_rectangle_t area = { 0, 0, width, height };
    xcb_poly_fill_rectangle(connection, pixmap, gc, 1, &area);
    xcb_free_gc(connection, gc);
    return pixmap;
}

// Presents the pixmap and returns how it was shown.
static int present(xcb_connection_t* connection, uint8_t opcode, xcb_window_t window, xcb_pixmap_t pixmap,
    xcb_xfixes_region_t update)
{
    static uint32_t serial;
    xcb_present_pixmap(connection, window, pixmap, ++serial, XCB_NONE, update, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
        0, 0, 0, 0, 0, NULL);
    xcb_flush(connection);
    xcb_generic_event_t* event;
    while ((event = xcb_wait_for_event(connection))) {
        const xcb_present_complete_notify_event_t* notify = (const xcb_present_complete_notify_event_t*)event;
        if ((event->response_type & 0x7f) == XCB_GE_GENERIC && notify->extension == opcode
            && notify->event_type == XCB_PRESENT_EVENT_COMPLETE_NOTIFY
            && notify->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP && notify->serial == serial) {
            int mode = notify->mode;
            free(event);
            return mode;
        }
        free(event);
    }
    return -1;
}

int main(int argc, char** argv)
{
    int width = argc == 3 ? atoi(argv[1]) : 400;
    int height = argc == 3 ? atoi(argv[2]) : 300;
    xcb_connection_t* connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(connection)) {
        fprintf(stderr, "vela-x11-present: no X display (DISPLAY)\n");
        return 1;
    }
    xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    free(xcb_present_query_version_reply(connection, xcb_present_query_version(connection, 1, 2), NULL));
    free(xcb_xfixes_query_version_reply(connection, xcb_xfixes_query_version(connection, 5, 0), NULL));
    const xcb_query_extension_reply_t* extension = xcb_get_extension_data(connection, &xcb_present_id);
    if (!extension || !extension->present) {
        fprintf(stderr, "vela-x11-present: no Present extension\n");
        return 1;
    }

    xcb_window_t window = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, width, height, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
    const char title[] = "vela-x11-present";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
        sizeof(title) - 1, title);
    xcb_present_select_input(connection, xcb_generate_id(connection), window,
        XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    xcb_map_window(connection, window);
    xcb_pixmap_t orange = filled(connection, screen, window, width, height, 0xff8000);
    xcb_pixmap_t green = filled(connection, screen, window, width, height, 0x00c000);
    xcb_xfixes_region_t empty = xcb_generate_id(connection);
    xcb_xfixes_create_region(connection, empty, 0, NULL);

    // The first frames are copied while Xwayland sets the window up.
    int mode = -1;
    for (int i = 0; i < 50 && mode != XCB_PRESENT_COMPLETE_MODE_FLIP; i++) {
        mode = present(connection, extension->major_opcode, window, orange, XCB_NONE);
        usleep(100000);
    }
    printf("orange %d\n", mode);
    fflush(stdout);
    char line[16];
    if (!fgets(line, sizeof line, stdin)) {
        return 0;
    }
    printf("green %d\n", present(connection, extension->major_opcode, window, green, empty));
    fflush(stdout);
    // Stay until the test ends.
    while (fgets(line, sizeof line, stdin)) {
    }
    xcb_disconnect(connection);
    return 0;
}
