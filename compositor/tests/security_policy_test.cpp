// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "security_policy.h"
}

#include <gtest/gtest.h>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

TEST(SecurityPolicy, PrivilegedGlobals)
{
    for (const char *name : { "zwlr_screencopy_manager_v1", "ext_image_copy_capture_manager_v1",
             "ext_output_image_capture_source_manager_v1", "ext_foreign_toplevel_image_capture_source_manager_v1",
             "zwlr_output_manager_v1", "zwlr_data_control_manager_v1", "ext_data_control_manager_v1",
             "zwlr_virtual_pointer_manager_v1", "zwp_virtual_keyboard_manager_v1", "zwlr_layer_shell_v1",
             "ext_session_lock_manager_v1", "zwlr_foreign_toplevel_manager_v1", "ext_foreign_toplevel_list_v1",
             "vela_output_preview_manager_v1", "wp_security_context_manager_v1" }) {
        EXPECT_TRUE(vela_global_privileged(name)) << name;
    }
    for (const char *name : { "wl_compositor", "wl_shm", "wl_seat", "wl_output", "xdg_wm_base",
             "zwp_linux_dmabuf_v1", "wp_viewporter", "xdg_activation_v1", "zxdg_output_manager_v1",
             "zwlr_screencopy_manager_v1_extra", "zwlr_screencopy" }) {
        EXPECT_FALSE(vela_global_privileged(name)) << name;
    }
    EXPECT_TRUE(vela_global_privileged(nullptr));
}

TEST(SecurityPolicy, TestOnlyCommands)
{
    for (const char *line : { "test-output add 1920x1080", "test-output remove HEADLESS-2", "test-power off",
             "test-power on", "test-shell 1234", "test-power", "test-title-line-box on" }) {
        EXPECT_TRUE(vela_command_test_only(line)) << line;
    }
    for (const char *line :
        { "state", "lock", "test-outputs", "test-powerful", "test-title-line-boxes", "", "window active close" }) {
        EXPECT_FALSE(vela_command_test_only(line)) << line;
    }
    EXPECT_FALSE(vela_command_test_only(nullptr));
}

TEST(SecurityPolicy, PeerSandbox)
{
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    // The peer of a socketpair is this very process.
    if (access("/.flatpak-info", F_OK) != 0) {
        EXPECT_EQ(vela_peer_sandbox(pair[0], getpid()), VELA_SANDBOX_NONE);
    }
    // Outside our pid namespace, or no pid: never trusted.
    EXPECT_EQ(vela_peer_sandbox(pair[0], 0), VELA_SANDBOX_UNKNOWN);
    EXPECT_EQ(vela_peer_sandbox(pair[0], -1), VELA_SANDBOX_UNKNOWN);
    close(pair[0]);
    close(pair[1]);
}

TEST(SecurityPolicy, SystemPromptName)
{
    for (const char *name : { "pinentry", "org.gnupg.pinentry-qt", "kwalletd6", "org.kde.ksecretd",
             "org.kde.polkit-kde-authentication-agent-1", "gcr-prompter", "org.gnome.keyring.SystemPrompter",
             "PINENTRY-GTK" }) {
        EXPECT_TRUE(vela_system_prompt_name(name)) << name;
    }
    for (const char *name : { "", "org.mozilla.firefox", "vela-pattern", "keyring" }) {
        EXPECT_FALSE(vela_system_prompt_name(name)) << name;
    }
    EXPECT_FALSE(vela_system_prompt_name(nullptr));
}

TEST(SecurityPolicy, SystemPromptProgram)
{
    // Installed by the system, under the program's own name.
    for (const char *exe : { "/usr/bin/pinentry-qt", "/usr/bin/kwalletd6", "/usr/bin/ksecretd",
             "/usr/lib/polkit-kde-authentication-agent-1", "/usr/libexec/kf6/polkit-kde-authentication-agent-1",
             "/usr/libexec/gcr-prompter", "/usr/bin/pinentry-qt (deleted)" }) {
        EXPECT_TRUE(vela_system_prompt_program(exe, "/usr")) << exe;
    }
    // The right name anywhere the user can write, or the wrong program in /usr.
    for (const char *exe : { "/home/user/pinentry", "/opt/kwalletd6", "/usrx/bin/pinentry", "/usr/bin/firefox",
             "/usr/bin/../../opt/pinentry", "/usr/bin/pinentry-qt (copy)", "/usr/bin/", "pinentry", "" }) {
        EXPECT_FALSE(vela_system_prompt_program(exe, "/usr")) << exe;
    }
    EXPECT_FALSE(vela_system_prompt_program(nullptr, "/usr"));
    EXPECT_FALSE(vela_system_prompt_program("/usr/bin/pinentry", nullptr));
    EXPECT_FALSE(vela_system_prompt_program("/usr/bin/pinentry", "relative"));
    // A directory given by the tests, with or without its final slash.
    EXPECT_TRUE(vela_system_prompt_program("/home/test/prompts/pinentry-test", "/home/test/prompts/"));
    EXPECT_TRUE(vela_system_prompt_program("/home/test/prompts/pinentry-test", "/home/test/prompts"));
    EXPECT_FALSE(vela_system_prompt_program("/home/test/prompts2/pinentry-test", "/home/test/prompts"));
}

TEST(SecurityPolicy, PeerProgram)
{
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    char self[4096] = {};
    ASSERT_GT(readlink("/proc/self/exe", self, sizeof(self) - 1), 0);
    char exe[4096];
    // The peer of a socketpair is this very process.
    ASSERT_TRUE(vela_peer_program(pair[0], getpid(), exe, sizeof(exe)));
    EXPECT_STREQ(exe, self);
    // Another pid than the connection's, or none: unknown.
    EXPECT_FALSE(vela_peer_program(pair[0], getppid(), exe, sizeof(exe)));
    EXPECT_FALSE(vela_peer_program(pair[0], 0, exe, sizeof(exe)));
    // Too small to hold the path: refused, not truncated.
    char tiny[4];
    EXPECT_FALSE(vela_peer_program(pair[0], getpid(), tiny, sizeof(tiny)));
    close(pair[0]);
    close(pair[1]);
}

TEST(SecurityPolicy, PidNotTiedToTheConnectionIsUnknown)
{
    // A live, unsandboxed process with this pid exists (this one), but the
    // descriptor carries no peer pidfd that proves it is the peer: the case
    // of a reused pid, or of a kernel without SO_PEERPIDFD.
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);
    EXPECT_EQ(vela_peer_sandbox(pipe_fds[0], getpid()), VELA_SANDBOX_UNKNOWN);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    EXPECT_EQ(vela_peer_sandbox(-1, getpid()), VELA_SANDBOX_UNKNOWN);
}

TEST(SecurityPolicy, PidOfAnotherProcessIsUnknown)
{
    // The connection's peer is this process; the pid names another live,
    // unsandboxed one (as a reused pid would). Liveness of the real peer
    // must not vouch for it: only the pidfd's own pid is inspected.
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    pid_t other = fork();
    ASSERT_GE(other, 0);
    if (other == 0) {
        pause();
        _exit(0);
    }
    EXPECT_EQ(vela_peer_sandbox(pair[0], other), VELA_SANDBOX_UNKNOWN);
    kill(other, SIGKILL);
    waitpid(other, nullptr, 0);
    close(pair[0]);
    close(pair[1]);
}

TEST(SecurityPolicy, PeerGoneBeforeInspectionIsUnknown)
{
    // A peer that connects, sends and exits before it is inspected.
    int pair[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        close(pair[0]);
        int connected[2];
        // A fresh connection made by the child, handed back to the parent.
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, connected) != 0) {
            _exit(1);
        }
        char byte = 0;
        struct iovec iov = { &byte, 1 };
        char control[CMSG_SPACE(sizeof(int))] = {};
        struct msghdr message = {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &connected[0], sizeof(int));
        _exit(sendmsg(pair[1], &message, 0) == 1 ? 0 : 1);
    }
    close(pair[1]);
    char byte = 0;
    struct iovec iov = { &byte, 1 };
    char control[CMSG_SPACE(sizeof(int))] = {};
    struct msghdr message = {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ASSERT_EQ(recvmsg(pair[0], &message, 0), 1);
    int socket_fd = -1;
    memcpy(&socket_fd, CMSG_DATA(CMSG_FIRSTHDR(&message)), sizeof(int));
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    // Its credentials outlive it; its process does not.
    struct ucred peer = {};
    socklen_t length = sizeof(peer);
    ASSERT_EQ(getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &peer, &length), 0);
    EXPECT_EQ(peer.pid, child);
    EXPECT_EQ(vela_peer_sandbox(socket_fd, peer.pid), VELA_SANDBOX_UNKNOWN);
    close(socket_fd);
    close(pair[0]);
}

TEST(SecurityPolicy, ShmLayout)
{
    EXPECT_TRUE(vela_shm_layout_valid(100, 50, 400, 4));
    EXPECT_TRUE(vela_shm_layout_valid(100, 50, 512, 4)); // padded rows
    EXPECT_TRUE(vela_shm_layout_valid(1, 1, 4, 4));
    EXPECT_FALSE(vela_shm_layout_valid(100, 50, 100, 4)); // stride in pixels, not bytes
    EXPECT_FALSE(vela_shm_layout_valid(100, 50, 399, 4));
    EXPECT_FALSE(vela_shm_layout_valid(0, 50, 400, 4));
    EXPECT_FALSE(vela_shm_layout_valid(100, 0, 400, 4));
    EXPECT_FALSE(vela_shm_layout_valid(-1, 50, 400, 4));
    EXPECT_FALSE(vela_shm_layout_valid(100, 50, 400, 0));
    // The last byte read does not fit in size_t.
    EXPECT_FALSE(vela_shm_layout_valid(16, INT32_MAX, SIZE_MAX / 2, 4));
}

TEST(SecurityPolicy, DmabufLayout)
{
    const uint32_t offset[4] = { 0, 0, 0, 0 };
    const uint32_t stride[4] = { 1024 * 4, 0, 0, 0 };
    const uint64_t size = 1024ull * 4 * 768;
    EXPECT_TRUE(vela_dmabuf_layout_valid(1024, 768, 1, offset, stride, 4, size));
    EXPECT_TRUE(vela_dmabuf_layout_valid(1024, 768, 1, offset, stride, 4, 0)); // size unknown
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 769, 1, offset, stride, 4, size)); // one row too many
    const uint32_t late[4] = { 4096, 0, 0, 0 };
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 768, 1, late, stride, 4, size));
    const uint32_t narrow[4] = { 1023 * 4, 0, 0, 0 };
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 768, 1, offset, narrow, 4, 0));
    // 32-bit offset + stride * height: no wrap-around.
    const uint32_t huge_offset[4] = { UINT32_MAX, 0, 0, 0 };
    const uint32_t huge_stride[4] = { UINT32_MAX, 0, 0, 0 };
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 768, 1, huge_offset, huge_stride, 4, size));
    // Auxiliary planes start inside the buffer.
    const uint32_t aux_offset[4] = { 0, (uint32_t)size - 256, 0, 0 };
    const uint32_t aux_stride[4] = { 1024 * 4, 256, 0, 0 };
    EXPECT_TRUE(vela_dmabuf_layout_valid(1024, 767, 2, aux_offset, aux_stride, 4, size));
    const uint32_t aux_outside[4] = { 0, (uint32_t)size, 0, 0 };
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 767, 2, aux_outside, aux_stride, 4, size));
    const uint32_t aux_zero[4] = { 1024 * 4, 0, 0, 0 };
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 767, 2, aux_offset, aux_zero, 4, size));
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 768, 0, offset, stride, 4, size));
    EXPECT_FALSE(vela_dmabuf_layout_valid(1024, 768, 5, offset, stride, 4, size));
}
