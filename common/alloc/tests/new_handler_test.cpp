// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The C++ new handler (alloc/new_handler.h): a failed operator new aborts
// with the policy's one-line report naming the process.

#include "alloc/new_handler.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef VELA_TEST_COVERAGE
extern "C" void __gcov_dump(void);
#endif

namespace {

void flush_coverage(int)
{
#ifdef VELA_TEST_COVERAGE
    __gcov_dump();
#endif
}

// The child's stderr and its exit status, after `body` ran in it.
template<typename Body> std::string run_child(Body body, int *status)
{
    int fds[2];
    if (pipe(fds) != 0) {
        std::perror("pipe");
        _exit(1);
    }
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        struct rlimit no_core = { 0, 0 };
        setrlimit(RLIMIT_CORE, &no_core);
        std::signal(SIGABRT, flush_coverage);
        body();
        _exit(77);
    }
    close(fds[1]);
    std::string text;
    char chunk[512];
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof(chunk))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) {
            text.append(chunk, static_cast<size_t>(n));
        }
    }
    close(fds[0]);
    waitpid(pid, status, 0);
    return text;
}

} // namespace

int main()
{
#ifdef VELA_TEST_ASAN
    // ASan answers an impossible size itself, before any new handler.
    std::puts("ASan build: new handler test skipped");
    return 77;
#else
    int failures = 0;
    int status = 0;
    std::string text = run_child(
        [] {
            vela::install_new_handler("vela-new-handler-test");
            // Far beyond any address space: malloc fails, the handler runs.
            void *block = ::operator new(SIZE_MAX / 2);
            std::printf("%p\n", block);
        },
        &status);
    bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    if (!aborted || text.find("vela: out of memory (operator new) in vela-new-handler-test") != 0
        || text.find(" MiB; available ") == std::string::npos || text.back() != '\n') {
        std::fprintf(stderr, "expected SIGABRT and the report, got status %d and: %s\n", status, text.c_str());
        ++failures;
    }
    // Without the handler, the same request throws std::bad_alloc instead.
    text = run_child(
        [] {
            try {
                void *block = ::operator new(SIZE_MAX / 2);
                std::printf("%p\n", block);
            } catch (const std::bad_alloc &) {
                _exit(3);
            }
        },
        &status);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 3) {
        std::fprintf(stderr, "without the handler: expected bad_alloc, got status %d\n", status);
        ++failures;
    }
    return failures ? 1 : 0;
#endif
}
