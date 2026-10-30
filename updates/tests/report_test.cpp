// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The pure parts of vela-update-daemon (report.h): what vela-update
// reports, Install's options, the terminal, and the session's compositor,
// on a fake /proc and a fake PATH.

extern "C" {
#include "report.h"
}

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

class Files : public testing::Test {
protected:
    fs::path root;

    void SetUp() override
    {
        std::string pattern = ::testing::TempDir() + "vela-update-test-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
    }

    void TearDown() override { fs::remove_all(root); }

    std::string write(const std::string& name, const std::string& content)
    {
        fs::path path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << content;
        return path.string();
    }

    // A process in the fake /proc: its name, parent, start time and
    // executable.
    void process(int pid, const std::string& name, int parent, unsigned long start, const std::string& exe)
    {
        std::string stat = std::to_string(pid) + " (" + name + ") S " + std::to_string(parent);
        // Fields 5 to 21, then the start time (22).
        for (int field = 5; field <= 21; ++field) {
            stat += " 0";
        }
        stat += " " + std::to_string(start) + " 0 0\n";
        write("proc/" + std::to_string(pid) + "/stat", stat);
        fs::create_symlink(exe, root / "proc" / std::to_string(pid) / "exe");
    }

    std::string proc() const { return (root / "proc").string(); }

    void program(const std::string& dir, const std::string& name)
    {
        std::string path = write(dir + "/" + name, "#!/bin/sh\n");
        fs::permissions(path, fs::perms::owner_all);
    }
};

const char* const updateAvailable = R"({
  "schema_version": 2, "channel": "experimental", "status": "update-available",
  "installed_package": "vela-git", "installed_version": "0.1.0.r1.gabcdef0-1",
  "version": "0.1.0", "tag": "", "commit": "1111111111111111111111111111111111111111",
  "source": "https://github.com/vela-desktop/vela.git",
  "changes": [{"commit": "1111111111111111111111111111111111111111", "summary": "Fix the taskbar"},
              {"commit": "2222222222222222222222222222222222222222", "summary": "Faster blur"}],
  "changes_total": 140, "notes": [], "checked_at": 1793289600, "error": null
})";

} // namespace

TEST(Check, ParsesSchema2)
{
    vela_up_check check;
    ASSERT_EQ(vela_up_check_parse(updateAvailable, &check), 0);
    EXPECT_TRUE(check.valid);
    EXPECT_STREQ(check.channel, "experimental");
    EXPECT_STREQ(check.status, "update-available");
    EXPECT_STREQ(check.installed_package, "vela-git");
    EXPECT_STREQ(check.tag, "");
    ASSERT_EQ(check.change_count, 2u);
    EXPECT_STREQ(check.changes[1].first, "2222222222222222222222222222222222222222");
    EXPECT_STREQ(check.changes[1].second, "Faster blur");
    EXPECT_EQ(check.changes_total, 140u);
    EXPECT_EQ(check.note_count, 0u);
    EXPECT_EQ(check.checked_at, 1793289600);
    EXPECT_STREQ(check.error_code, "");
    vela_up_check_clear(&check);
}

TEST(Check, BetaNotesAndErrors)
{
    vela_up_check check;
    ASSERT_EQ(vela_up_check_parse(R"({"schema_version": 2, "channel": "beta", "status": "update-available",
        "notes": [{"version": "0.1.11", "text": "Bluetooth"}, 7, {"version": "0.1.10"}]})", &check), 0);
    // Entries that aren't objects are skipped; missing strings are "".
    ASSERT_EQ(check.note_count, 2u);
    EXPECT_STREQ(check.notes[0].second, "Bluetooth");
    EXPECT_STREQ(check.notes[1].first, "0.1.10");
    EXPECT_STREQ(check.notes[1].second, "");
    vela_up_check_clear(&check);

    ASSERT_EQ(vela_up_check_parse(R"({"schema_version": 2, "status": "error",
        "error": {"code": "rate-limited", "message": "HTTP 403"}})", &check), 0);
    EXPECT_STREQ(check.error_code, "rate-limited");
    EXPECT_STREQ(check.error_message, "HTTP 403");
    vela_up_check_clear(&check);

    // A failure is a failure even without its code.
    ASSERT_EQ(vela_up_check_parse(R"({"schema_version": 2, "status": "error"})", &check), 0);
    EXPECT_STREQ(check.error_code, "failed");
    vela_up_check_clear(&check);
}

TEST(Check, RefusesOtherSchemasAndGarbage)
{
    vela_up_check check;
    EXPECT_LT(vela_up_check_parse(R"({"schema_version": 1, "status": "up-to-date"})", &check), 0);
    EXPECT_FALSE(check.valid);
    EXPECT_LT(vela_up_check_parse("[2]", &check), 0);
    EXPECT_LT(vela_up_check_parse("vela-update: no", &check), 0);
    EXPECT_LT(vela_up_check_parse("", &check), 0);
    EXPECT_LT(vela_up_check_read("/nonexistent/check.json", &check), 0);
}

TEST_F(Files, RunAndInstall)
{
    vela_up_run run;
    ASSERT_EQ(vela_up_run_read(write("run.json", R"({"schema_version": 2, "action": "install",
        "channel": "beta", "phase": "verifying", "pid": 4242, "start_time": 777, "started_at": 5})").c_str(),
                  &run),
        0);
    EXPECT_TRUE(run.present);
    EXPECT_STREQ(run.action, "install");
    EXPECT_STREQ(run.phase, "verifying");
    EXPECT_EQ(run.pid, 4242);
    EXPECT_EQ(run.start_time, 777u);

    // Alive: same PID and start time; a reused PID is another process.
    process(4242, "sh", 1, 777, "/usr/bin/bash");
    EXPECT_TRUE(vela_up_run_alive(proc().c_str(), &run));
    run.start_time = 778;
    EXPECT_FALSE(vela_up_run_alive(proc().c_str(), &run));
    run.pid = 4243;
    EXPECT_FALSE(vela_up_run_alive(proc().c_str(), &run));
    vela_up_run_clear(&run);

    vela_up_install install;
    ASSERT_EQ(vela_up_install_read(write("install.json", R"({"schema_version": 2, "channel": "experimental",
        "result": "failed", "error": {"code": "build-failed", "message": "the update failed"},
        "installed_version": "0.1.0.r1.gabcdef0-1", "finished_at": 9})").c_str(),
                  &install),
        0);
    EXPECT_STREQ(install.result, "failed");
    EXPECT_STREQ(install.error_code, "build-failed");
    EXPECT_EQ(install.finished_at, 9);
    vela_up_install_clear(&install);
    EXPECT_LT(vela_up_install_read(write("old.json", R"({"result": "success"})").c_str(), &install), 0);
}

TEST_F(Files, StartTimeSurvivesOddNames)
{
    // ")" and spaces in the name: fields are counted after the last ")".
    process(10, "a) b (c", 1, 123456, "/usr/bin/x");
    EXPECT_EQ(vela_up_start_time(proc().c_str(), 10), 123456u);
    EXPECT_EQ(vela_up_start_time(proc().c_str(), 11), 0u);
}

TEST(Options, LikeVelaUpdate)
{
    vela_up_options none {};
    EXPECT_EQ(vela_up_options_invalid(&none, nullptr), nullptr);
    vela_up_options beta { "beta", nullptr, nullptr, nullptr, "v0.1.11" };
    EXPECT_EQ(vela_up_options_invalid(&beta, "experimental"), nullptr);
    vela_up_options branch { nullptr, "fix/nvidia", nullptr, nullptr, nullptr };
    EXPECT_EQ(vela_up_options_invalid(&branch, "experimental"), nullptr);
    EXPECT_EQ(vela_up_options_invalid(&branch, nullptr), nullptr);
    // Experimental's options on Beta, and the other way round.
    EXPECT_NE(vela_up_options_invalid(&branch, "beta"), nullptr);
    vela_up_options tag { "experimental", nullptr, nullptr, nullptr, "v0.1.2" };
    EXPECT_NE(vela_up_options_invalid(&tag, nullptr), nullptr);

    const std::vector<vela_up_options> invalid = {
        { "stable", nullptr, nullptr, nullptr, nullptr },
        { nullptr, "main", "abcd", nullptr, nullptr },
        { nullptr, "-x", nullptr, nullptr, nullptr },
        { nullptr, "a..b", nullptr, nullptr, nullptr },
        { nullptr, "a b", nullptr, nullptr, nullptr },
        { nullptr, "x.lock", nullptr, nullptr, nullptr },
        { nullptr, nullptr, "abc", nullptr, nullptr },
        { nullptr, nullptr, "xyz1234", nullptr, nullptr },
        { nullptr, nullptr, nullptr, "--upload-pack=x", nullptr },
        { nullptr, nullptr, nullptr, "a\nb", nullptr },
        { "beta", nullptr, nullptr, nullptr, "v1.0.0" },
        { "beta", nullptr, nullptr, nullptr, "v0.1" },
        { "beta", nullptr, nullptr, nullptr, "v0.1.2-rc1" },
    };
    for (const vela_up_options& options : invalid) {
        EXPECT_NE(vela_up_options_invalid(&options, nullptr), nullptr)
            << (options.branch ? options.branch : "") << (options.commit ? options.commit : "")
            << (options.source ? options.source : "") << (options.tag ? options.tag : "");
    }
}

TEST(Options, Arguments)
{
    const char* argv[12];
    char tag[64];
    vela_up_options experimental { "experimental", nullptr, "abcdef0", "/home/me/vela", nullptr };
    vela_up_options_argv(&experimental, "vela-update", argv, tag);
    std::vector<std::string> got;
    for (int i = 0; argv[i]; ++i) {
        got.emplace_back(argv[i]);
    }
    EXPECT_EQ(got, (std::vector<std::string> { "vela-update", "--channel", "experimental", "--commit", "abcdef0",
                       "--source", "/home/me/vela" }));
    // The Beta helper takes the tag only as one word.
    vela_up_options beta { "beta", nullptr, nullptr, nullptr, "v0.1.9" };
    vela_up_options_argv(&beta, "vela-update", argv, tag);
    EXPECT_STREQ(argv[3], "--tag=v0.1.9");
    EXPECT_EQ(argv[4], nullptr);
}

TEST_F(Files, Terminal)
{
    program("bin1", "konsole");
    program("bin2", "kitty");
    const std::string path = (root / "bin1").string() + ":" + (root / "bin2").string();

    // konsole -e without anything else.
    char** argv = vela_up_terminal(path.c_str(), nullptr);
    ASSERT_NE(argv, nullptr);
    EXPECT_EQ(std::string(argv[0]), (root / "bin1/konsole").string());
    EXPECT_STREQ(argv[1], "-e");
    EXPECT_EQ(argv[2], nullptr);
    vela_up_strv_free(argv);

    // VELA_TERMINAL, with its own options.
    argv = vela_up_terminal(path.c_str(), "kitty  --single-instance");
    ASSERT_NE(argv, nullptr);
    EXPECT_EQ(std::string(argv[0]), (root / "bin2/kitty").string());
    EXPECT_STREQ(argv[1], "--single-instance");
    EXPECT_STREQ(argv[2], "-e");
    EXPECT_EQ(argv[3], nullptr);
    vela_up_strv_free(argv);

    // At most 64 words.
    std::string words = "kitty";
    for (int i = 1; i < 64; ++i) {
        words += " -1";
    }
    argv = vela_up_terminal(path.c_str(), words.c_str());
    ASSERT_NE(argv, nullptr);
    EXPECT_STREQ(argv[64], "-e");
    vela_up_strv_free(argv);
    EXPECT_EQ(vela_up_terminal(path.c_str(), (words + " -1").c_str()), nullptr);

    // xdg-terminal-exec wins, and takes the command as it is.
    program("bin2", "xdg-terminal-exec");
    argv = vela_up_terminal(path.c_str(), "kitty");
    ASSERT_NE(argv, nullptr);
    EXPECT_EQ(std::string(argv[0]), (root / "bin2/xdg-terminal-exec").string());
    EXPECT_EQ(argv[1], nullptr);
    vela_up_strv_free(argv);

    // Nothing installed.
    EXPECT_EQ(vela_up_terminal((root / "empty").c_str(), "foot"), nullptr);
}

TEST_F(Files, SessionCompositorAndReplacedExecutable)
{
    // daemon (300) < sh (200) < compositor (100) < supervisor (50)
    process(50, "vela-compositor", 1, 1, "/usr/bin/vela-compositor");
    process(100, "vela-compositor", 50, 2, "/usr/bin/vela-compositor");
    process(200, "sh", 100, 3, "/usr/bin/bash");
    process(300, "vela-update-dae", 200, 4, "/usr/lib/vela/vela-update-daemon");
    EXPECT_EQ(vela_up_session_process(proc().c_str(), 300), 100);
    EXPECT_FALSE(vela_up_exe_replaced(proc().c_str(), 100));

    // A package upgrade replaced the compositor's executable.
    process(110, "vela-compositor", 50, 2, "/usr/bin/vela-compositor (deleted)");
    process(310, "vela-update-dae", 110, 4, "/usr/lib/vela/vela-update-daemon");
    EXPECT_EQ(vela_up_session_process(proc().c_str(), 310), 110);
    EXPECT_TRUE(vela_up_exe_replaced(proc().c_str(), 110));

    // Not in a Vela session.
    process(400, "bash", 1, 5, "/usr/bin/bash");
    EXPECT_EQ(vela_up_session_process(proc().c_str(), 400), 0);
    EXPECT_FALSE(vela_up_exe_replaced(proc().c_str(), 0));
}
