// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A configuration file that exists but cannot be opened or read to the end is
// never rewritten from what was read (docs/memory-allocation-policy.md,
// "Existing defects", 1). Runs under getline_fail_preload.so (CMake sets
// LD_PRELOAD), which makes getline fail partway through a chosen file.

#include "config.h"
#include "desktop_conf.h"
#include "output_config.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// CTest builds with NDEBUG in RelWithDebInfo, so assert() would skip checks.
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static char root[256];
static char dir[512];

static void path_of(const char *name, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", dir, name);
}

static void write_file(const char *name, const char *text)
{
    char path[640];
    path_of(name, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    CHECK(fd >= 0);
    size_t length = strlen(text);
    CHECK(write(fd, text, length) == (ssize_t)length);
    CHECK(close(fd) == 0);
}

// Takes every permission away from the file, through a descriptor (the
// path is never looked up twice). Returns it for give_back().
static int take_permissions(const char *name)
{
    char path[640];
    path_of(name, path, sizeof(path));
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(fd >= 0);
    CHECK(fchmod(fd, 0) == 0);
    return fd;
}

static void give_back(int fd)
{
    CHECK(fchmod(fd, 0600) == 0);
    CHECK(close(fd) == 0);
}

// The file's whole content, or "<missing>".
static char *read_file(const char *name)
{
    char path[640];
    path_of(name, path, sizeof(path));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return strdup("<missing>");
    }
    char *text = calloc(1, 65536);
    CHECK(text);
    ssize_t n = read(fd, text, 65535);
    close(fd);
    CHECK(n >= 0);
    return text;
}

static void expect_content(const char *name, const char *expected)
{
    char *text = read_file(name);
    if (strcmp(text, expected) != 0) {
        fprintf(stderr, "%s is\n---\n%s---\nexpected\n---\n%s---\n", name, text, expected);
        exit(EXIT_FAILURE);
    }
    free(text);
}

static void remove_file(const char *name)
{
    char path[640];
    path_of(name, path, sizeof(path));
    unlink(path);
}

static void fail_getline(const char *name, int line, const char *error)
{
    char spec[128];
    snprintf(spec, sizeof(spec), "%s %d %s", name, line, error);
    CHECK(setenv("VELA_TEST_GETLINE_FAIL", spec, 1) == 0);
}

static void no_getline_failure(void)
{
    unsetenv("VELA_TEST_GETLINE_FAIL");
}

// Runs `operation` in a child whose descriptors are exhausted: any new open
// fails with EMFILE. Returns its result.
static bool with_no_free_descriptor(bool (*operation)(void))
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int highest = 2;
        for (int fd = 3; fd < 1024; ++fd) {
            if (fcntl(fd, F_GETFD) >= 0) {
                highest = fd;
            }
        }
        struct rlimit limit = { (rlim_t)highest + 1, (rlim_t)highest + 1 };
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0 || open("/dev/null", O_RDONLY) >= 0 || errno != EMFILE) {
            _exit(2); // the setup itself failed
        }
        _exit(operation() ? 1 : 0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    CHECK(WEXITSTATUS(status) != 2);
    return WEXITSTATUS(status) == 1;
}

static bool write_setting(void)
{
    return vela_config_write_checked("screen-off", "5");
}

static const struct vela_output_config_entry output = {
    .key = "Maker Model 1", .enabled = true, .width = 1920, .height = 1080, .refresh_mhz = 60000,
    .scale = 1.0f, .transform = 0, .x = 0, .y = 0,
};

static bool save_output(void)
{
    return vela_output_config_save(&output, 1);
}

static const char settings[] = "# mine\nscreen-off=10\ntheme=dark\nlanguage=it\n";

static void test_vela_conf(bool root_user)
{
    // A missing file is an empty one: the write creates it.
    remove_file("vela.conf");
    CHECK(vela_config_write_checked("screen-off", "5"));
    expect_content("vela.conf", "# Vela settings (written by the Settings app)\nscreen-off=5\n");

    // A readable file keeps everything else.
    write_file("vela.conf", settings);
    CHECK(vela_config_write_checked("screen-off", "5"));
    expect_content("vela.conf", "# mine\nscreen-off=5\ntheme=dark\nlanguage=it\n");

    // A read error partway through (I/O, or no memory for a line).
    for (int i = 0; i < 2; ++i) {
        const char *error = i == 0 ? "EIO" : "ENOMEM";
        write_file("vela.conf", settings);
        fail_getline("vela.conf", 3, error);
        CHECK(!vela_config_write_checked("screen-off", "5"));
        no_getline_failure();
        expect_content("vela.conf", settings);
    }

    // Exists but cannot be opened: descriptors exhausted.
    write_file("vela.conf", settings);
    CHECK(!with_no_free_descriptor(write_setting));
    expect_content("vela.conf", settings);

    // Exists but cannot be opened: no permission (root ignores it).
    if (!root_user) {
        int fd = take_permissions("vela.conf");
        CHECK(!vela_config_write_checked("screen-off", "5"));
        give_back(fd);
        expect_content("vela.conf", settings);
    }

    // The startup migration does not rewrite a file it could not read.
    write_file("vela.conf", "spegni-schermo=10\ntheme=dark\n");
    fail_getline("vela.conf", 2, "EIO");
    vela_config_migrate();
    no_getline_failure();
    expect_content("vela.conf", "spegni-schermo=10\ntheme=dark\n");

    // Reading for settings falls back to the defaults, without a crash.
    write_file("vela.conf", settings);
    fail_getline("vela.conf", 2, "EIO");
    struct vela_config config;
    vela_config_read(&config);
    no_getline_failure();
    vela_config_finish(&config);
}

static const char outputs[] = "# Vela's outputs\n\n[Other Monitor 2]\nenabled=yes\nmode=2560x1440@180.000\n"
                              "scale=1.25\n";
static const char legacy[] = "[Other Monitor 2]\nattivo=si\nmodo=1280x1024@60.000\n";

static void test_outputs_conf(bool root_user)
{
    // A read error partway through: the other monitor's settings survive.
    write_file("outputs.conf", outputs);
    fail_getline("outputs.conf", 4, "EIO");
    CHECK(!vela_output_config_save(&output, 1));
    no_getline_failure();
    expect_content("outputs.conf", outputs);

    // outputs.conf exists but cannot be opened: neither rewritten, nor
    // replaced by the stale legacy schermi.conf, which also stays.
    write_file("outputs.conf", outputs);
    write_file("schermi.conf", legacy);
    CHECK(!with_no_free_descriptor(save_output));
    expect_content("outputs.conf", outputs);
    expect_content("schermi.conf", legacy);
    if (!root_user) {
        int fd = take_permissions("outputs.conf");
        struct vela_saved_output saved;
        CHECK(!vela_output_config_load("Other Monitor 2", &saved)); // not the legacy values
        CHECK(!vela_output_config_save(&output, 1));
        give_back(fd);
        expect_content("outputs.conf", outputs);
        expect_content("schermi.conf", legacy);
    }

    // Only a missing outputs.conf reads the legacy file (as before), and the
    // first save replaces it.
    remove_file("outputs.conf");
    CHECK(vela_output_config_save(&output, 1));
    char *text = read_file("outputs.conf");
    CHECK(strstr(text, "[Other Monitor 2]") && strstr(text, "mode=1280x1024@60.000"));
    CHECK(strstr(text, "[Maker Model 1]"));
    free(text);
    expect_content("schermi.conf", "<missing>");
}

// ini sections (icons.c, text.c): a partial read gives no values at all.
static void test_ini(void)
{
    static const char *const keys[] = { "Name", "Icon" };
    char values[2][256];
    char path[640];
    path_of("app.desktop", path, sizeof(path));
    write_file("app.desktop", "[Other]\nName=Wrong\n[Desktop Entry]\nName=Files\nIcon=folder\n[Last]\n");
    CHECK(vela_ini_read(path, "Desktop Entry", keys, values, 2));
    CHECK(strcmp(values[0], "Files") == 0 && strcmp(values[1], "folder") == 0);
    CHECK(vela_ini_read(path, "Missing", keys, values, 2));
    CHECK(!values[0][0] && !values[1][0]);

    fail_getline("app.desktop", 5, "EIO"); // after Name, before Icon
    CHECK(!vela_ini_read(path, "Desktop Entry", keys, values, 2));
    no_getline_failure();
    CHECK(!values[0][0] && !values[1][0]); // not a mix

    remove_file("app.desktop");
    CHECK(!vela_ini_read(path, "Desktop Entry", keys, values, 2));
    CHECK(!values[0][0] && !values[1][0]);
}

struct desktops {
    char names[8][64];
    int count;
    char sticky[8][64];
    int sticky_count;
};

static void on_desktop(void *data, const char *name)
{
    struct desktops *d = data;
    snprintf(d->names[d->count++], sizeof(d->names[0]), "%s", name);
}

static void on_sticky(void *data, const char *app_id)
{
    struct desktops *d = data;
    snprintf(d->sticky[d->sticky_count++], sizeof(d->sticky[0]), "%s", app_id);
}

static bool read_desktops(void)
{
    char path[640];
    path_of("desktop.conf", path, sizeof(path));
    struct desktops d = { 0 };
    const struct vela_desktop_conf_reader reader = { on_desktop, on_sticky, &d };
    return vela_desktop_conf_read(path, &reader);
}

static void test_desktop_conf(void)
{
    char path[640];
    path_of("desktop.conf", path, sizeof(path));
    struct desktops d = { 0 };
    const struct vela_desktop_conf_reader reader = { on_desktop, on_sticky, &d };

    // Missing: no desktops, and safe to write later.
    remove_file("desktop.conf");
    CHECK(vela_desktop_conf_read(path, &reader) && d.count == 0);

    write_file("desktop.conf", "# mine\ndesktop=Work\ndesktop=\nsticky-app=org.kde.konsole\n"
                               "app-ovunque=old.name\nsticky-app=\n");
    CHECK(vela_desktop_conf_read(path, &reader));
    CHECK(d.count == 2 && strcmp(d.names[0], "Work") == 0 && d.names[1][0] == '\0');
    CHECK(d.sticky_count == 2 && strcmp(d.sticky[0], "org.kde.konsole") == 0
        && strcmp(d.sticky[1], "old.name") == 0); // the empty one is skipped

    memset(&d, 0, sizeof(d));
    fail_getline("desktop.conf", 3, "ENOMEM");
    CHECK(!vela_desktop_conf_read(path, &reader));
    no_getline_failure();
    CHECK(d.count == 1); // what was read before the failure, and false

    // Exists but cannot be opened.
    CHECK(!with_no_free_descriptor(read_desktops));
}

static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk)
{
    (void)info;
    (void)walk;
    return type == FTW_DP ? rmdir(path) : unlink(path);
}

int main(void)
{
    no_getline_failure();
    // A private directory under the test's own working directory (the build
    // tree), never in a publicly writable one.
    char cwd[192];
    CHECK(getcwd(cwd, sizeof(cwd)));
    snprintf(root, sizeof(root), "%s/vela-config-read-XXXXXX", cwd);
    CHECK(mkdtemp(root));
    CHECK(setenv("XDG_CONFIG_HOME", root, 1) == 0);
    snprintf(dir, sizeof(dir), "%s/vela", root);
    CHECK(mkdir(dir, 0700) == 0);
    bool root_user = geteuid() == 0;

    // The shim must really be loaded, or the read-error cases prove nothing.
    write_file("probe.conf", "one\ntwo\n");
    fail_getline("probe.conf", 1, "EIO");
    char probe[640];
    path_of("probe.conf", probe, sizeof(probe));
    FILE *file = fopen(probe, "re");
    CHECK(file);
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = 0;
    CHECK(vela_read_line(file, &line, &capacity, &length) == -1);
    fclose(file);
    no_getline_failure();
    file = fopen(probe, "re");
    CHECK(file);
    CHECK(vela_read_line(file, &line, &capacity, &length) == 1 && length == 4);
    CHECK(vela_read_line(file, &line, &capacity, &length) == 1);
    CHECK(vela_read_line(file, &line, &capacity, &length) == 0);
    fclose(file);
    free(line);
    remove_file("probe.conf");

    test_vela_conf(root_user);
    test_outputs_conf(root_user);
    test_ini();
    test_desktop_conf();

    CHECK(nftw(root, remove_entry, 8, FTW_DEPTH | FTW_PHYS) == 0);
    puts("configuration reads passed");
    return 0;
}
