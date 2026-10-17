// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "supervisor_children.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Linux-only, like the supervisor: getdents64 + read avoid heap-backed
// DIR/FILE buffers while shutting down under memory pressure.
struct proc_dirent64 {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};

int vela_supervisor_children(int signal)
{
    int proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (proc < 0) {
        return -1;
    }
    pid_t self = getpid();
    int count = 0;
    _Alignas(8) char entries[8192];
    for (;;) {
        ssize_t size = syscall(SYS_getdents64, proc, entries, sizeof(entries));
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0) {
            close(proc);
            return -1;
        }
        if (size == 0) {
            break;
        }
        for (size_t offset = 0; offset < (size_t)size;) {
            struct proc_dirent64 *entry = (void *)(entries + offset);
            if (entry->reclen < offsetof(struct proc_dirent64, name) + 2
                || offset + entry->reclen > (size_t)size) {
                close(proc);
                return -1;
            }
            offset += entry->reclen;
            pid_t pid = (pid_t)atoi(entry->name);
            if (pid <= 0) {
                continue;
            }
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
            int stat_fd = open(path, O_RDONLY | O_CLOEXEC);
            if (stat_fd < 0) {
                continue; // exited during enumeration
            }
            char line[1024];
            ssize_t length;
            do {
                length = read(stat_fd, line, sizeof(line) - 1);
            } while (length < 0 && errno == EINTR);
            close(stat_fd);
            if (length <= 0) {
                continue;
            }
            line[length] = '\0';
            // /proc/PID/stat: "pid (name) state ppid ..."; names may contain ')'.
            const char *end = strrchr(line, ')');
            char state = 0;
            int parent = 0;
            if (!end || sscanf(end + 1, " %c %d", &state, &parent) != 2 || parent != self || state == 'Z') {
                continue;
            }
            ++count;
            if (signal != 0) {
                (void)kill(pid, signal);
            }
        }
    }
    close(proc);
    return count;
}
