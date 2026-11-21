// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The terminal lookup (docs/keyboard.md §11): preference priority and
// integration through the packaged launcher in a private tree.

#include "terminal.h"
#include "privatetemp.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

class Terminal : public ::testing::Test {
protected:
    fs::path root;
    std::string path;
    std::string configHome;
    std::string dataHome;
    std::string variable;

    void SetUp() override
    {
        root = vela::test::privateTempDirectory("terminal");
        path = (root / "bin").string();
        configHome = (root / "config").string();
        dataHome = (root / "data").string();
        fs::create_directories(root / "bin");
        fs::create_directories(root / "config");
    }

    void TearDown() override { fs::remove_all(root); }

    void program(const std::string& name)
    {
        const fs::path file = root / "bin" / name;
        std::ofstream(file) << "#!/bin/sh\n";
        fs::permissions(file, fs::perms::owner_all);
    }

    void write(const fs::path& file, const std::string& text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file) << text;
    }

    vela_terminal_env env() const
    {
        vela_terminal_env e {};
        e.home = root.c_str();
        e.path = path.c_str();
        e.vela_terminal = variable.c_str();
        e.config_home = configHome.c_str();
        return e;
    }

    // What runs, as one string: "source id | word word...".
    std::string run(const std::vector<const char*>& command = {})
    {
        const vela_terminal_env e = env();
        vela_terminal t;
        if (!vela_terminal_find(&e, &t)) {
            return "none";
        }
        std::vector<const char*> words(command);
        words.push_back(nullptr);
        char** argv = vela_terminal_argv(&t, words.data());
        std::string out = std::to_string(t.source) + " " + (t.id ? t.id : "-") + " |";
        for (char** word = argv; *word; ++word) {
            std::string w = *word;
            const std::string bin = (root / "bin").string() + "/";
            if (w.rfind(bin, 0) == 0) {
                w = w.substr(bin.size());
            }
            out += " " + w;
        }
        vela_terminal_strv_free(argv);
        vela_terminal_free(&t);
        return out;
    }
};

const std::vector<const char*> command { "sh", "-c", "echo hi" };

} // namespace

TEST_F(Terminal, NothingInstalled)
{
    EXPECT_EQ(run(), "none");
}

TEST_F(Terminal, XdgTerminalExec)
{
    program("xdg-terminal-exec");
    EXPECT_EQ(run(), "3 - | xdg-terminal-exec");
    EXPECT_EQ(run(command), "3 - | xdg-terminal-exec sh -c echo hi");
}

TEST_F(Terminal, Variable)
{
    program("kitty");
    program("xdg-terminal-exec");
    variable = "kitty  --single-instance";
    EXPECT_EQ(run(command), "2 - | kitty --single-instance -e sh -c echo hi");
    EXPECT_EQ(run(), "2 - | kitty --single-instance");
    // A full path.
    variable = (root / "bin/kitty").string();
    EXPECT_EQ(run(), "2 - | kitty");
    // At most 64 words.
    variable = "kitty";
    for (int i = 1; i < 64; ++i) {
        variable += " -1";
    }
    EXPECT_NE(run().find("2 - | kitty -1"), std::string::npos);
    variable += " -1";
    EXPECT_EQ(run(), "3 - | xdg-terminal-exec");
    // Missing, or only spaces: the next step.
    variable = "missing";
    EXPECT_EQ(run(), "3 - | xdg-terminal-exec");
    variable = "   ";
    EXPECT_EQ(run(), "3 - | xdg-terminal-exec");
}

TEST_F(Terminal, UserChoice)
{
    program("kitty");
    program("xdg-terminal-exec");
    variable = "kitty";
    const fs::path list = fs::path(configHome) / "vela-xdg-terminals.list";
    write(list, "# Settings\n/execarg_default:b.desktop:-x\n-a.desktop\n+b.desktop\n");
    EXPECT_EQ(run(), "2 - | kitty");
    write(list, "# Settings\nexample.desktop:profile\n");
    EXPECT_EQ(run(command), "1 example.desktop | xdg-terminal-exec sh -c echo hi");
}

TEST_F(Terminal, LauncherReceivesOriginalProfileAndArguments)
{
    const char* binary = getenv("VELA_TERMINAL_PROGRAM");
    ASSERT_NE(binary, nullptr);
    const char* originalPath = getenv("PATH");
    ASSERT_NE(originalPath, nullptr);
    if (system("command -v xdg-terminal-exec >/dev/null") != 0) {
        GTEST_SKIP() << "xdg-terminal-exec is required for the integration test";
    }
    const fs::path term = root / "bin/term";
    write(term, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$HOME/ran\"\n");
    fs::permissions(term, fs::perms::owner_all);
    write(fs::path(dataHome) / "applications/example.desktop",
        "[Desktop Entry]\nType=Application\nName=Term\nCategories=TerminalEmulator;\nExec=term main\n"
        "Actions=profile;\nX-TerminalArgExec=--\n[Desktop Action profile]\nName=Work\nExec=term work-profile\n");
    write(fs::path(configHome) / "vela-xdg-terminals.list", "example.desktop:profile\n");
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (!child) {
        setenv("HOME", root.c_str(), 1);
        setenv("PATH", (path + ":" + originalPath).c_str(), 1);
        setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
        setenv("XDG_DATA_HOME", dataHome.c_str(), 1);
        setenv("XDG_CURRENT_DESKTOP", "Vela", 1);
        setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
        setenv("XTE_CACHE_ENABLED", "false", 1);
        setenv("VELA_TERMINAL", "false", 1);
        execl(binary, binary, "--", "echo", "a b", (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    std::ifstream ran(root / "ran");
    std::string args((std::istreambuf_iterator<char>(ran)), std::istreambuf_iterator<char>());
    EXPECT_EQ(args, "work-profile\n--\necho\na b\n");
}
