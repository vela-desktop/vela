// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "path_security.h"
#include "config.h"
}

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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
    std::string pattern = (std::filesystem::temp_directory_path() / "vela-runtime-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
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
    std::string pattern = (std::filesystem::temp_directory_path() / "vela-marker-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    const std::string marker = pattern + "/vela-locked-wayland-0";
    const std::string sentinel = pattern + "/do-not-change";
    {
        FILE *out = fopen(sentinel.c_str(), "w");
        ASSERT_NE(out, nullptr);
        ASSERT_GT(fputs("unchanged", out), 0);
        fclose(out);
    }
    ASSERT_EQ(symlink(sentinel.c_str(), marker.c_str()), 0);
    ASSERT_TRUE(vela_lock_marker_create(marker.c_str()));
    struct stat marker_info {};
    ASSERT_EQ(lstat(marker.c_str(), &marker_info), 0);
    EXPECT_TRUE(S_ISREG(marker_info.st_mode));
    EXPECT_EQ(marker_info.st_uid, geteuid());
    EXPECT_EQ(marker_info.st_mode & 0777, 0600);
    char content[16] = {};
    FILE *in = fopen(sentinel.c_str(), "r");
    ASSERT_NE(in, nullptr);
    ASSERT_NE(fgets(content, sizeof(content), in), nullptr);
    fclose(in);
    EXPECT_STREQ(content, "unchanged");
    unlink(marker.c_str());
    unlink(sentinel.c_str());
    rmdir(pattern.c_str());
}
