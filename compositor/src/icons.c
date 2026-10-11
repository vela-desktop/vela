// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "icons.h"

#include "util.h"
#include "path_security.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

#if VELA_HAVE_RSVG
#include <cairo.h>
#include <librsvg/rsvg.h>
#endif

// A list of allocated strings.
struct strings {
    char **items;
    int count;
    int capacity;
};

static void strings_push(struct strings *list, const char *text)
{
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->items = realloc(list->items, (size_t)list->capacity * sizeof(*list->items));
    }
    list->items[list->count++] = strdup(text);
}

static bool strings_contain(const struct strings *list, const char *text)
{
    for (int i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i], text) == 0) {
            return true;
        }
    }
    return false;
}

static void strings_free(struct strings *list)
{
    for (int i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

// The non-empty entries of "a,b,c" (or with another separator).
static void strings_split(struct strings *list, const char *text, char separator)
{
    while (*text) {
        const char *end = strchr(text, separator);
        size_t length = end ? (size_t)(end - text) : strlen(text);
        if (length > 0) {
            char *item = strndup(text, length);
            strings_push(list, item);
            free(item);
        }
        text += length + (end ? 1 : 0);
    }
}

// A name -> icon mapping, from .desktop files.
struct icon_name {
    char *key; // lowercase
    char *icon;
};

struct icon_names {
    struct icon_name *items;
    int count;
    int capacity;
};

static const char *icon_names_find(const struct icon_names *names, const char *key)
{
    for (int i = 0; i < names->count; ++i) {
        if (strcmp(names->items[i].key, key) == 0) {
            return names->items[i].icon;
        }
    }
    return NULL;
}

// The first one found wins (directories go from the most important).
static void icon_names_add(struct icon_names *names, const char *key, const char *icon)
{
    if (icon_names_find(names, key)) {
        return;
    }
    if (names->count == names->capacity) {
        names->capacity = names->capacity ? names->capacity * 2 : 128;
        names->items = realloc(names->items, (size_t)names->capacity * sizeof(*names->items));
    }
    names->items[names->count].key = strdup(key);
    names->items[names->count].icon = strdup(icon);
    ++names->count;
}

static void icon_names_free(struct icon_names *names)
{
    for (int i = 0; i < names->count; ++i) {
        free(names->items[i].key);
        free(names->items[i].icon);
    }
    free(names->items);
}

// An icon already drawn (or looked up without success), per app and size.
struct cached_icon {
    char *app_id;
    int size;
    struct vela_image image;
    struct cached_icon *next;
};

struct vela_icons {
    struct strings theme_chain; // the theme, those it inherits, hicolor
    struct strings icon_dirs; // .../icons
    bool desktop_loaded;
    struct icon_names by_id; // .desktop id (without extension) -> Icon
    struct icon_names by_class; // StartupWMClass -> Icon
    struct cached_icon *cache;
};

static void lowercase(char *text)
{
    for (; *text; ++text) {
        *text = (char)tolower((unsigned char)*text);
    }
}

static bool exists(const char *path)
{
    return access(path, F_OK) == 0;
}

// The XDG data directories, most important first.
static void data_dirs(struct strings *dirs)
{
    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    char path[PATH_MAX];
    if (data_home && *data_home) {
        strings_push(dirs, data_home);
    } else if (home) {
        snprintf(path, sizeof(path), "%s/.local/share", home);
        strings_push(dirs, path);
    }
    const char *system = getenv("XDG_DATA_DIRS");
    strings_split(dirs, system && *system ? system : "/usr/local/share:/usr/share", ':');
}

// The values of some keys in a [section] of an .ini or .desktop file (for
// repeated keys the last line wins). values[i] stays empty if the key is
// missing.
static void read_section(const char *path, const char *section, const char *const *keys, char (*values)[256],
    int count)
{
    for (int i = 0; i < count; ++i) {
        values[i][0] = '\0';
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }
    size_t section_length = strlen(section);
    bool inside = false;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n;
    while ((n = getline(&line, &capacity, file)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') {
            line[--n] = '\0';
        }
        if (line[0] == '[') {
            inside = (size_t)n == section_length + 2 && strncmp(line + 1, section, section_length) == 0
                && line[n - 1] == ']';
            continue;
        }
        char *eq = inside ? strchr(line, '=') : NULL;
        if (!eq) {
            continue;
        }
        *eq = '\0';
        for (int i = 0; i < count; ++i) {
            if (strcmp(line, keys[i]) == 0) {
                snprintf(values[i], sizeof(values[i]), "%s", eq + 1);
            }
        }
    }
    free(line);
    fclose(file);
}

static void kdeglobals_path(char *out, size_t size)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (config && *config) {
        snprintf(out, size, "%s/kdeglobals", config);
    } else if (home) {
        snprintf(out, size, "%s/.config/kdeglobals", home);
    } else {
        out[0] = '\0';
    }
}

struct vela_icons *vela_icons_create(void)
{
    struct vela_icons *icons = calloc(1, sizeof(*icons));
    struct strings dirs = { 0 };
    data_dirs(&dirs);
    char path[PATH_MAX];
    for (int i = 0; i < dirs.count; ++i) {
        snprintf(path, sizeof(path), "%s/icons", dirs.items[i]);
        strings_push(&icons->icon_dirs, path);
    }
    strings_free(&dirs);

    // The theme chosen in KDE ([Icons] Theme in kdeglobals), otherwise Breeze.
    static const char *const theme_key[] = { "Theme" };
    char theme[1][256];
    kdeglobals_path(path, sizeof(path));
    read_section(path, "Icons", theme_key, theme, 1);
    // The chain: the theme, those it inherits (Inherits), finally hicolor.
    struct strings pending = { 0 };
    strings_push(&pending, vela_path_component_safe(theme[0]) ? theme[0] : "breeze");
    for (int next = 0; next < pending.count; ++next) {
        const char *name = pending.items[next];
        // A theme name is exactly one directory component. Inherited
        // names from index.theme must not escape the selected icon roots.
        if (!vela_path_component_safe(name)
            || strings_contain(&icons->theme_chain, name)) {
            continue;
        }
        strings_push(&icons->theme_chain, name);
        for (int i = 0; i < icons->icon_dirs.count; ++i) {
            snprintf(path, sizeof(path), "%s/%s/index.theme", icons->icon_dirs.items[i], name);
            if (exists(path)) {
                static const char *const inherits_key[] = { "Inherits" };
                char inherits[1][256];
                read_section(path, "Icon Theme", inherits_key, inherits, 1);
                strings_split(&pending, inherits[0], ',');
                break;
            }
        }
    }
    strings_free(&pending);
    if (!strings_contain(&icons->theme_chain, "hicolor")) {
        strings_push(&icons->theme_chain, "hicolor");
    }
    return icons;
}

void vela_icons_destroy(struct vela_icons *icons)
{
    if (!icons) {
        return;
    }
    strings_free(&icons->theme_chain);
    strings_free(&icons->icon_dirs);
    icon_names_free(&icons->by_id);
    icon_names_free(&icons->by_class);
    struct cached_icon *entry = icons->cache;
    while (entry) {
        struct cached_icon *next = entry->next;
        free(entry->app_id);
        vela_image_finish(&entry->image);
        free(entry);
        entry = next;
    }
    free(icons);
}

// The .desktop files under `dir` (relative: the part of the id after `apps`),
// recursively. Linked directories aren't followed.
static void scan_applications(struct vela_icons *icons, const char *apps, const char *relative)
{
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s%s%s", apps, *relative ? "/" : "", relative);
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        char path[PATH_MAX];
        struct stat link_info;
        if (!vela_format(child, sizeof(child), "%s%s%s", relative, *relative ? "/" : "", entry->d_name)
            || !vela_format(path, sizeof(path), "%s/%s", apps, child) || lstat(path, &link_info) != 0) {
            continue;
        }
        if (S_ISDIR(link_info.st_mode)) {
            scan_applications(icons, apps, child);
            continue;
        }
        struct stat info;
        size_t length = strlen(child);
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || length <= 8
            || strcmp(child + length - 8, ".desktop") != 0) {
            continue;
        }
        static const char *const keys[] = { "Icon", "StartupWMClass" };
        char values[2][256];
        read_section(path, "Desktop Entry", keys, values, 2);
        if (!values[0][0]) {
            continue;
        }
        // The id with subdirectories joined by '-' (Desktop Entry spec).
        child[length - 8] = '\0';
        for (char *c = child; *c; ++c) {
            if (*c == '/') {
                *c = '-';
            }
        }
        lowercase(child);
        icon_names_add(&icons->by_id, child, values[0]);
        if (values[1][0]) {
            lowercase(values[1]);
            icon_names_add(&icons->by_class, values[1], values[0]);
        }
    }
    closedir(dir);
}

static void load_desktop_entries(struct vela_icons *icons)
{
    icons->desktop_loaded = true;
    struct strings dirs = { 0 };
    data_dirs(&dirs);
    for (int i = 0; i < dirs.count; ++i) {
        char apps[PATH_MAX];
        snprintf(apps, sizeof(apps), "%s/applications", dirs.items[i]);
        scan_applications(icons, apps, "");
    }
    strings_free(&dirs);
}

// The app's icon name. From the safest rule: the .desktop name is the app_id,
// then the X11 class, then the last part of a reverse-domain id (the first in
// alphabetical order); finally the app_id itself, because the theme often has
// an icon named after the app.
static const char *icon_name(struct vela_icons *icons, const char *app_id)
{
    if (!icons->desktop_loaded) {
        load_desktop_entries(icons);
    }
    char id[256];
    snprintf(id, sizeof(id), "%s", app_id);
    lowercase(id);
    const char *icon = icon_names_find(&icons->by_id, id);
    if (!icon) {
        icon = icon_names_find(&icons->by_class, id);
    }
    if (icon) {
        return icon;
    }
    const char *best_id = NULL;
    for (int i = 0; i < icons->by_id.count; ++i) {
        const char *desktop_id = icons->by_id.items[i].key;
        const char *dot = strrchr(desktop_id, '.');
        if (dot && strcmp(dot + 1, id) == 0 && (!best_id || strcmp(desktop_id, best_id) < 0)) {
            best_id = desktop_id;
            icon = icons->by_id.items[i].icon;
        }
    }
    return icon ? icon : app_id;
}

// The directories of an icon theme (index.theme): name, size, whether
// scalable. Directories says which ones count, each [directory] its sizes.
struct theme_dir {
    char name[256];
    int size;
    bool scalable;
};

static int theme_dirs(const char *index, struct theme_dir **out)
{
    *out = NULL;
    FILE *file = fopen(index, "r");
    if (!file) {
        return 0;
    }
    struct strings listed = { 0 };
    struct theme_dir *dirs = NULL; // all sections, then only the listed ones
    int count = 0;
    int capacity = 0;
    int current = -1; // the current section; -1: [Icon Theme] or none
    bool in_theme = false;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t n;
    while ((n = getline(&line, &line_capacity, file)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') {
            line[--n] = '\0';
        }
        if (line[0] == '[') {
            in_theme = strcmp(line, "[Icon Theme]") == 0;
            current = -1;
            if (!in_theme && n >= 2 && line[n - 1] == ']') {
                line[n - 1] = '\0';
                for (int i = 0; i < count; ++i) {
                    if (strcmp(dirs[i].name, line + 1) == 0) {
                        current = i;
                    }
                }
                if (current < 0) {
                    if (count == capacity) {
                        capacity = capacity ? capacity * 2 : 64;
                        dirs = realloc(dirs, (size_t)capacity * sizeof(*dirs));
                    }
                    current = count++;
                    snprintf(dirs[current].name, sizeof(dirs[current].name), "%s", line + 1);
                    dirs[current].size = 0;
                    dirs[current].scalable = false;
                }
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *value = eq + 1;
        if (in_theme && strcmp(line, "Directories") == 0) {
            strings_free(&listed);
            strings_split(&listed, value, ',');
        } else if (current >= 0 && strcmp(line, "Size") == 0) {
            dirs[current].size = atoi(value);
        } else if (current >= 0 && strcmp(line, "Type") == 0) {
            dirs[current].scalable = strcmp(value, "Scalable") == 0;
        }
    }
    free(line);
    fclose(file);

    // In the order of Directories; a directory without a section counts as 0,
    // Threshold.
    struct theme_dir *result = calloc((size_t)(listed.count ? listed.count : 1), sizeof(*result));
    for (int i = 0; i < listed.count; ++i) {
        snprintf(result[i].name, sizeof(result[i].name), "%s", listed.items[i]);
        for (int d = 0; d < count; ++d) {
            if (strcmp(dirs[d].name, listed.items[i]) == 0) {
                result[i].size = dirs[d].size;
                result[i].scalable = dirs[d].scalable;
            }
        }
    }
    int listed_count = listed.count;
    strings_free(&listed);
    free(dirs);
    *out = result;
    return listed_count;
}

// The file of icon `name` for `size` pixels, in `out`; false if there is none.
// In the theme and those it inherits: the directory with the closest size; on
// a tie, the SVG (drawn exactly at any size).
static bool find_file(struct vela_icons *icons, const char *name, int size, char *out, size_t out_size)
{
    if (!*name) {
        return false;
    }
    if (name[0] == '/') {
        // Icon=/absolute/path is explicitly supported by .desktop files.
        snprintf(out, out_size, "%s", name);
        return exists(name);
    }
    if (!vela_relative_path_safe(name)) {
        return false;
    }
    char path[PATH_MAX];
    for (int t = 0; t < icons->theme_chain.count; ++t) {
        const char *theme = icons->theme_chain.items[t];
        bool found = false;
        int best_score = INT_MAX;
        for (int b = 0; b < icons->icon_dirs.count; ++b) {
            char root[PATH_MAX];
            if (!vela_format(root, sizeof(root), "%s/%s", icons->icon_dirs.items[b], theme)
                || !vela_format(path, sizeof(path), "%s/index.theme", root) || !exists(path)) {
                continue;
            }
            struct theme_dir *dirs = NULL;
            int count = theme_dirs(path, &dirs);
            for (int d = 0; d < count; ++d) {
                // Theme subdirectories such as "48x48/apps" are valid;
                // "." and ".." components are not.
                if (!vela_relative_path_safe(dirs[d].name)) {
                    continue;
                }
                static const char *const extensions[] = { ".svg", ".png" };
                for (int e = 0; e < 2; ++e) {
                    if (!vela_format(path, sizeof(path), "%s/%s/%s%s", root, dirs[d].name, name, extensions[e])
                        || !exists(path)) {
                        continue;
                    }
                    int score = dirs[d].scalable ? 0 : abs(dirs[d].size - size) * 2;
                    if (dirs[d].size < size && !dirs[d].scalable) {
                        score += 1000; // enlarging a small PNG: last choice
                    }
                    score = e == 0 ? (score > 0 ? score - 1 : 0) : score + 1;
                    if (score < best_score) {
                        best_score = score;
                        snprintf(out, out_size, "%s", path);
                        found = true;
                    }
                }
            }
            free(dirs);
        }
        if (found) {
            return true;
        }
    }
    static const char *const pixmaps[] = { ".svg", ".png" };
    for (int e = 0; e < 2; ++e) {
        snprintf(path, sizeof(path), "/usr/share/pixmaps/%s%s", name, pixmaps[e]);
        if (exists(path)) {
            snprintf(out, out_size, "%s", path);
            return true;
        }
    }
    return false;
}

static void render(const char *path, int size, struct vela_image *image)
{
#if VELA_HAVE_RSVG
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    bool ok = false;
    size_t length = strlen(path);
    if (length > 4 && strcmp(path + length - 4, ".svg") == 0) {
        GError *error = NULL;
        RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
        if (handle) {
            RsvgRectangle viewport = { 0, 0, (double)size, (double)size };
            ok = rsvg_handle_render_document(handle, cr, &viewport, &error);
            g_object_unref(handle);
        }
        if (error) {
            g_error_free(error);
        }
    } else {
        cairo_surface_t *png = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS) {
            int w = cairo_image_surface_get_width(png);
            int h = cairo_image_surface_get_height(png);
            double scale = (double)size / (w > h ? w : h);
            cairo_translate(cr, (size - w * scale) / 2.0, (size - h * scale) / 2.0);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, png, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
            cairo_paint(cr);
            ok = true;
        }
        cairo_surface_destroy(png);
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    if (ok) {
        // Cairo ARGB32 is already premultiplied ARGB8888, row by row.
        int stride = cairo_image_surface_get_stride(surface);
        const unsigned char *data = cairo_image_surface_get_data(surface);
        image->width = size;
        image->height = size;
        image->pixels = malloc((size_t)size * (size_t)size * 4);
        for (int y = 0; y < size; ++y) {
            memcpy(&image->pixels[(size_t)y * (size_t)size], data + (size_t)y * (size_t)stride, (size_t)size * 4);
        }
    }
    cairo_surface_destroy(surface);
#else
    (void)path;
    (void)size;
    (void)image;
#endif
}

const struct vela_image *vela_icons_app(struct vela_icons *icons, const char *app_id, int size)
{
    for (struct cached_icon *entry = icons->cache; entry; entry = entry->next) {
        if (entry->size == size && strcmp(entry->app_id, app_id) == 0) {
            return &entry->image;
        }
    }
    struct cached_icon *entry = calloc(1, sizeof(*entry));
    entry->app_id = strdup(app_id);
    entry->size = size;
    char path[PATH_MAX];
    if (find_file(icons, icon_name(icons, app_id), size, path, sizeof(path))) {
        render(path, size, &entry->image);
    }
    if (!entry->image.pixels) {
        wlr_log(WLR_DEBUG, "Icons: no icon for %s", app_id);
    }
    entry->next = icons->cache;
    icons->cache = entry;
    return &entry->image;
}
