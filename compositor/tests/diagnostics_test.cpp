// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "diagnostics.h"
}

#include "undervalgrind.h"

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>

TEST(ProcessResources, CountsDescriptorsAndReturnsToBaseline)
{
    struct vela_process_resources before{}, during{}, after{};
    ASSERT_TRUE(vela_process_resources(getpid(), &before));
    int event = eventfd(0, EFD_CLOEXEC);
    int pair[2];
    ASSERT_GE(event, 0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    EXPECT_TRUE(vela_process_resources(getpid(), &during));
    EXPECT_EQ(during.fds, before.fds + 3);
    EXPECT_EQ(during.eventfd, before.eventfd + 1);
    EXPECT_EQ(during.sockets, before.sockets + 2);
    struct rlimit limit;
    ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &limit), 0);
    // Valgrind keeps descriptors for itself above the limit it reports.
    if (!VELA_UNDER_VALGRIND()) {
        EXPECT_EQ(during.nofile, limit.rlim_cur);
    }
    EXPECT_GT(during.rss_kib, 0u);
    close(event);
    close(pair[0]);
    close(pair[1]);
    EXPECT_TRUE(vela_process_resources(getpid(), &after));
    EXPECT_EQ(after.fds, before.fds);
}

TEST(ProcessResources, UnavailableProcessDoesNotProduceInventedData)
{
    struct vela_process_resources sample{};
    sample.fds = 42;
    EXPECT_FALSE(vela_process_resources(-1, &sample));
    EXPECT_EQ(sample.fds, 0u);
}
