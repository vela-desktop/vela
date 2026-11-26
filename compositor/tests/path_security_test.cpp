// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "path_security.h"
#include "config.h"
}

#include <gtest/gtest.h>
#include "privatetemp.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

TEST(PrivateFixtures, UsesPrivateBuildDirectoriesAndRejectsUnsafeRoots)
{
    const std::string root = vela::test::privateTempDirectory("fixture-security");
    const std::string first = vela::test::privateTempDirectory("child", root.c_str());
    const std::string second = vela::test::privateTempDirectory("child", root.c_str());
    EXPECT_NE(first, second);
    struct stat info {};
    ASSERT_EQ(lstat(first.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
    EXPECT_EQ(info.st_uid, geteuid());
    EXPECT_EQ(std::filesystem::path(root).parent_path(), VELA_TEST_PRIVATE_DIR);

    const std::string publicDir = root + "/public";
    ASSERT_EQ(mkdir(publicDir.c_str(), 0755), 0);
    ASSERT_EQ(chmod(publicDir.c_str(), 0755), 0);
    EXPECT_THROW(vela::test::privateTempDirectory("child", publicDir.c_str()), std::runtime_error);
    const std::string alias = root + "/alias";
    ASSERT_EQ(symlink(first.c_str(), alias.c_str()), 0);
    EXPECT_THROW(vela::test::privateTempDirectory("child", alias.c_str()), std::runtime_error);
    EXPECT_THROW(vela::test::privateTempDirectory("../escape", root.c_str()), std::invalid_argument);
    EXPECT_THROW(vela::test::privateTempDirectory("child", (root + "/missing/base").c_str()),
        std::system_error);
    EXPECT_TRUE(std::filesystem::is_empty(first));
    std::filesystem::remove_all(root);
}

TEST(PathSecurity, ValidatesOnePathComponent)
{
    EXPECT_TRUE(vela_path_component_safe("wayland-0"));
    EXPECT_TRUE(vela_path_component_safe("Tela Dark"));
    EXPECT_TRUE(vela_path_component_safe("breeze-dark"));
    for (const char *bad : { "", ".", "..", "../config", "theme/evil", "/abs", "a\nb" }) {
        EXPECT_FALSE(vela_path_component_safe(bad)) << bad;
    }
    EXPECT_FALSE(vela_path_component_safe(nullptr));
}

TEST(PathSecurity, AllowsIconSubdirectoriesButNotTraversal)
{
    EXPECT_TRUE(vela_relative_path_safe("48x48/apps"));
    EXPECT_TRUE(vela_relative_path_safe("scalable/actions/edit-copy"));
    for (const char *bad : { "", "/", "../icon", "a/../b", "a/./b", "a//b",
             "a/", "/usr/share/icons", "a/\nb" }) {
        EXPECT_FALSE(vela_relative_path_safe(bad)) << bad;
    }
}

TEST(PathSecurity, KeepsLockFlagsInsideRuntimeDirectory)
{
    char out[128];
    EXPECT_TRUE(vela_lock_flag_path_for_runtime("/run/user/1000", "wayland-0",
        out, sizeof(out)));
    EXPECT_STREQ(out, "/run/user/1000/vela-locked-wayland-0");
    EXPECT_FALSE(vela_lock_flag_path_for_runtime("/run/user/1000", "../secret",
        out, sizeof(out)));
    EXPECT_FALSE(vela_lock_flag_path_for_runtime("/run/user/1000", "/tmp/secret",
        out, sizeof(out)));
    EXPECT_FALSE(vela_lock_flag_path_for_runtime("/run/user/1000", "wayland/1",
        out, sizeof(out)));
    EXPECT_FALSE(vela_lock_flag_path_for_runtime("relative/dir", "wayland-0",
        out, sizeof(out)));
    EXPECT_FALSE(vela_lock_flag_path_for_runtime("/run/user/1000", "wayland-0",
        out, 8));
}

TEST(PathSecurity, RequiresPrivateOwnedRuntimeDirectory)
{
    std::string pattern = vela::test::privateTempDirectory("vela-runtime");
    EXPECT_TRUE(vela_runtime_dir_private(pattern.c_str()));
    EXPECT_TRUE(vela_runtime_dir_private((pattern + "/").c_str()));
    EXPECT_TRUE(vela_runtime_dir_private((pattern + "//").c_str()));
    EXPECT_TRUE(vela_runtime_dir_private((pattern + "/./").c_str()));
    int runtime_fd = vela_runtime_dir_open((pattern + "/").c_str());
    ASSERT_GE(runtime_fd, 0);
    close(runtime_fd);
    EXPECT_EQ(chmod(pattern.c_str(), 0755), 0);
    EXPECT_FALSE(vela_runtime_dir_private(pattern.c_str()));
    EXPECT_EQ(chmod(pattern.c_str(), 0700), 0);
    const std::string link = pattern + "-symlink";
    ASSERT_EQ(symlink(pattern.c_str(), link.c_str()), 0);
    EXPECT_FALSE(vela_runtime_dir_private(link.c_str()));
    EXPECT_FALSE(vela_runtime_dir_private((link + "/").c_str()));
    EXPECT_FALSE(vela_runtime_dir_private((pattern + "/../" +
        std::filesystem::path(pattern).filename().string()).c_str()));
    EXPECT_FALSE(vela_runtime_dir_private("relative"));
    EXPECT_FALSE(vela_runtime_dir_private(nullptr));
    unlink(link.c_str());
    rmdir(pattern.c_str());
}

TEST(PathSecurity, AllowsSymlinkInParentButNotRuntimeDirectory)
{
    std::string parent = vela::test::privateTempDirectory("vela-runtime-parent");
    const std::string runtime = parent + "/runtime";
    ASSERT_EQ(mkdir(runtime.c_str(), 0700), 0);
    const std::string link = parent + "-link";
    ASSERT_EQ(symlink(parent.c_str(), link.c_str()), 0);

    EXPECT_TRUE(vela_runtime_dir_private((link + "/runtime").c_str()));

    unlink(link.c_str());
    rmdir(runtime.c_str());
    rmdir(parent.c_str());
}

TEST(PathSecurity, RejectsConfigFilenameTraversal)
{
    char path[4096];
    EXPECT_FALSE(vela_config_path("../secrets", path, sizeof(path)));
    EXPECT_FALSE(vela_config_path("a/b", path, sizeof(path)));
    EXPECT_FALSE(vela_config_path("/etc/passwd", path, sizeof(path)));
    EXPECT_FALSE(vela_config_path("", path, sizeof(path)));
    EXPECT_TRUE(vela_config_path("vela.conf", path, sizeof(path)));
    EXPECT_TRUE(vela_config_path(nullptr, path, sizeof(path)));
}

TEST(PathSecurity, LockMarkerAtomicallyReplacesSymlinks)
{
    std::string pattern = vela::test::privateTempDirectory("vela-marker");
    int runtime_fd = vela_runtime_dir_open(pattern.c_str());
    ASSERT_GE(runtime_fd, 0);

    const std::string marker = pattern + "/vela-locked-wayland-0";
    const std::string sentinel = pattern + "/do-not-change";
    {
        // Closed on every return, the early ones of a failed ASSERT included.
        std::unique_ptr<FILE, int (*)(FILE *)> out(fopen(sentinel.c_str(), "w"), fclose);
        ASSERT_NE(out, nullptr);
        ASSERT_GT(fputs("unchanged", out.get()), 0);
    }
    ASSERT_EQ(symlink(sentinel.c_str(), marker.c_str()), 0);
    ASSERT_TRUE(vela_lock_marker_create_at(runtime_fd, "wayland-0"));
    EXPECT_TRUE(vela_lock_marker_present_at(runtime_fd, "wayland-0"));

    struct stat marker_info {};
    ASSERT_EQ(lstat(marker.c_str(), &marker_info), 0);
    EXPECT_TRUE(S_ISREG(marker_info.st_mode));
    EXPECT_EQ(marker_info.st_uid, geteuid());
    EXPECT_EQ(marker_info.st_mode & 0777, 0600);

    char content[16] = {};
    std::unique_ptr<FILE, int (*)(FILE *)> in(fopen(sentinel.c_str(), "r"), fclose);
    ASSERT_NE(in, nullptr);
    ASSERT_NE(fgets(content, sizeof(content), in.get()), nullptr);
    in.reset();
    EXPECT_STREQ(content, "unchanged");

    EXPECT_TRUE(vela_lock_marker_remove_at(runtime_fd, "wayland-0"));
    EXPECT_FALSE(vela_lock_marker_present_at(runtime_fd, "wayland-0"));
    close(runtime_fd);
    unlink(sentinel.c_str());
    rmdir(pattern.c_str());
}

TEST(PathSecurity, LockMarkerSurvivesRuntimeAliasRetarget)
{
    std::string root = vela::test::privateTempDirectory("vela-marker-alias");
    const std::string first = root + "/first";
    const std::string second = root + "/second";
    const std::string first_runtime = first + "/runtime";
    const std::string second_runtime = second + "/runtime";
    const std::string alias = root + "/parent";
    ASSERT_EQ(mkdir(first.c_str(), 0700), 0);
    ASSERT_EQ(mkdir(second.c_str(), 0700), 0);
    ASSERT_EQ(mkdir(first_runtime.c_str(), 0700), 0);
    ASSERT_EQ(mkdir(second_runtime.c_str(), 0700), 0);
    ASSERT_EQ(symlink(first.c_str(), alias.c_str()), 0);

    // The runtime directory itself is a real private directory; only a parent
    // component is an alias, which is an accepted XDG path shape.
    int runtime_fd = vela_runtime_dir_open((alias + "/runtime").c_str());
    ASSERT_GE(runtime_fd, 0);
    ASSERT_TRUE(vela_lock_marker_create_at(runtime_fd, "wayland-0"));

    ASSERT_EQ(unlink(alias.c_str()), 0);
    ASSERT_EQ(symlink(second.c_str(), alias.c_str()), 0);
    EXPECT_TRUE(vela_lock_marker_present_at(runtime_fd, "wayland-0"));
    EXPECT_TRUE(std::filesystem::exists(first_runtime + "/vela-locked-wayland-0"));
    EXPECT_FALSE(std::filesystem::exists(second_runtime + "/vela-locked-wayland-0"));

    EXPECT_TRUE(vela_lock_marker_remove_at(runtime_fd, "wayland-0"));
    EXPECT_FALSE(std::filesystem::exists(first_runtime + "/vela-locked-wayland-0"));
    close(runtime_fd);
    unlink(alias.c_str());
    rmdir(first_runtime.c_str());
    rmdir(second_runtime.c_str());
    rmdir(first.c_str());
    rmdir(second.c_str());
    rmdir(root.c_str());
}
