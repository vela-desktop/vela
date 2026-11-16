// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A one-rectangle region normally takes no heap. Forced mode must turn each
// fast path into a genuine broken pixman region, while natural mode must leave
// those same allocation-free calls untouched. No GPU is needed.
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pixman_bool_t operation(const char *name, pixman_region32_t *dest,
    const pixman_region32_t *a, const pixman_region32_t *b)
{
    switch (*name) {
    case 'u':
        return name[5] == '_' ? pixman_region32_union_rect(dest, a, 0, 0, 32, 32)
                             : pixman_region32_union(dest, a, b);
    case 'i': return pixman_region32_intersect(dest, a, b);
    case 's': return pixman_region32_subtract(dest, a, b);
    default: return pixman_region32_copy(dest, a);
    }
}

int main(void)
{
    char directory[] = "/tmp/vela-pixman-test-XXXXXX";
    assert(mkdtemp(directory));
    int fd = open(directory, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    char number[32];
    snprintf(number, sizeof(number), "%d", fd);
    assert(setenv("VELA_TEST_PIXMAN_FAIL_DIR", number, 1) == 0);
    const char *names[] = { "union", "union_rect", "intersect", "subtract", "copy" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        for (int force = 0; force <= 1; ++force) {
            pixman_region32_t a, b, dest;
            pixman_region32_init_rect(&a, 0, 0, 32, 32);
            pixman_region32_init_rect(&b, 0, 0, 32, 32);
            pixman_region32_init(&dest);
            FILE *trigger = fdopen(openat(fd, "pixman-fail", O_CREAT | O_WRONLY | O_TRUNC, 0600), "w");
            assert(trigger);
            fprintf(trigger, "%ld %s 0 %u%s", (long)getpid(), names[i], 2 * i + (unsigned)force,
                force ? " force" : "");
            assert(fclose(trigger) == 0);
            pixman_bool_t ok = operation(names[i], &dest, &a, &b);
            // Subtract already allocates even for these rectangles; retain
            // coverage of that natural OOM path as well as forced fast paths.
            bool fails = force || names[i][0] == 's';
            assert(!!ok == !fails);
            if (fails) {
                assert(!pixman_region32_selfcheck(&dest));
                assert(faccessat(fd, "pixman-fail", F_OK, 0) != 0);
            } else {
                assert(pixman_region32_selfcheck(&dest));
                assert(unlinkat(fd, "pixman-fail", 0) == 0);
            }
            pixman_region32_fini(&dest);
            pixman_region32_fini(&a);
            pixman_region32_fini(&b);
        }
    }
    close(fd);
    assert(rmdir(directory) == 0);
    puts("pixman failure modes: ok");
    return 0;
}
