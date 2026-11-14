// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "security_policy.h"
}

#include <gtest/gtest.h>
#include <cstdint>
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
             "test-power on", "test-shell 1234", "test-power" }) {
        EXPECT_TRUE(vela_command_test_only(line)) << line;
    }
    for (const char *line : { "state", "lock", "test-outputs", "test-powerful", "", "window active close" }) {
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
