// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "terminal.h"

#include "alloc/alloc.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// ----------------------------------------------------------- string lists --

struct list {
    char **items; // NULL-terminated once anything is in it
    int capacity;
    int count;
};

static void list_add(struct list *list, char *item)
{
    list->items = vela_grow(list->items, &list->capacity, (long long)list->count + 2, sizeof(*list->items));
    list->items[list->count++] = item;
    list->items[list->count] = NULL;
}

// The items, NULL-terminated even when empty.
static char **list_finish(struct list *list)
{
    return list->items ? list->items : VELA_NEW_N(char *, 1);
}

void vela_terminal_strv_free(char **strv)
{
    for (char **s = strv; s && *s; ++s) {
        free(*s);
    }
    free(strv);
}

// A growing string, for the words of Exec.
struct text {
    char *chars;
    size_t capacity;
    size_t length;
};

static void text_add(struct text *text, char c)
{
    text->chars = vela_grow(text->chars, &text->capacity, text->length + 2, 1);
    text->chars[text->length++] = c;
    text->chars[text->length] = '\0';
}

// ------------------------------------------------------------ environment --

void vela_terminal_env_get(struct vela_terminal_env *env)
{
    env->home = getenv("HOME");
    env->path = getenv("PATH");
    env->vela_terminal = getenv("VELA_TERMINAL");
    env->config_home = getenv("XDG_CONFIG_HOME");
    env->data_home = getenv("XDG_DATA_HOME");
    env->data_dirs = getenv("XDG_DATA_DIRS");
    env->current_desktop = getenv("XDG_CURRENT_DESKTOP");
}

static bool set(const char *value)
{
    return value && *value;
}

// A base directory: the variable's, else `fallback` under HOME. Allocated,
// or NULL.
static char *home_dir(const struct vela_terminal_env *env, const char *variable, const char *fallback)
{
    if (set(variable)) {
        return vela_strdup(variable);
    }
    return set(env->home) ? vela_asprintf("%s/%s", env->home, fallback) : NULL;
}

char *vela_terminal_choice_path(const struct vela_terminal_env *env)
{
    char *config = home_dir(env, env->config_home, ".config");
    char *path = config ? vela_asprintf("%s/vela-xdg-terminals.list", config) : NULL;
    free(config);
    return path;
}

// The next ':'-separated item of `*list` into `item` (empty items skipped);
// false at the end or for an item too long.
static bool next_item(const char **list, char *item, size_t size)
{
    while (**list == ':') {
        ++*list;
    }
    if (!**list) {
        return false;
    }
    size_t length = strcspn(*list, ":");
    bool fits = length < size;
    if (fits) {
        memcpy(item, *list, length);
        item[length] = '\0';
    } else {
        item[0] = '\0';
    }
    *list += length;
    return true;
}

// ---------------------------------------------------------------- programs --

static bool executable(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

// `name`'s full path: as it is with a '/', else from `path` (PATH). NULL
// when it isn't there.
static char *find_program(const char *path, const char *name)
{
    if (!set(name)) {
        return NULL;
    }
    if (strchr(name, '/')) {
        return executable(name) ? vela_strdup(name) : NULL;
    }
    char dir[4096];
    for (const char *list = path ? path : ""; next_item(&list, dir, sizeof(dir));) {
        if (!dir[0]) {
            continue;
        }
        char *candidate = vela_asprintf("%s/%s", dir, name);
        if (executable(candidate)) {
            return candidate;
        }
        free(candidate);
    }
    return NULL;
}

// ------------------------------------------------------------------- lines --

enum { LINE_LENGTH = 4096 };

// The next line of `file`, without its end; a longer line than LINE_LENGTH
// is skipped.
static bool next_line(FILE *file, char line[LINE_LENGTH])
{
    while (fgets(line, LINE_LENGTH, file)) {
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') {
            line[--n] = '\0';
            if (n > 0 && line[n - 1] == '\r') {
                line[n - 1] = '\0';
            }
            return true;
        }
        if (feof(file)) {
            return true; // the last line, without its newline
        }
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n') { }
    }
    return false;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

// ---------------------------------------------------------- desktop entries --

// The keys of [Desktop Entry] that matter here.
struct entry {
    bool application; // Type=Application
    bool hidden; // Hidden=true
    bool terminal; // TerminalEmulator in Categories
    char *exec;
    char *try_exec;
    char *only_show_in;
    char *not_show_in;
    bool has_exec_arg; // X-TerminalArgExec is there, maybe empty
    char *exec_arg;
};

static void entry_free(struct entry *entry)
{
    free(entry->exec);
    free(entry->try_exec);
    free(entry->only_show_in);
    free(entry->not_show_in);
    free(entry->exec_arg);
}

static void replace(char **field, char *value)
{
    free(*field);
    *field = value;
}

// A string value's escapes: \s \n \t \r \\ (the others stay as they are,
// for Exec's own rules).
static char *unescape(const char *value)
{
    char *out = vela_strdup(value);
    char *to = out;
    for (const char *from = value; *from; ++from) {
        char replacement = '\0';
        if (*from == '\\') {
            switch (from[1]) {
            case 's': replacement = ' '; break;
            case 'n': replacement = '\n'; break;
            case 't': replacement = '\t'; break;
            case 'r': replacement = '\r'; break;
            case '\\': replacement = '\\'; break;
            default: break;
            }
        }
        if (replacement) {
            *to++ = replacement;
            ++from;
        } else {
            *to++ = *from;
        }
    }
    *to = '\0';
    return out;
}

// Whether `item` is one of the `separator`-separated items of `list`.
static bool has_item(const char *list, char separator, const char *item, bool any_case)
{
    size_t length = strlen(item);
    for (const char *at = list; at && *at;) {
        size_t n = strcspn(at, (char[]) { separator, '\0' });
        if (n == length && (any_case ? strncasecmp(at, item, n) : strncmp(at, item, n)) == 0) {
            return true;
        }
        at += n;
        at += *at == separator;
    }
    return false;
}

static bool read_entry(const char *path, struct entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    FILE *file = fopen(path, "re");
    if (!file) {
        return false;
    }
    char line[LINE_LENGTH];
    bool in_group = false;
    while (next_line(file, line)) {
        char *s = trim(line);
        if (!*s || *s == '#') {
            continue;
        }
        if (*s == '[') {
            if (in_group) {
                break; // [Desktop Entry] is over: actions follow
            }
            in_group = strcmp(s, "[Desktop Entry]") == 0;
            continue;
        }
        char *equals = strchr(s, '=');
        if (!in_group || !equals) {
            continue;
        }
        *equals = '\0';
        const char *key = trim(s);
        const char *value = trim(equals + 1);
        if (strcmp(key, "Type") == 0) {
            entry->application = strcmp(value, "Application") == 0;
        } else if (strcmp(key, "Hidden") == 0) {
            entry->hidden = strcmp(value, "true") == 0;
        } else if (strcmp(key, "Categories") == 0) {
            entry->terminal = has_item(value, ';', "TerminalEmulator", false);
        } else if (strcmp(key, "Exec") == 0) {
            replace(&entry->exec, unescape(value));
        } else if (strcmp(key, "TryExec") == 0) {
            replace(&entry->try_exec, unescape(value));
        } else if (strcmp(key, "OnlyShowIn") == 0) {
            replace(&entry->only_show_in, vela_strdup(value));
        } else if (strcmp(key, "NotShowIn") == 0) {
            replace(&entry->not_show_in, vela_strdup(value));
        } else if (strcmp(key, "X-TerminalArgExec") == 0) {
            entry->has_exec_arg = true;
            replace(&entry->exec_arg, unescape(value));
        }
    }
    fclose(file);
    return in_group || entry->exec;
}

// Exec's words, without field codes (%f, %U, %i...: nothing is opened);
// NULL for an unfinished quote or no words.
static char **exec_argv(const char *exec)
{
    struct list words = { 0 };
    struct text word = { 0 };
    bool in_word = false;
    bool quoted = false;
    for (const char *at = exec;; ++at) {
        char c = *at;
        if (quoted && c) {
            if (c == '"') {
                quoted = false;
                continue;
            }
            if (c == '\\' && at[1] && strchr("\"`$\\", at[1])) {
                c = *++at;
            }
            text_add(&word, c);
            continue;
        }
        if (!c || c == ' ' || c == '\t') {
            if (in_word) {
                list_add(&words, word.chars ? word.chars : vela_strdup(""));
                word = (struct text) { 0 };
                in_word = false;
            }
            if (!c) {
                break;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            in_word = true;
            continue;
        }
        if (c == '%' && at[1]) {
            ++at;
            if (*at == '%') {
                text_add(&word, '%');
                in_word = true;
            }
            continue;
        }
        text_add(&word, c);
        in_word = true;
    }
    if (quoted || words.count == 0) {
        free(word.chars);
        vela_terminal_strv_free(words.items);
        return NULL;
    }
    return list_finish(&words);
}

// Shown in this desktop: OnlyShowIn names one of XDG_CURRENT_DESKTOP's,
// NotShowIn none.
static bool shown_here(const struct vela_terminal_env *env, const struct entry *entry)
{
    bool listed_only = false;
    bool listed_not = false;
    char desktop[256];
    for (const char *list = env->current_desktop ? env->current_desktop : "";
         next_item(&list, desktop, sizeof(desktop));) {
        listed_only = listed_only || (desktop[0] && has_item(entry->only_show_in, ';', desktop, true));
        listed_not = listed_not || (desktop[0] && has_item(entry->not_show_in, ';', desktop, true));
    }
    return (!entry->only_show_in || listed_only) && !listed_not;
}

// The entry's command line, with argv[0] a full path, when it can start:
// an application, not hidden, its TryExec and Exec programs installed.
static char **runnable(const struct vela_terminal_env *env, const struct entry *entry)
{
    if (!entry->application || entry->hidden || !entry->exec) {
        return NULL;
    }
    if (set(entry->try_exec)) {
        char *found = find_program(env->path, entry->try_exec);
        if (!found) {
            return NULL;
        }
        free(found);
    }
    char **argv = exec_argv(entry->exec);
    char *program = argv ? find_program(env->path, argv[0]) : NULL;
    if (!program) {
        vela_terminal_strv_free(argv);
        return NULL;
    }
    free(argv[0]);
    argv[0] = program;
    return argv;
}

// Never picked by themselves: drop-down terminals and the entries that
// only work with something else, as xdg-terminal-exec's own list.
static bool excluded(const char *id)
{
    static const char *const ids[] = {
        "org.kde.yakuake.desktop",
        "yakuake.desktop",
        "guake.desktop",
        "org.guake.Guake.desktop",
        "tilda.desktop",
        "com.github.lanoxx.tilda.desktop",
        "kitty-open.desktop",
        "foot-server.desktop",
        "footclient.desktop",
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(*ids); ++i) {
        if (strcmp(id, ids[i]) == 0) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------- desktop IDs --

struct found {
    char *id;
    char *path;
};

struct found_list {
    struct found *items;
    int capacity;
    int count;
};

static bool found_has(const struct found_list *list, const char *id)
{
    for (int i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

// The desktop files under `dir`: a subdirectory's files have IDs prefixed
// with its name and '-'. The first of an ID wins.
static void scan(const char *dir, const char *prefix, int depth, struct found_list *out)
{
    DIR *handle = opendir(dir);
    if (!handle) {
        return;
    }
    struct dirent *item;
    while ((item = readdir(handle))) {
        if (item->d_name[0] == '.') {
            continue;
        }
        char *path = vela_asprintf("%s/%s", dir, item->d_name);
        struct stat st;
        size_t n = strlen(item->d_name);
        if (stat(path, &st) != 0) {
            free(path);
        } else if (S_ISDIR(st.st_mode)) {
            if (depth < 3) {
                char *inner = vela_asprintf("%s%s-", prefix, item->d_name);
                scan(path, inner, depth + 1, out);
                free(inner);
            }
            free(path);
        } else if (S_ISREG(st.st_mode) && n > 8 && strcmp(item->d_name + n - 8, ".desktop") == 0) {
            char *id = vela_asprintf("%s%s", prefix, item->d_name);
            if (found_has(out, id)) {
                free(id);
                free(path);
            } else {
                out->items = vela_grow(out->items, &out->capacity, (long long)out->count + 1, sizeof(*out->items));
                out->items[out->count++] = (struct found) { id, path };
            }
        } else {
            free(path);
        }
    }
    closedir(handle);
}

static int by_id(const void *a, const void *b)
{
    return strcmp(((const struct found *)a)->id, ((const struct found *)b)->id);
}

// Every desktop file of the applications directories, by ID.
static struct found_list desktop_files(const struct vela_terminal_env *env)
{
    struct found_list out = { 0 };
    char *data_home = home_dir(env, env->data_home, ".local/share");
    if (data_home) {
        char *dir = vela_asprintf("%s/applications", data_home);
        scan(dir, "", 0, &out);
        free(dir);
        free(data_home);
    }
    char base[4096];
    for (const char *list = set(env->data_dirs) ? env->data_dirs : "/usr/local/share:/usr/share";
         next_item(&list, base, sizeof(base));) {
        if (base[0]) {
            char *dir = vela_asprintf("%s/applications", base);
            scan(dir, "", 0, &out);
            free(dir);
        }
    }
    if (out.count > 1) {
        qsort(out.items, (size_t)out.count, sizeof(*out.items), by_id);
    }
    return out;
}

static void found_free(struct found_list *list)
{
    for (int i = 0; i < list->count; ++i) {
        free(list->items[i].id);
        free(list->items[i].path);
    }
    free(list->items);
}

// --------------------------------------------------------------- the steps --

// From a desktop file. `installed`: step 4's rules, a terminal shown here.
static bool from_entry(const struct vela_terminal_env *env, const struct found *file, bool installed,
    enum vela_terminal_source source, struct vela_terminal *terminal)
{
    struct entry entry;
    if (!read_entry(file->path, &entry) || (installed && (!entry.terminal || !shown_here(env, &entry)))) {
        entry_free(&entry);
        return false;
    }
    char **argv = runnable(env, &entry);
    if (!argv) {
        entry_free(&entry);
        return false;
    }
    terminal->source = source;
    terminal->id = vela_strdup(file->id);
    terminal->argv = argv;
    // X-TerminalArgExec, maybe empty (foot cmd), else the usual -e.
    terminal->exec_arg = entry.has_exec_arg ? entry.exec_arg : vela_strdup("-e");
    entry.exec_arg = NULL;
    entry_free(&entry);
    return true;
}

// Step 1: the first entry of vela-xdg-terminals.list ("ID" or
// "ID:action"; comments and xdg-terminal-exec's directives skipped).
static bool chosen(const struct vela_terminal_env *env, struct vela_terminal *terminal)
{
    char *path = vela_terminal_choice_path(env);
    FILE *file = path ? fopen(path, "re") : NULL;
    free(path);
    if (!file) {
        return false;
    }
    char line[LINE_LENGTH];
    char *id = NULL;
    while (!id && next_line(file, line)) {
        char *s = trim(line);
        if (*s && *s != '#' && *s != '-' && *s != '/') {
            s[strcspn(s, ":")] = '\0';
            id = vela_strdup(s);
        }
    }
    fclose(file);
    if (!id) {
        return false;
    }
    struct found_list files = desktop_files(env);
    bool ok = false;
    for (int i = 0; !ok && i < files.count; ++i) {
        ok = strcmp(files.items[i].id, id) == 0
            && from_entry(env, &files.items[i], false, VELA_TERMINAL_CHOSEN, terminal);
    }
    found_free(&files);
    free(id);
    return ok;
}

// Step 2: VELA_TERMINAL, as the shell reads it: a command, maybe with
// options, at most 64 words.
static bool variable(const struct vela_terminal_env *env, struct vela_terminal *terminal)
{
    enum { MAX_WORDS = 64 };
    struct list words = { 0 };
    for (const char *c = env->vela_terminal ? env->vela_terminal : ""; *c;) {
        c += strspn(c, " \t");
        size_t length = strcspn(c, " \t");
        if (length > 0) {
            if (words.count == MAX_WORDS) {
                vela_terminal_strv_free(words.items);
                return false;
            }
            list_add(&words, vela_strndup(c, length));
        }
        c += length;
    }
    char *program = words.count > 0 ? find_program(env->path, words.items[0]) : NULL;
    if (!program) {
        vela_terminal_strv_free(words.items);
        return false;
    }
    free(words.items[0]);
    words.items[0] = program;
    terminal->source = VELA_TERMINAL_VARIABLE;
    terminal->argv = words.items;
    terminal->exec_arg = vela_strdup("-e");
    return true;
}

// Step 3: xdg-terminal-exec takes the command as it is.
static bool xdg_terminal_exec(const struct vela_terminal_env *env, struct vela_terminal *terminal)
{
    char *program = find_program(env->path, "xdg-terminal-exec");
    if (!program) {
        return false;
    }
    terminal->source = VELA_TERMINAL_XDG_TERMINAL_EXEC;
    terminal->argv = VELA_NEW_N(char *, 2);
    terminal->argv[0] = program;
    return true;
}

// Step 4.
static bool installed(const struct vela_terminal_env *env, struct vela_terminal *terminal)
{
    struct found_list files = desktop_files(env);
    bool ok = false;
    for (int i = 0; !ok && i < files.count; ++i) {
        ok = !excluded(files.items[i].id) && from_entry(env, &files.items[i], true, VELA_TERMINAL_INSTALLED, terminal);
    }
    found_free(&files);
    return ok;
}

bool vela_terminal_find(const struct vela_terminal_env *env, bool automatic, struct vela_terminal *terminal)
{
    memset(terminal, 0, sizeof(*terminal));
    return (!automatic && chosen(env, terminal)) || variable(env, terminal) || xdg_terminal_exec(env, terminal)
        || installed(env, terminal);
}

void vela_terminal_free(struct vela_terminal *terminal)
{
    free(terminal->id);
    vela_terminal_strv_free(terminal->argv);
    free(terminal->exec_arg);
    memset(terminal, 0, sizeof(*terminal));
}

char **vela_terminal_argv(const struct vela_terminal *terminal, const char *const *command)
{
    struct list out = { 0 };
    for (char **word = terminal->argv; word && *word; ++word) {
        list_add(&out, vela_strdup(*word));
    }
    if (command && command[0]) {
        if (set(terminal->exec_arg)) {
            list_add(&out, vela_strdup(terminal->exec_arg));
        }
        for (const char *const *word = command; *word; ++word) {
            list_add(&out, vela_strdup(*word));
        }
    }
    return list_finish(&out);
}

char **vela_terminal_installed(const struct vela_terminal_env *env)
{
    struct found_list files = desktop_files(env);
    struct list out = { 0 };
    for (int i = 0; i < files.count; ++i) {
        struct vela_terminal terminal;
        memset(&terminal, 0, sizeof(terminal));
        if (!excluded(files.items[i].id)
            && from_entry(env, &files.items[i], true, VELA_TERMINAL_INSTALLED, &terminal)) {
            list_add(&out, vela_strdup(files.items[i].id));
        }
        vela_terminal_free(&terminal);
    }
    found_free(&files);
    return list_finish(&out);
}
