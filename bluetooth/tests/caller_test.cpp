// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Who may call (caller.h): sandboxes on a fake /proc, Vela's executables,
// and a pidfd's process.

extern "C" {
#include "caller.h"
}

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <sys/syscall.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

class Sandbox : public testing::Test {
protected:
    fs::path proc;

    void SetUp() override
    {
        std::string pattern = ::testing::TempDir() + "vela-bluetooth-proc-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        proc = pattern;
        process(100, "0::/user.slice/user-1000.slice/session-2.scope\n");
        process(200, "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-org.example.App-1.scope\n");
        std::ofstream(proc / "200" / "root" / ".flatpak-info") << "[Application]\nname=org.example.App\n";
        process(300, "0::/user.slice/user-1000.slice/user@1000.service/app.slice/snap.firefox.firefox-12.scope\n");
        process(400, "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-snapper.scope\n");
    }

    void TearDown() override { fs::remove_all(proc); }

    void process(int pid, const std::string& cgroup)
    {
        fs::create_directories(proc / std::to_string(pid) / "root");
        std::ofstream(proc / std::to_string(pid) / "cgroup") << cgroup;
    }
};

} // namespace

TEST_F(Sandbox, FlatpakAndSnapAreSandboxed)
{
    EXPECT_FALSE(vela_bt_sandboxed(proc.c_str(), 100));
    EXPECT_TRUE(vela_bt_sandboxed(proc.c_str(), 200));
    EXPECT_TRUE(vela_bt_sandboxed(proc.c_str(), 300));
    EXPECT_FALSE(vela_bt_sandboxed(proc.c_str(), 400));
    // A process that can't be read isn't trusted.
    EXPECT_TRUE(vela_bt_sandboxed(proc.c_str(), 999));
    // ELOOP is not ENOENT: an unreadable/invalid Flatpak marker is unknown,
    // not proof that the process is unsandboxed.
    process(500, "0::/user.slice/session-2.scope\n");
    fs::create_symlink(".flatpak-info", proc / "500" / "root" / ".flatpak-info");
    EXPECT_TRUE(vela_bt_sandboxed(proc.c_str(), 500));
}

TEST(Caller, VelasExecutables)
{
    const char* const allowed[] = { "/usr/bin/vela-shell", "/usr/bin/vela-settings", nullptr };
    EXPECT_TRUE(vela_bt_exe_allowed("/usr/bin/vela-shell", allowed));
    EXPECT_TRUE(vela_bt_exe_allowed("/usr/bin/vela-settings", allowed));
    // Replaced by an update while it runs.
    EXPECT_TRUE(vela_bt_exe_allowed("/usr/bin/vela-shell (deleted)", allowed));
    EXPECT_FALSE(vela_bt_exe_allowed("/usr/bin/vela-shell2", allowed));
    EXPECT_FALSE(vela_bt_exe_allowed("/usr/bin/vela-shel", allowed));
    EXPECT_FALSE(vela_bt_exe_allowed("/home/user/vela-shell", allowed));
    EXPECT_FALSE(vela_bt_exe_allowed("/usr/bin/python3", allowed));
    EXPECT_FALSE(vela_bt_exe_allowed("/usr/bin/vela-shell", nullptr));
}

TEST(Caller, PidfdNamesItsProcess)
{
    int pidfd = int(syscall(SYS_pidfd_open, getpid(), 0));
    if (pidfd < 0) {
        GTEST_SKIP() << "no pidfd_open";
    }
    EXPECT_EQ(vela_bt_pidfd_pid("/proc", pidfd), getpid());
    close(pidfd);
    EXPECT_EQ(vela_bt_pidfd_pid("/proc", pidfd), -1);
}
