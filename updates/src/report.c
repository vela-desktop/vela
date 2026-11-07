// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "report.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-json.h>
#include <unistd.h>

#define SCHEMA 2

// A key's string, copied; "" when missing or not a string.
static char *string_at(sd_json_variant *object, const char *key)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);
    const char *s = value && sd_json_variant_is_string(value) ? sd_json_variant_string(value) : "";
    return strdup(s ? s : "");
}

static int64_t integer_at(sd_json_variant *object, const char *key)
{
    sd_json_variant *value = sd_json_variant_by_key(object, key);
    return value && sd_json_variant_is_integer(value) ? sd_json_variant_integer(value) : 0;
}

// [{first_key, second_key}] as pairs; entries that aren't objects are skipped.
static int pairs_at(sd_json_variant *object, const char *key, const char *first_key, const char *second_key,
    struct vela_up_pair **pairs, size_t *count)
{
    *pairs = NULL;
    *count = 0;
    sd_json_variant *array = sd_json_variant_by_key(object, key);
    if (!array || !sd_json_variant_is_array(array)) {
        return 0;
    }
    size_t n = sd_json_variant_elements(array);
    if (n == 0) {
        return 0;
    }
    *pairs = calloc(n, sizeof(**pairs));
    if (!*pairs) {
        return -ENOMEM;
    }
    for (size_t i = 0; i < n; ++i) {
        sd_json_variant *entry = sd_json_variant_by_index(array, i);
        if (!entry || !sd_json_variant_is_object(entry)) {
            continue;
        }
        struct vela_up_pair *pair = &(*pairs)[(*count)++];
        pair->first = string_at(entry, first_key);
        pair->second = string_at(entry, second_key);
        if (!pair->first || !pair->second) {
            return -ENOMEM;
        }
    }
    return 0;
}

static void pairs_free(struct vela_up_pair *pairs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(pairs[i].first);
        free(pairs[i].second);
    }
    free(pairs);
}

static bool schema_2(sd_json_variant *v)
{
    return v && sd_json_variant_is_object(v) && integer_at(v, "schema_version") == SCHEMA;
}

static int check_from(sd_json_variant *v, struct vela_up_check *check)
{
    memset(check, 0, sizeof(*check));
    if (!schema_2(v)) {
        return -EINVAL;
    }
    check->channel = string_at(v, "channel");
    check->status = string_at(v, "status");
    check->installed_package = string_at(v, "installed_package");
    check->installed_version = string_at(v, "installed_version");
    check->version = string_at(v, "version");
    check->tag = string_at(v, "tag");
    check->commit = string_at(v, "commit");
    check->source = string_at(v, "source");
    check->checked_at = integer_at(v, "checked_at");
    int64_t total = integer_at(v, "changes_total");
    check->changes_total = total > 0 && total <= UINT32_MAX ? (uint32_t)total : 0;
    sd_json_variant *error = sd_json_variant_by_key(v, "error");
    bool has_error = error && sd_json_variant_is_object(error);
    check->error_code = has_error ? string_at(error, "code") : strdup("");
    check->error_message = has_error ? string_at(error, "message") : strdup("");
    int r = pairs_at(v, "changes", "commit", "summary", &check->changes, &check->change_count);
    if (r >= 0) {
        r = pairs_at(v, "notes", "version", "text", &check->notes, &check->note_count);
    }
    if (r < 0 || !check->channel || !check->status || !check->installed_package || !check->installed_version
        || !check->version || !check->tag || !check->commit || !check->source || !check->error_code
        || !check->error_message) {
        vela_up_check_clear(check);
        return r < 0 ? r : -ENOMEM;
    }
    // A failure without its code is still a failure.
    if (strcmp(check->status, "error") == 0 && !*check->error_code) {
        free(check->error_code);
        check->error_code = strdup("failed");
    }
    check->valid = true;
    return 0;
}

int vela_up_check_parse(const char *json, struct vela_up_check *check)
{
    sd_json_variant *v = NULL;
    int r = sd_json_parse(json, 0, &v, NULL, NULL);
    if (r >= 0) {
        r = check_from(v, check);
    } else {
        memset(check, 0, sizeof(*check));
    }
    sd_json_variant_unref(v);
    return r;
}

static int parse_file(const char *path, sd_json_variant **v)
{
    FILE *f = fopen(path, "re");
    if (!f) {
        return -errno;
    }
    int r = sd_json_parse_file(f, path, 0, v, NULL, NULL);
    fclose(f);
    return r;
}

int vela_up_check_read(const char *path, struct vela_up_check *check)
{
    sd_json_variant *v = NULL;
    int r = parse_file(path, &v);
    if (r >= 0) {
        r = check_from(v, check);
    } else {
        memset(check, 0, sizeof(*check));
    }
    sd_json_variant_unref(v);
    return r;
}

void vela_up_check_clear(struct vela_up_check *check)
{
    free(check->channel);
    free(check->status);
    free(check->installed_package);
    free(check->installed_version);
    free(check->version);
    free(check->tag);
    free(check->commit);
    free(check->source);
    pairs_free(check->changes, check->change_count);
    pairs_free(check->notes, check->note_count);
    free(check->error_code);
    free(check->error_message);
    memset(check, 0, sizeof(*check));
}

int vela_up_run_read(const char *path, struct vela_up_run *run)
{
    memset(run, 0, sizeof(*run));
    sd_json_variant *v = NULL;
    int r = parse_file(path, &v);
    if (r >= 0 && !schema_2(v)) {
        r = -EINVAL;
    }
    if (r >= 0) {
        run->action = string_at(v, "action");
        run->channel = string_at(v, "channel");
        run->phase = string_at(v, "phase");
        int64_t pid = integer_at(v, "pid");
        run->pid = pid > 0 && pid <= INT_MAX ? (pid_t)pid : 0;
        int64_t start = integer_at(v, "start_time");
        run->start_time = start > 0 ? (uint64_t)start : 0;
        run->started_at = integer_at(v, "started_at");
        if (!run->action || !run->channel || !run->phase) {
            vela_up_run_clear(run);
            r = -ENOMEM;
        } else {
            run->present = run->pid > 0;
        }
    }
    sd_json_variant_unref(v);
    return r;
}

void vela_up_run_clear(struct vela_up_run *run)
{
    free(run->action);
    free(run->channel);
    free(run->phase);
    memset(run, 0, sizeof(*run));
}

int vela_up_install_read(const char *path, struct vela_up_install *install)
{
    memset(install, 0, sizeof(*install));
    sd_json_variant *v = NULL;
    int r = parse_file(path, &v);
    if (r >= 0 && !schema_2(v)) {
        r = -EINVAL;
    }
    if (r >= 0) {
        install->channel = string_at(v, "channel");
        install->result = string_at(v, "result");
        install->installed_version = string_at(v, "installed_version");
        install->finished_at = integer_at(v, "finished_at");
        sd_json_variant *error = sd_json_variant_by_key(v, "error");
        bool has_error = error && sd_json_variant_is_object(error);
        install->error_code = has_error ? string_at(error, "code") : strdup("");
        install->error_message = has_error ? string_at(error, "message") : strdup("");
        if (!install->channel || !install->result || !install->installed_version || !install->error_code
            || !install->error_message) {
            vela_up_install_clear(install);
            r = -ENOMEM;
        } else {
            install->present = true;
        }
    }
    sd_json_variant_unref(v);
    return r;
}

void vela_up_install_clear(struct vela_up_install *install)
{
    free(install->channel);
    free(install->result);
    free(install->error_code);
    free(install->error_message);
    free(install->installed_version);
    memset(install, 0, sizeof(*install));
}

uint64_t vela_up_start_time(const char *proc, pid_t pid)
{
    char path[PATH_MAX];
    if (pid <= 0 || snprintf(path, sizeof(path), "%s/%d/stat", proc, (int)pid) >= (int)sizeof(path)) {
        return 0;
    }
    FILE *f = fopen(path, "re");
    if (!f) {
        return 0;
    }
    char line[4096];
    char *read = fgets(line, sizeof(line), f);
    fclose(f);
    // The name, field 2, is in parentheses and may contain anything: the
    // fields are counted after the last ')'. Field 22 is the 20th after it.
    char *end = read ? strrchr(line, ')') : NULL;
    if (!end) {
        return 0;
    }
    char *field = end + 1;
    for (int i = 0; i < 19 && field; ++i) {
        field = strchr(field + 1, ' ');
    }
    return field ? strtoull(field + 1, NULL, 10) : 0;
}

bool vela_up_run_alive(const char *proc, const struct vela_up_run *run)
{
    if (!run->present || run->pid <= 0) {
        return false;
    }
    uint64_t now = vela_up_start_time(proc, run->pid);
    // A run that couldn't read its start time is trusted by PID alone.
    return now != 0 && (run->start_time == 0 || now == run->start_time);
}

static bool word(const char *s, size_t max)
{
    size_t length = s ? strlen(s) : 0;
    if (length == 0 || length > max || s[0] == '-') {
        return false;
    }
    for (const char *c = s; *c; ++c) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) {
            return false;
        }
    }
    return true;
}

// git check-ref-format --branch, in short.
static bool branch_valid(const char *b)
{
    if (!word(b, 255) || strstr(b, "..") || strstr(b, "@{") || strstr(b, "//") || b[strlen(b) - 1] == '/'
        || b[strlen(b) - 1] == '.' || b[0] == '/' || b[0] == '.') {
        return false;
    }
    size_t length = strlen(b);
    if (length >= 5 && strcmp(b + length - 5, ".lock") == 0) {
        return false;
    }
    return strpbrk(b, " ~^:?*[\\") == NULL && strcmp(b, "@") != 0;
}

static bool commit_valid(const char *c)
{
    size_t length = c ? strlen(c) : 0;
    if (length < 4 || length > 40) {
        return false;
    }
    for (; *c; ++c) {
        if (!isxdigit((unsigned char)*c)) {
            return false;
        }
    }
    return true;
}

static bool tag_valid(const char *t)
{
    // v0.<minor>.<patch>
    if (!t || strncmp(t, "v0.", 3) != 0) {
        return false;
    }
    const char *c = t + 3;
    int parts = 1;
    bool digit = false;
    for (; *c; ++c) {
        if (*c == '.' && digit) {
            ++parts;
            digit = false;
        } else if (isdigit((unsigned char)*c)) {
            digit = true;
        } else {
            return false;
        }
    }
    return parts == 2 && digit;
}

const char *vela_up_options_invalid(const struct vela_up_options *o, const char *installed)
{
    if (o->channel && strcmp(o->channel, "beta") != 0 && strcmp(o->channel, "experimental") != 0) {
        return "unknown channel";
    }
    const char *channel = o->channel ? o->channel : (installed && *installed ? installed : NULL);
    bool beta = channel && strcmp(channel, "beta") == 0;
    bool experimental = channel && strcmp(channel, "experimental") == 0;
    if (o->branch && o->commit) {
        return "use either a branch or a commit, not both";
    }
    if ((o->branch || o->commit || o->source) && beta) {
        return "branch, commit and source are for the Experimental channel";
    }
    if (o->tag && experimental) {
        return "a release tag is for the Beta channel";
    }
    if (o->branch && !branch_valid(o->branch)) {
        return "invalid branch name";
    }
    if (o->commit && !commit_valid(o->commit)) {
        return "a commit is 4 to 40 hexadecimal digits";
    }
    if (o->source && !word(o->source, 4096)) {
        return "invalid source";
    }
    if (o->tag && !tag_valid(o->tag)) {
        return "a release tag looks like v0.1.2";
    }
    return NULL;
}

void vela_up_options_argv(const struct vela_up_options *o, const char *command, const char **argv, char tag[64])
{
    size_t n = 0;
    argv[n++] = command;
    if (o->channel) {
        argv[n++] = "--channel";
        argv[n++] = o->channel;
    }
    if (o->branch) {
        argv[n++] = "--branch";
        argv[n++] = o->branch;
    }
    if (o->commit) {
        argv[n++] = "--commit";
        argv[n++] = o->commit;
    }
    if (o->source) {
        argv[n++] = "--source";
        argv[n++] = o->source;
    }
    if (o->tag) {
        // The Beta helper takes it only as one word.
        snprintf(tag, 64, "--tag=%s", o->tag);
        argv[n++] = tag;
    }
    argv[n] = NULL;
}

// `name`'s full path in `path`, or NULL.
static char *find_program(const char *path, const char *name)
{
    if (!name || !*name) {
        return NULL;
    }
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0 ? strdup(name) : NULL;
    }
    for (const char *dir = path ? path : ""; *dir;) {
        size_t length = strcspn(dir, ":");
        char candidate[PATH_MAX];
        if (length > 0 && (size_t)snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, dir, name)
                < sizeof(candidate)
            && access(candidate, X_OK) == 0) {
            return strdup(candidate);
        }
        dir += length;
        dir += *dir == ':';
    }
    return NULL;
}

void vela_up_strv_free(char **strv)
{
    for (char **s = strv; s && *s; ++s) {
        free(*s);
    }
    free(strv);
}

char **vela_up_terminal(const char *path, const char *vela_terminal)
{
    char *program = find_program(path, "xdg-terminal-exec");
    if (program) {
        char **argv = calloc(2, sizeof(*argv));
        if (!argv) {
            free(program);
            return NULL;
        }
        argv[0] = program;
        return argv;
    }
    // VELA_TERMINAL as the shell reads it: a command, maybe with options.
    const char *chosen = vela_terminal && *vela_terminal ? vela_terminal : "konsole";
    size_t words = 0;
    for (const char *c = chosen; *c;) {
        c += strspn(c, " ");
        if (*c) {
            ++words;
            c += strcspn(c, " ");
        }
    }
    // No terminal needs more; a longer VELA_TERMINAL is a mistake.
    enum { MAX_WORDS = 64 };
    if (words > MAX_WORDS) {
        return NULL;
    }
    char **argv = calloc(MAX_WORDS + 2, sizeof(*argv));
    if (!argv) {
        return NULL;
    }
    size_t n = 0;
    for (const char *c = chosen; *c;) {
        c += strspn(c, " ");
        size_t length = strcspn(c, " ");
        if (length > 0) {
            argv[n++] = strndup(c, length);
        }
        c += length;
    }
    char *found = n > 0 && argv[0] ? find_program(path, argv[0]) : NULL;
    if (!found) {
        vela_up_strv_free(argv);
        return NULL;
    }
    free(argv[0]);
    argv[0] = found;
    argv[n++] = strdup("-e");
    for (size_t i = 0; i < n; ++i) {
        if (!argv[i]) {
            vela_up_strv_free(argv);
            return NULL;
        }
    }
    return argv;
}

// /proc/PID/exe's target, without " (deleted)"; `deleted` says whether it
// was there.
static bool executable(const char *proc, pid_t pid, char *exe, size_t size, bool *deleted)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%d/exe", proc, (int)pid) >= (int)sizeof(path)) {
        return false;
    }
    // Zeroed first, readlink leaves the terminator in place.
    memset(exe, 0, size);
    if (readlink(path, exe, size - 1) <= 0) {
        return false;
    }
    static const char suffix[] = " (deleted)";
    size_t n = strlen(exe);
    char *tail = n > strlen(suffix) ? exe + n - strlen(suffix) : NULL;
    *deleted = tail && strcmp(tail, suffix) == 0;
    if (*deleted) {
        *tail = '\0';
    }
    return true;
}

static pid_t parent(const char *proc, pid_t pid)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%d/stat", proc, (int)pid) >= (int)sizeof(path)) {
        return 0;
    }
    FILE *f = fopen(path, "re");
    if (!f) {
        return 0;
    }
    char line[4096];
    char *read = fgets(line, sizeof(line), f);
    fclose(f);
    char *end = read ? strrchr(line, ')') : NULL;
    int ppid = 0;
    // ") S 1234 ...": state, then the parent.
    return end && sscanf(end + 1, " %*c %d", &ppid) == 1 ? (pid_t)ppid : 0;
}

pid_t vela_up_session_process(const char *proc, pid_t pid)
{
    for (int depth = 0; pid > 1 && depth < 16; ++depth, pid = parent(proc, pid)) {
        char exe[PATH_MAX];
        bool deleted;
        if (executable(proc, pid, exe, sizeof(exe), &deleted)) {
            const char *name = strrchr(exe, '/');
            if (strcmp(name ? name + 1 : exe, "vela-compositor") == 0) {
                return pid;
            }
        }
    }
    return 0;
}

bool vela_up_exe_replaced(const char *proc, pid_t pid)
{
    char exe[PATH_MAX];
    bool deleted = false;
    return pid > 0 && executable(proc, pid, exe, sizeof(exe), &deleted) && deleted;
}
