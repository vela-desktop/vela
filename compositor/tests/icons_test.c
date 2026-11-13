// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The title bars' icons under a client that keeps changing its app_id
// (docs/memory-allocation-policy.md, "Aggregate growth"), through
// vela_icons_app as decoration.c calls it, on an icon theme written here:
// the cache stays within VELA_ICON_CACHE_BUDGET; an icon handed out stays
// valid after its entry is evicted, and is drawn again when asked for; a
// recently used entry survives and the oldest goes; a miss is remembered;
// an app_id too long to have an icon is never remembered.
//
// Without librsvg nothing is ever drawn: the cases that need an icon found
// are skipped, the others run on misses alone.

#include "icon_cache.h"
#include "icons.h"

#include <ftw.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// CTest builds with NDEBUG in RelWithDebInfo, so assert() would skip checks.
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define BUDGET VELA_ICON_CACHE_BUDGET

// Premultiplied ARGB8888 of the opaque colors the icons are filled with.
#define RED 0xffff0000u
#define GREEN 0xff00ff00u
#define BLUE 0xff0000ffu
#define YELLOW 0xffffff00u

static char root[PATH_MAX];
static char apps_dir[PATH_MAX]; // the theme's directory of app icons

static void make_dir(const char *relative)
{
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/%s", root, relative) < (int)sizeof(path));
    CHECK(mkdir(path, 0700) == 0);
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "we");
    CHECK(file);
    CHECK(fputs(text, file) >= 0);
    CHECK(fclose(file) == 0);
}

// The theme's icon of `app_id`: a square filled with `color`.
static void write_icon(const char *app_id, uint32_t color)
{
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/%s.svg", apps_dir, app_id) < (int)sizeof(path));
    char svg[256];
    snprintf(svg, sizeof(svg),
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
        "<rect width=\"16\" height=\"16\" fill=\"#%06x\"/></svg>\n",
        (unsigned)(color & 0xffffffu));
    write_text(path, svg);
}

// Whether the image is size x size with every pixel `color`. It reads every
// pixel, so under ASan a freed image fails here.
static bool filled(const struct vela_image *image, int size, uint32_t color)
{
    if (!image->pixels || image->width != size || image->height != size) {
        return false;
    }
    for (int i = 0; i < size * size; ++i) {
        if (image->pixels[i] != color) {
            return false;
        }
    }
    return true;
}

// `count` requests for app_ids never asked before and without an icon: each
// adds a miss to the cache.
static void misses(struct vela_icons *icons, int count)
{
    static int next;
    for (int i = 0; i < count; ++i) {
        char app_id[64];
        snprintf(app_id, sizeof(app_id), "vela-icons-test.unknown%d", next++);
        struct vela_image image;
        CHECK(!vela_icons_app(icons, app_id, 16, &image));
        CHECK(!image.pixels && image.width == 0 && image.height == 0);
        CHECK(vela_icons_cached(icons) <= BUDGET);
    }
}

// A client cycling through app_ids, on outputs of several scales, some with
// an icon and most without: the cache never passes its budget.
static void test_budget(void)
{
    struct vela_icons *icons = vela_icons_create();
    CHECK(vela_icons_cached(icons) == 0);
    static const int sizes[] = { 16, 20, 24, 32 };
    int found = 0;
    for (int i = 0; i < 3 * BUDGET; ++i) {
        char app_id[64];
        snprintf(app_id, sizeof(app_id), "vela-icons-test.cycle%d", i / 2);
        bool has_file = (i / 2) % 4 == 0;
        if (has_file && i % 2 == 0) {
            write_icon(app_id, RED);
        }
        int size = sizes[i % 4];
        struct vela_image image;
        bool ok = vela_icons_app(icons, app_id, size, &image);
#if VELA_HAVE_RSVG
        CHECK(ok == has_file);
#else
        CHECK(!ok);
#endif
        if (ok) {
            CHECK(filled(&image, size, RED));
            ++found;
        } else {
            CHECK(!image.pixels);
        }
        vela_image_finish(&image);
        int expected = i + 1 < BUDGET ? i + 1 : BUDGET;
        CHECK(vela_icons_cached(icons) == expected);
    }
#if VELA_HAVE_RSVG
    CHECK(found == 3 * BUDGET / 4);
#endif
    vela_icons_destroy(icons);
}

// A miss is remembered (the theme is not searched again), counts toward the
// budget, and is evicted like any entry.
static void test_miss_is_remembered(void)
{
    struct vela_icons *icons = vela_icons_create();
    struct vela_image image;
    CHECK(!vela_icons_app(icons, "vela-icons-test.late", 16, &image));
    CHECK(vela_icons_cached(icons) == 1);
    // The icon appears on disk, but the answer is the remembered one.
    write_icon("vela-icons-test.late", GREEN);
    CHECK(!vela_icons_app(icons, "vela-icons-test.late", 16, &image));
    CHECK(vela_icons_cached(icons) == 1);
    // Another size is another entry.
    bool other_size = vela_icons_app(icons, "vela-icons-test.late", 32, &image);
    CHECK(vela_icons_cached(icons) == 2);
    vela_image_finish(&image);
    // Once the miss is evicted, the theme is searched again.
    misses(icons, BUDGET);
    CHECK(vela_icons_cached(icons) == BUDGET);
    bool again = vela_icons_app(icons, "vela-icons-test.late", 16, &image);
#if VELA_HAVE_RSVG
    CHECK(other_size);
    CHECK(again && filled(&image, 16, GREEN));
#else
    CHECK(!other_size && !again);
#endif
    vela_image_finish(&image);
    vela_icons_destroy(icons);
}

// An app_id longer than any file name has no icon and takes no entry; the
// longest one that can have an icon takes one.
static void test_long_app_id(void)
{
    struct vela_icons *icons = vela_icons_create();
    char app_id[VELA_ICON_APP_ID_MAX + 2];
    memset(app_id, 'a', sizeof(app_id));
    app_id[VELA_ICON_APP_ID_MAX + 1] = '\0';
    struct vela_image image;
    for (int i = 0; i < 2 * BUDGET; ++i) {
        // Distinct every time.
        snprintf(app_id, 16, "%015d", i);
        app_id[15] = 'a';
        CHECK(strlen(app_id) == VELA_ICON_APP_ID_MAX + 1);
        CHECK(!vela_icons_app(icons, app_id, 16, &image));
        CHECK(!image.pixels);
        CHECK(vela_icons_cached(icons) == 0);
    }
    app_id[VELA_ICON_APP_ID_MAX] = '\0';
    CHECK(!vela_icons_app(icons, app_id, 16, &image));
    CHECK(vela_icons_cached(icons) == 1);
    vela_icons_destroy(icons);
}

#if VELA_HAVE_RSVG
// A title bar holds its icon while the entry it came from is evicted: the
// held image is untouched, and the same request draws the icon again.
static void test_held_icon_outlives_eviction(void)
{
    struct vela_icons *icons = vela_icons_create();
    write_icon("vela-icons-test.held", RED);
    struct vela_image held;
    CHECK(vela_icons_app(icons, "vela-icons-test.held", 32, &held));
    CHECK(filled(&held, 32, RED));
    // The file changes, so that a new drawing can be told from the entry.
    write_icon("vela-icons-test.held", BLUE);
    struct vela_image second;
    CHECK(vela_icons_app(icons, "vela-icons-test.held", 32, &second));
    CHECK(filled(&second, 32, RED)); // still the entry
    CHECK(second.pixels != held.pixels); // each caller has its own copy

    misses(icons, BUDGET); // the entry is evicted
    CHECK(vela_icons_cached(icons) == BUDGET);
    CHECK(filled(&held, 32, RED));
    CHECK(filled(&second, 32, RED));

    // Asked again, the evicted icon is searched and drawn again.
    struct vela_image again;
    CHECK(vela_icons_app(icons, "vela-icons-test.held", 32, &again));
    CHECK(filled(&again, 32, BLUE));
    CHECK(vela_icons_cached(icons) == BUDGET);
    CHECK(filled(&held, 32, RED));

    // The images outlive the loader too.
    vela_icons_destroy(icons);
    CHECK(filled(&held, 32, RED));
    CHECK(filled(&second, 32, RED));
    CHECK(filled(&again, 32, BLUE));
    vela_image_finish(&held);
    vela_image_finish(&second);
    vela_image_finish(&again);
}

// The least recently used entry is the one evicted: an icon asked for again
// stays, an icon not asked for since goes.
static void test_recently_used_survives(void)
{
    struct vela_icons *icons = vela_icons_create();
    write_icon("vela-icons-test.old", YELLOW);
    write_icon("vela-icons-test.kept", GREEN);
    struct vela_image image;
    CHECK(vela_icons_app(icons, "vela-icons-test.old", 16, &image));
    CHECK(filled(&image, 16, YELLOW));
    vela_image_finish(&image);
    CHECK(vela_icons_app(icons, "vela-icons-test.kept", 16, &image));
    CHECK(filled(&image, 16, GREEN));
    vela_image_finish(&image);
    // From here a blue icon means "drawn again": the entry had been evicted.
    write_icon("vela-icons-test.old", BLUE);
    write_icon("vela-icons-test.kept", BLUE);

    misses(icons, BUDGET - 2); // full: old, kept, the misses
    CHECK(vela_icons_cached(icons) == BUDGET);
    CHECK(vela_icons_app(icons, "vela-icons-test.kept", 16, &image));
    CHECK(filled(&image, 16, GREEN)); // used: now the most recent
    vela_image_finish(&image);

    misses(icons, 1); // evicts the least recently used: old
    CHECK(vela_icons_app(icons, "vela-icons-test.old", 16, &image));
    CHECK(filled(&image, 16, BLUE));
    vela_image_finish(&image);
    CHECK(vela_icons_app(icons, "vela-icons-test.kept", 16, &image));
    CHECK(filled(&image, 16, GREEN)); // older than every miss, yet kept
    vela_image_finish(&image);

    // Not used while a whole budget of other entries arrives: evicted.
    misses(icons, BUDGET);
    CHECK(vela_icons_app(icons, "vela-icons-test.kept", 16, &image));
    CHECK(filled(&image, 16, BLUE));
    vela_image_finish(&image);
    CHECK(vela_icons_cached(icons) == BUDGET);
    vela_icons_destroy(icons);
}
#endif

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)walk;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

int main(void)
{
    // A private directory under the test's own working directory (the build
    // tree), never in a publicly writable one. Every place icons.c reads is
    // inside it: the user's and the system's themes and .desktop files, and
    // kdeglobals.
    char cwd[PATH_MAX - 64];
    CHECK(getcwd(cwd, sizeof(cwd)));
    snprintf(root, sizeof(root), "%s/vela-icons-XXXXXX", cwd);
    CHECK(mkdtemp(root));
    char path[PATH_MAX];
    CHECK(setenv("HOME", root, 1) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/data", root) < (int)sizeof(path));
    CHECK(setenv("XDG_DATA_HOME", path, 1) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/system", root) < (int)sizeof(path));
    CHECK(setenv("XDG_DATA_DIRS", path, 1) == 0);
    CHECK(snprintf(path, sizeof(path), "%s/config", root) < (int)sizeof(path));
    CHECK(setenv("XDG_CONFIG_HOME", path, 1) == 0);
    make_dir("config");
    make_dir("system");
    make_dir("data");
    make_dir("data/icons");
    make_dir("data/icons/hicolor");
    make_dir("data/icons/hicolor/scalable");
    make_dir("data/icons/hicolor/scalable/apps");
    CHECK(snprintf(apps_dir, sizeof(apps_dir), "%s/data/icons/hicolor/scalable/apps", root) < (int)sizeof(apps_dir));
    CHECK(snprintf(path, sizeof(path), "%s/data/icons/hicolor/index.theme", root) < (int)sizeof(path));
    write_text(path, "[Icon Theme]\nName=Hicolor\nDirectories=scalable/apps\n\n"
                     "[scalable/apps]\nSize=48\nType=Scalable\n");

    test_budget();
    test_miss_is_remembered();
    test_long_app_id();
#if VELA_HAVE_RSVG
    test_held_icon_outlives_eviction();
    test_recently_used_survives();
#endif

    CHECK(nftw(root, remove_entry, 8, FTW_DEPTH | FTW_PHYS) == 0);
#if VELA_HAVE_RSVG
    puts("icons passed");
#else
    puts("icons passed (without librsvg: the cases that need an icon found were skipped)");
#endif
    return 0;
}
