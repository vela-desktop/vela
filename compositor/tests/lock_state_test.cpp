// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "lock_state.h"
#include "path_security.h"
}

#include <gtest/gtest.h>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool fd_open(int fd)
{
    return fcntl(fd, F_GETFD) >= 0 || errno != EBADF;
}

struct TempDir {
    std::string path;
    TempDir()
    {
        path = (std::filesystem::temp_directory_path() / "vela-lock-state-XXXXXX").string();
        if (!mkdtemp(path.data())) {
            path.clear();
        }
    }
    ~TempDir()
    {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

} // namespace

TEST(LockState, CreatedPageIsSealedUnlockedAndCloseOnExec)
{
    int fd = vela_lock_state_create();
    ASSERT_GE(fd, 0);
    EXPECT_NE(fcntl(fd, F_GETFD) & FD_CLOEXEC, 0);
    int seals = fcntl(fd, F_GET_SEALS);
    ASSERT_GE(seals, 0);
    EXPECT_NE(seals & F_SEAL_SHRINK, 0);
    EXPECT_NE(seals & F_SEAL_GROW, 0);
    EXPECT_NE(seals & F_SEAL_SEAL, 0);
    // The sealed size cannot change under a mapping.
    EXPECT_NE(ftruncate(fd, 0), 0);
    EXPECT_NE(ftruncate(fd, 2 * sysconf(_SC_PAGESIZE)), 0);

    struct vela_lock_state *state = vela_lock_state_map(fd);
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(vela_lock_state_locked(state));
    vela_lock_state_unmap(state);
}

TEST(LockState, MapClosesTheDescriptorAndValuePersistsAcrossCompositors)
{
    int supervisor_fd = vela_lock_state_create();
    ASSERT_GE(supervisor_fd, 0);

    // First compositor: its own descriptor (as after fork + exec).
    int first_fd = dup(supervisor_fd);
    ASSERT_GE(first_fd, 0);
    struct vela_lock_state *first = vela_lock_state_map(first_fd);
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(fd_open(first_fd)) << "no child may inherit a writable view";
    vela_lock_state_store(first, true);
    vela_lock_state_unmap(first); // the first compositor crashes

    // The restarted compositor sees the value the crashed one stored.
    int second_fd = dup(supervisor_fd);
    ASSERT_GE(second_fd, 0);
    struct vela_lock_state *second = vela_lock_state_map(second_fd);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(vela_lock_state_locked(second));
    vela_lock_state_store(second, false);
    EXPECT_FALSE(vela_lock_state_locked(second));
    vela_lock_state_unmap(second);
    close(supervisor_fd);
}

TEST(LockState, AnyValueOtherThanUnlockedReadsLocked)
{
    int fd = vela_lock_state_create();
    ASSERT_GE(fd, 0);
    const unsigned garbage = 0xdeadbeefu;
    ASSERT_EQ(pwrite(fd, &garbage, sizeof(garbage), 0), (ssize_t)sizeof(garbage));
    struct vela_lock_state *state = vela_lock_state_map(fd);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(vela_lock_state_locked(state));
    vela_lock_state_unmap(state);
}

TEST(LockState, NullPageIsUnlockedAndStoresAreIgnored)
{
    EXPECT_FALSE(vela_lock_state_locked(nullptr));
    vela_lock_state_store(nullptr, true);
    vela_lock_state_unmap(nullptr);
}

TEST(LockState, RejectsAnythingButASealedOnePageMemfd)
{
    long page = sysconf(_SC_PAGESIZE);
    EXPECT_EQ(vela_lock_state_map(-1), nullptr);

    // A regular file of the right size: not sealable.
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    std::string file = dir.path + "/page";
    int regular = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_GE(regular, 0);
    ASSERT_EQ(ftruncate(regular, page), 0);
    EXPECT_EQ(vela_lock_state_map(regular), nullptr);
    EXPECT_TRUE(fd_open(regular)) << "a rejected descriptor is left to the caller";
    close(regular);

    // A memfd without the size seals.
    int unsealed = memfd_create("unsealed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ASSERT_GE(unsealed, 0);
    ASSERT_EQ(ftruncate(unsealed, page), 0);
    EXPECT_EQ(vela_lock_state_map(unsealed), nullptr);
    close(unsealed);

    // Sealed, but not one page.
    for (off_t size : { (off_t)0, (off_t)page / 2, (off_t)page * 2 }) {
        int wrong = memfd_create("wrong-size", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        ASSERT_GE(wrong, 0);
        ASSERT_EQ(ftruncate(wrong, size), 0);
        ASSERT_EQ(fcntl(wrong, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW), 0);
        EXPECT_EQ(vela_lock_state_map(wrong), nullptr) << size;
        close(wrong);
    }

    // Write-sealed: the mapping cannot be writable.
    int read_only = memfd_create("read-only", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ASSERT_GE(read_only, 0);
    ASSERT_EQ(ftruncate(read_only, page), 0);
    ASSERT_EQ(fcntl(read_only, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE), 0);
    EXPECT_EQ(vela_lock_state_map(read_only), nullptr);
    close(read_only);

    // A pipe or socket number, as a mistyped descriptor would be.
    int pipe_fds[2];
    ASSERT_EQ(pipe2(pipe_fds, O_CLOEXEC), 0);
    EXPECT_EQ(vela_lock_state_map(pipe_fds[0]), nullptr);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST(LockState, ParsesOnlyPlainDescriptorNumbers)
{
    EXPECT_EQ(vela_lock_state_parse_fd("0"), 0);
    EXPECT_EQ(vela_lock_state_parse_fd("17"), 17);
    for (const char *bad : { "", "-1", "+3", " 3", "3 ", "3x", "0x10", "99999999999", "abc" }) {
        EXPECT_EQ(vela_lock_state_parse_fd(bad), -1) << bad;
    }
    EXPECT_EQ(vela_lock_state_parse_fd(nullptr), -1);
}

TEST(LockState, StartsUnlockedOnlyWhenPageAndMarkerBothSayUnlocked)
{
    EXPECT_FALSE(vela_lock_start_locked(false, VELA_LOCK_MARKER_ABSENT));
    EXPECT_TRUE(vela_lock_start_locked(false, VELA_LOCK_MARKER_PRESENT));
    EXPECT_TRUE(vela_lock_start_locked(false, VELA_LOCK_MARKER_UNKNOWN));
    EXPECT_TRUE(vela_lock_start_locked(true, VELA_LOCK_MARKER_ABSENT));
    EXPECT_TRUE(vela_lock_start_locked(true, VELA_LOCK_MARKER_PRESENT));
    EXPECT_TRUE(vela_lock_start_locked(true, VELA_LOCK_MARKER_UNKNOWN));
}

TEST(LockMarker, OnlyAMissingEntryIsAbsent)
{
    TempDir dir;
    ASSERT_FALSE(dir.path.empty());
    int runtime_fd = vela_runtime_dir_open(dir.path.c_str());
    ASSERT_GE(runtime_fd, 0);
    const std::string marker = dir.path + "/vela-locked-wayland-0";

    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, "wayland-0"), VELA_LOCK_MARKER_ABSENT);
    EXPECT_FALSE(vela_lock_marker_present_at(runtime_fd, "wayland-0"));

    ASSERT_TRUE(vela_lock_marker_create_at(runtime_fd, "wayland-0"));
    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, "wayland-0"), VELA_LOCK_MARKER_PRESENT);

    // A marker that is not a private regular file is not proof of "unlocked".
    ASSERT_EQ(chmod(marker.c_str(), 0644), 0);
    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, "wayland-0"), VELA_LOCK_MARKER_UNKNOWN);
    EXPECT_TRUE(vela_lock_marker_present_at(runtime_fd, "wayland-0"));
    ASSERT_TRUE(vela_lock_marker_remove_at(runtime_fd, "wayland-0"));
    ASSERT_EQ(mkdir(marker.c_str(), 0700), 0);
    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, "wayland-0"), VELA_LOCK_MARKER_UNKNOWN);
    ASSERT_EQ(rmdir(marker.c_str()), 0);
    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, "wayland-0"), VELA_LOCK_MARKER_ABSENT);

    // An fstatat error other than ENOENT (here ENOTDIR: the descriptor is a
    // file, not a directory) is unknown, never absent.
    std::string file = dir.path + "/not-a-directory";
    int not_directory = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_GE(not_directory, 0);
    EXPECT_EQ(vela_lock_marker_state_at(not_directory, "wayland-0"), VELA_LOCK_MARKER_UNKNOWN);
    close(not_directory);

    // No descriptor, or a display that is not one path component.
    EXPECT_EQ(vela_lock_marker_state_at(-1, "wayland-0"), VELA_LOCK_MARKER_UNKNOWN);
    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, "../wayland-0"), VELA_LOCK_MARKER_UNKNOWN);
    EXPECT_EQ(vela_lock_marker_state_at(runtime_fd, nullptr), VELA_LOCK_MARKER_UNKNOWN);
    close(runtime_fd);
}
