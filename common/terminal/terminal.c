// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "terminal.h"

#include "alloc/alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void vela_terminal_env_get(struct vela_terminal_env *env)
{
    env->home = getenv("HOME");
    env->path = getenv("PATH");
    env->vela_terminal = getenv("VELA_TERMINAL");
    env->config_home = getenv("XDG_CONFIG_HOME");
}

static bool set(const char *value)
{
    return value && *value;
}

char *vela_terminal_choice_path(const struct vela_terminal_env *env)
{
    if (env->config_home && env->config_home[0] == '/') {
        return vela_asprintf("%s/vela-xdg-terminals.list", env->config_home);
    }
    return env->home && env->home[0] == '/' ? vela_asprintf("%s/.config/vela-xdg-terminals.list", env->home) : NULL;
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
    for (const char *dir = path; dir && *dir;) {
        size_t length = strcspn(dir, ":");
        if (length && length < 4096) {
            char *candidate = vela_asprintf("%.*s/%s", (int)length, dir, name);
            if (executable(candidate)) {
                return candidate;
            }
            free(candidate);
        }
        dir += length;
        dir += *dir == ':';
    }
    return NULL;
}

// Read just the preference that takes precedence over VELA_TERMINAL.
// xdg-terminal-exec reads the list itself, including actions and directives.
static char *preference(const struct vela_terminal_env *env)
{
    char *path = vela_terminal_choice_path(env);
    FILE *file = path ? fopen(path, "re") : NULL;
    free(path);
    if (!file) {
        return NULL;
    }
    char *line = NULL;
    size_t capacity = 0;
    char *id = NULL;
    while (!id) {
        errno = 0;
        if (getline(&line, &capacity, file) < 0) {
            if (errno == ENOMEM) {
                vela_oom(capacity, 1, __FILE__, __LINE__);
            }
            break;
        }
        char *start = line + strspn(line, " \t");
        if (strchr("#+-/", *start)) {
            continue; // comments, directives, actions, blank lines
        }
        // The id is the first word; it must name a .desktop file.
        char *word = vela_strndup(start, strcspn(start, ": \t\r\n"));
        const char *dot = strrchr(word, '.');
        if (dot && dot != word && strcmp(dot, ".desktop") == 0) {
            id = word;
        } else {
            free(word);
        }
    }
    free(line);
    fclose(file);
    return id;
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

bool vela_terminal_find(const struct vela_terminal_env *env, struct vela_terminal *terminal)
{
    memset(terminal, 0, sizeof(*terminal));
    char *id = preference(env);
    if (id && xdg_terminal_exec(env, terminal)) {
        terminal->source = VELA_TERMINAL_CHOSEN;
        terminal->id = id;
        return true;
    }
    free(id);
    return variable(env, terminal) || xdg_terminal_exec(env, terminal);
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
