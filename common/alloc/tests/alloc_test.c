// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The allocation helpers' contract (docs/memory-allocation-policy.md, "Tests"):
// never NULL, zeroed, overflow is failure, and every failure aborts with one
// line that names the caller's own file and line. Linked with
// -Wl,--wrap=calloc,--wrap=realloc so a real allocation failure can be forced
// without exhausting memory.

#include "alloc/alloc.h"

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include <assert.h>
#include <limits.h>
#include <malloc.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

#if defined(__SANITIZE_ADDRESS__)
#define VELA_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VELA_TEST_ASAN 1
#endif
#endif

static int calloc_calls, realloc_calls;
static int fail_calloc, fail_realloc; // 1-based call to fail, 0: none

void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *items, size_t size);

// C allows NULL for a zero-size request; glibc returns a pointer. Behave like
// the libc that returns NULL, so vela_alloc must not pass a zero size on.
void *__wrap_calloc(size_t count, size_t size)
{
    if (++calloc_calls == fail_calloc || count == 0 || size == 0) {
        return NULL;
    }
    return __real_calloc(count, size);
}

// Fresh memory is often zero already: poison what realloc adds, so only
// vela_grow's own zeroing can make the tail zero.
void *__wrap_realloc(void *items, size_t size)
{
    if (++realloc_calls == fail_realloc) {
        return NULL;
    }
    size_t old = items ? malloc_usable_size(items) : 0;
    unsigned char *bigger = __real_realloc(items, size);
    if (bigger && size > old) {
        memset(bigger + old, 0xa5, size - old);
    }
    return bigger;
}

static void reset_counts(void)
{
    calloc_calls = realloc_calls = fail_calloc = fail_realloc = 0;
}

// ---- Death tests ------------------------------------------------------------

static int failures;

// Coverage builds (GCC's gcov, VELA_TEST_COVERAGE from CMake) flush their
// counters at exit, which an abort skips: flush them from the SIGABRT
// handler. When it returns, abort() still ends the process with SIGABRT.
#ifdef VELA_TEST_COVERAGE
extern void __gcov_dump(void);
#endif

static void flush_coverage(int signal_number)
{
    (void)signal_number;
#ifdef VELA_TEST_COVERAGE
    __gcov_dump();
#endif
}

static pid_t start_child(int *out)
{
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core);
        signal(SIGABRT, flush_coverage);
        return 0;
    }
    close(pipe_fds[1]);
    *out = pipe_fds[0];
    return pid;
}

static void finish_child(pid_t pid, int fd, const char *expected, int site)
{
    char text[1024];
    size_t length = 0;
    ssize_t n;
    while (length < sizeof(text) - 1 && (n = read(fd, text + length, sizeof(text) - 1 - length)) > 0) {
        length += (size_t)n;
    }
    text[length] = '\0';
    close(fd);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);

    // site 0: the location is not checked.
    char location[64] = "";
    if (site > 0) {
        snprintf(location, sizeof(location), "alloc_test.c:%d", site);
    }
    bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    if (!aborted || !strstr(text, expected) || !strstr(text, location) || !strchr(text, '\n')) {
        fprintf(stderr, "line %d: expected SIGABRT with \"%s\" at %s, got status %d and: %s\n",
            site, expected, location, status, text);
        ++failures;
    }
}

// The expression runs in a child that must abort with `expected` and the line
// of this macro's invocation. Keep each use on one line.
#define EXPECT_ABORT(expected, expression)                                                         \
    do {                                                                                          \
        int site_ = __LINE__, fd_ = -1;                                                           \
        pid_t pid_ = start_child(&fd_);                                                           \
        if (pid_ == 0) {                                                                          \
            (void)(expression);                                                                   \
            _exit(77);                                                                            \
        }                                                                                         \
        finish_child(pid_, fd_, (expected), site_);                                               \
    } while (0)

static void overflow_names_the_call_site(void)
{
    int int_capacity = 0;
    size_t size_capacity = 0;
    void *items = NULL;
    EXPECT_ABORT("out of memory (18446744073709551615 x 2 bytes)", vela_alloc(SIZE_MAX, 2));
    EXPECT_ABORT("out of memory (4611686018427387904 x 8 bytes)", VELA_NEW_N(uint64_t, SIZE_MAX / 4 + 1));
    EXPECT_ABORT("out of memory (2147483648 x 4 bytes)", vela_grow(items, &int_capacity, (long long)INT_MAX + 1, 4));
    // A 64-bit count of 2^32 arrives intact instead of being truncated to 0.
    EXPECT_ABORT("out of memory (4294967296 x 4 bytes)", vela_grow(items, &int_capacity, (int64_t)1 << 32, 4));
    EXPECT_ABORT("out of memory (9223372036854775808 x 4 bytes)", vela_grow(items, &size_capacity, SIZE_MAX / 2 + 1, 4));
    EXPECT_ABORT("size overflow (18446744073709551615 + 1)", vela_size_add(SIZE_MAX, 1));
    EXPECT_ABORT("out of memory (4294967296 x 4294967296 bytes)", vela_size_mul((size_t)1 << 32, (size_t)1 << 32));
}

static void misuse_is_reported_as_such(void)
{
    int int_capacity = 0;
    size_t size_capacity = 0;
    void *items = NULL;
    EXPECT_ABORT("allocation misuse (negative count)", vela_grow(items, &int_capacity, -1, 4));
    // A size_t above LLONG_MAX arrives negative in the int form.
    size_t huge = (size_t)LLONG_MAX + 1;
    EXPECT_ABORT("allocation misuse (negative count)", vela_grow(items, &int_capacity, huge, 4));
    EXPECT_ABORT("allocation misuse (element size 0)", vela_grow(items, &size_capacity, 1, 0));
    size_capacity = 4;
    EXPECT_ABORT("allocation misuse (capacity without an array)", vela_grow(items, &size_capacity, 1, 1));
    int_capacity = -1;
    EXPECT_ABORT("allocation misuse (negative capacity)", vela_grow(items, &int_capacity, 1, 1));
    EXPECT_ABORT("allocation misuse (NULL string)", vela_strdup(NULL));
    EXPECT_ABORT("allocation misuse (NULL string)", vela_strndup(NULL, 3));
    // Not a character in any encoding: vsnprintf fails with EILSEQ.
    static const wchar_t invalid[] = { 0x110000, 0 };
    EXPECT_ABORT("allocation misuse (format failed)", vela_asprintf("%ls", invalid));
}

static void allocation_failure_aborts(void)
{
    reset_counts();
    int int_capacity = 0;
    size_t size_capacity = 0;
    void *items = NULL;
    fail_calloc = 1;
    EXPECT_ABORT("out of memory (3 x 16 bytes)", vela_alloc(3, 16));
    EXPECT_ABORT("out of memory (0 x 16 bytes)", vela_alloc(0, 16));
    EXPECT_ABORT("out of memory (6 x 1 bytes)", vela_strdup("hello"));
    EXPECT_ABORT("out of memory (3 x 1 bytes)", vela_strndup("hello", 2));
    EXPECT_ABORT("out of memory (8 x 1 bytes)", vela_asprintf("%s-%d", "ab", 1234));
    fail_realloc = 1;
    EXPECT_ABORT("out of memory (8 x 4 bytes)", vela_grow(items, &int_capacity, 1, 4));
    EXPECT_ABORT("out of memory (32 x 1 bytes)", vela_grow(items, &size_capacity, 20, 1));
    reset_counts();
}

// A real failure, not a forced one: the address space is limited below the
// request. ASan reserves terabytes of shadow memory and cannot run under it.
static void real_failure_reports_memory(void)
{
#ifndef VELA_TEST_ASAN
    int fd = -1;
    pid_t pid = start_child(&fd);
    if (pid == 0) {
        struct rlimit limit = { 256u << 20, 256u << 20 };
        assert(setrlimit(RLIMIT_AS, &limit) == 0);
        (void)vela_alloc(1, (size_t)1 << 30);
        _exit(77);
    }
    finish_child(pid, fd, "out of memory (1 x 1073741824 bytes)", 0);
    // Both figures from /proc: "...; rss <n> MiB; available <n> MiB".
    pid = start_child(&fd);
    if (pid == 0) {
        fail_calloc = calloc_calls + 1;
        (void)vela_alloc(1, 1);
        _exit(77);
    }
    finish_child(pid, fd, " MiB; available ", 0);
#endif
}

// ---- The contract when nothing fails -----------------------------------------

static bool all_zero(const void *block, size_t bytes)
{
    const unsigned char *c = block;
    for (size_t i = 0; i < bytes; ++i) {
        if (c[i]) {
            return false;
        }
    }
    return true;
}

static void contract(void)
{
    reset_counts();
    // Never NULL, zero sizes included, and freeable.
    void *empty = vela_alloc(0, 8);
    assert(empty);
    free(empty);
    empty = vela_alloc(8, 0);
    assert(empty);
    free(empty);
    struct pair { int a; long b; } *pair = VELA_NEW(struct pair);
    assert(pair && pair->a == 0 && pair->b == 0);
    free(pair);
    unsigned char *block = vela_alloc(4096, 3);
    assert(all_zero(block, 4096 * 3));
    free(block);
    int *numbers = VELA_NEW_N(int, 5);
    assert(all_zero(numbers, 5 * sizeof(int)));
    free(numbers);

    // An empty array asked for nothing still gets the initial capacity.
    int capacity = 0;
    int *list = vela_grow((int *)NULL, &capacity, 0, sizeof(*list));
    assert(list && capacity == 8 && all_zero(list, 8 * sizeof(*list)));
    for (int i = 0; i < 8; ++i) {
        list[i] = i + 1;
    }
    // No change when it is already large enough.
    int calls = realloc_calls;
    assert(vela_grow(list, &capacity, 8, sizeof(*list)) == list && capacity == 8);
    assert(realloc_calls == calls);
    // Doubling, contents preserved, new tail zeroed.
    list = vela_grow(list, &capacity, 9, sizeof(*list));
    assert(capacity == 16);
    for (int i = 0; i < 8; ++i) {
        assert(list[i] == i + 1);
    }
    assert(all_zero(list + 8, 8 * sizeof(*list)));
    list = vela_grow(list, &capacity, 100, sizeof(*list));
    assert(capacity == 128 && list[7] == 8 && all_zero(list + 16, 112 * sizeof(*list)));
    free(list);

    // The size_t form for byte buffers.
    size_t bytes_capacity = 0;
    char *bytes = vela_grow((char *)NULL, &bytes_capacity, 3, 1);
    assert(bytes && bytes_capacity == 8 && all_zero(bytes, 8));
    memcpy(bytes, "abcdefgh", 8);
    bytes = vela_grow(bytes, &bytes_capacity, 17, 1);
    assert(bytes_capacity == 32 && memcmp(bytes, "abcdefgh", 8) == 0 && all_zero(bytes + 8, 24));
    free(bytes);

    // Close to the int limit, doubling stops at INT_MAX instead of wrapping.
    // (Only the arithmetic is checked: the realloc is forced to fail.)
    capacity = 0;
    fail_realloc = realloc_calls + 1;
    EXPECT_ABORT("out of memory (2147483647 x 1 bytes)", vela_grow((char *)NULL, &capacity, INT_MAX - 1, 1));
    fail_realloc = 0;

    // Strings.
    char *copy = vela_strdup("vela");
    assert(strcmp(copy, "vela") == 0);
    free(copy);
    copy = vela_strdup("");
    assert(copy && *copy == '\0');
    free(copy);
    copy = vela_strndup("compositor", 4);
    assert(strcmp(copy, "comp") == 0);
    free(copy);
    copy = vela_strndup("ab", SIZE_MAX);
    assert(strcmp(copy, "ab") == 0);
    free(copy);
    copy = vela_asprintf("%s:%d", "output", 2);
    assert(strcmp(copy, "output:2") == 0);
    free(copy);
    copy = vela_asprintf("%s", "");
    assert(copy && *copy == '\0');
    free(copy);

    assert(vela_size_add(SIZE_MAX - 1, 1) == SIZE_MAX);
    assert(vela_size_mul(SIZE_MAX / 2, 2) == SIZE_MAX - 1);
    assert(vela_size_mul(0, SIZE_MAX) == 0);
}

int main(void)
{
    contract();
    overflow_names_the_call_site();
    misuse_is_reported_as_such();
    allocation_failure_aborts();
    real_failure_reports_memory();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
