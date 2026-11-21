// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The terminal lookup (docs/keyboard.md §11): the five steps, the desktop
// files' rules and the three requests, in a private tree.

#include "terminal.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

class Terminal : public ::testing::Test {
protected:
    fs::path root;
    std::string path;
    std::string configHome;
    std::string dataHome;
    std::string dataDirs;
    std::string desktop = "Vela";
    std::string variable;

    void SetUp() override
    {
        char pattern[] = "/tmp/vela-terminal-test-XXXXXX";
        ASSERT_NE(mkdtemp(pattern), nullptr);
        root = pattern;
        path = (root / "bin").string();
        configHome = (root / "config").string();
        dataHome = (root / "data").string();
        dataDirs = (root / "usr/share").string();
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

    // A desktop file in the system's applications directory (or `dir`).
    void entry(const std::string& id, const std::string& keys, const std::string& dir = "")
    {
        const fs::path file = fs::path(dir.empty() ? dataDirs : dir) / "applications" / id;
        fs::create_directories(file.parent_path());
        std::ofstream(file) << "[Desktop Entry]\nType=Application\nName=" << id << "\n"
                            << keys << "\n[Desktop Action New]\nExec=nothing\nX-TerminalArgExec=--wrong\n";
    }

    void terminal(const std::string& id, const std::string& exec, const std::string& more = "")
    {
        entry(id, "Exec=" + exec + "\nCategories=System;TerminalEmulator;\n" + more);
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
        e.config_dirs = "/nonexistent";
        e.data_home = dataHome.c_str();
        e.data_dirs = dataDirs.c_str();
        e.current_desktop = desktop.c_str();
        return e;
    }

    // What runs, as one string: "source id | word word...".
    std::string run(const std::vector<const char*>& command = {}, bool automatic = false)
    {
        const vela_terminal_env e = env();
        vela_terminal t;
        if (!vela_terminal_find(&e, automatic, &t)) {
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

    std::vector<std::string> installed() const
    {
        const vela_terminal_env e = env();
        char** ids = vela_terminal_installed(&e);
        std::vector<std::string> out;
        for (char** id = ids; *id; ++id) {
            out.emplace_back(*id);
        }
        vela_terminal_strv_free(ids);
        return out;
    }
};

const std::vector<const char*> command { "sh", "-c", "echo hi" };

} // namespace

TEST_F(Terminal, NothingInstalled)
{
    EXPECT_EQ(run(), "none");
    terminal("org.kde.konsole.desktop", "konsole"); // its program isn't there
    EXPECT_EQ(run(), "none");
    EXPECT_TRUE(installed().empty());
}

TEST_F(Terminal, FixedListWithoutDesktopFiles)
{
    program("xterm");
    EXPECT_EQ(run(command), "5 - | xterm -e sh -c echo hi");
    program("foot");
    EXPECT_EQ(run(command), "5 - | foot sh -c echo hi");
    program("konsole");
    EXPECT_EQ(run(), "5 - | konsole");
    EXPECT_EQ(run(command), "5 - | konsole -e sh -c echo hi");
}

TEST_F(Terminal, InstalledInDesktopIdOrder)
{
    program("konsole");
    program("alacritty");
    program("kwi");
    terminal("org.kde.konsole.desktop", "konsole");
    terminal("io.kwi.Kwi.desktop", "kwi %U");
    terminal("Alacritty.desktop", "alacritty");
    EXPECT_EQ(run(), "4 Alacritty.desktop | alacritty");
    EXPECT_EQ(installed(),
        (std::vector<std::string> { "Alacritty.desktop", "io.kwi.Kwi.desktop", "org.kde.konsole.desktop" }));
    // No name is written in Vela: Kwi is found like any other.
    fs::remove(fs::path(dataDirs) / "applications/Alacritty.desktop");
    EXPECT_EQ(run(command), "4 io.kwi.Kwi.desktop | kwi -e sh -c echo hi");
}

TEST_F(Terminal, DesktopFileRules)
{
    program("a");
    program("b");
    program("dropdown");
    // Not terminals, hidden, not here, drop-down, TryExec missing.
    entry("a1.desktop", "Exec=a\nCategories=System;");
    terminal("a2.desktop", "a", "Hidden=true");
    terminal("a3.desktop", "a", "NotShowIn=GNOME;Vela;");
    terminal("a4.desktop", "a", "OnlyShowIn=KDE;");
    terminal("a5.desktop", "a", "TryExec=missing");
    terminal("a6.desktop", "missing-program");
    terminal("org.kde.yakuake.desktop", "dropdown");
    entry("a7.desktop", "Type=Link\nExec=a\nCategories=TerminalEmulator;");
    terminal("b.desktop", "b");
    EXPECT_EQ(installed(), std::vector<std::string> { "b.desktop" });
    EXPECT_EQ(run(), "4 b.desktop | b");
    // OnlyShowIn in another of XDG_CURRENT_DESKTOP's names, any case.
    desktop = "Vela:kde";
    EXPECT_EQ(run(), "4 a4.desktop | a");
    // NoDisplay doesn't hide a terminal (xdg-terminal-exec's rule).
    desktop = "Vela";
    terminal("a0.desktop", "a", "NoDisplay=true");
    EXPECT_EQ(run(), "4 a0.desktop | a");
}

TEST_F(Terminal, DesktopIdsAndPrecedence)
{
    program("a");
    program("b");
    // A subdirectory's file: kde-term.desktop.
    entry("kde/term.desktop", "Exec=a\nCategories=TerminalEmulator;");
    EXPECT_EQ(run(), "4 kde-term.desktop | a");
    // The user's own file of the same ID wins over the system's.
    entry("kde-term.desktop", "Exec=b\nCategories=TerminalEmulator;", dataHome);
    EXPECT_EQ(run(), "4 kde-term.desktop | b");
    // Hidden there hides the system's too.
    entry("kde-term.desktop", "Exec=b\nCategories=TerminalEmulator;\nHidden=true", dataHome);
    EXPECT_EQ(run(), "none");
}

TEST_F(Terminal, ExecLine)
{
    program("my term");
    program("kitty");
    terminal("a.desktop", "\"my term\" --title \"A \\\\\\\\ \\\"b\\\"\" %F --%% %i");
    EXPECT_EQ(run(), "4 a.desktop | my term --title A \\ \"b\" --%");
    // An unfinished quote is no command.
    terminal("a.desktop", "\"my term");
    terminal("b.desktop", "kitty --single-instance %U");
    EXPECT_EQ(run(), "4 b.desktop | kitty --single-instance");
    // \s in a value.
    terminal("a.desktop", "kitty\\s--one");
    EXPECT_EQ(run(), "4 a.desktop | kitty --one");
}

TEST_F(Terminal, ExecArgument)
{
    program("t");
    // Set: before the command.
    terminal("a.desktop", "t", "X-TerminalArgExec=--");
    EXPECT_EQ(run(command), "4 a.desktop | t -- sh -c echo hi");
    // Set empty: the command right after.
    terminal("a.desktop", "t", "X-TerminalArgExec=");
    EXPECT_EQ(run(command), "4 a.desktop | t sh -c echo hi");
    // Absent, with xdg-terminal-exec's table.
    terminal("a.desktop", "t");
    write(fs::path(dataDirs) / "xdg-terminal-exec/xdg-terminals.list",
        "# defaults\n/execarg_default:other.desktop:-x\n/execarg_default:a.desktop:execute\n");
    EXPECT_EQ(run(command), "4 a.desktop | t execute sh -c echo hi");
    // The desktop's own list comes first.
    write(fs::path(dataDirs) / "xdg-terminal-exec/vela-xdg-terminals.list", "/execarg_default:a.desktop:-c\n");
    EXPECT_EQ(run(command), "4 a.desktop | t -c sh -c echo hi");
    // Absent everywhere: -e.
    fs::remove_all(fs::path(dataDirs) / "xdg-terminal-exec");
    EXPECT_EQ(run(command), "4 a.desktop | t -e sh -c echo hi");
    // An empty terminal never gets it.
    EXPECT_EQ(run(), "4 a.desktop | t");
}

TEST_F(Terminal, XdgTerminalExec)
{
    program("t");
    terminal("a.desktop", "t");
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
    program("a");
    program("b");
    program("kitty");
    program("xdg-terminal-exec");
    terminal("a.desktop", "a");
    entry("b.desktop", "Exec=b\nX-TerminalArgExec=--"); // not even a terminal: chosen anyway
    variable = "kitty";
    const fs::path list = fs::path(configHome) / "vela-xdg-terminals.list";
    write(list, "# Settings\n/execarg_default:b.desktop:-x\n-a.desktop\n\n  b.desktop:new-window \na.desktop\n");
    EXPECT_EQ(run(command), "1 b.desktop | b -- sh -c echo hi");
    // "Automatic" skips it.
    EXPECT_EQ(run({}, true), "2 - | kitty");
    // Only the first entry: one that can't start falls to the next steps.
    write(list, "missing.desktop\nb.desktop\n");
    EXPECT_EQ(run(), "2 - | kitty");
    write(list, "a.desktop\n");
    EXPECT_EQ(run(), "1 a.desktop | a");
    // From HOME without XDG_CONFIG_HOME.
    configHome.clear();
    EXPECT_EQ(run(), "2 - | kitty");
    write(root / ".config/vela-xdg-terminals.list", "a.desktop\n");
    EXPECT_EQ(run(), "1 a.desktop | a");
    const vela_terminal_env e = env();
    char* choice = vela_terminal_choice_path(&e);
    EXPECT_EQ(std::string(choice), (root / ".config/vela-xdg-terminals.list").string());
    free(choice);
}

TEST_F(Terminal, LongAndBrokenLines)
{
    program("t");
    terminal("a.desktop", "t", std::string(5000, 'x') + "=1\nX-TerminalArgExec=--");
    EXPECT_EQ(run(command), "4 a.desktop | t -- sh -c echo hi");
    write(fs::path(dataDirs) / "applications/b.desktop", std::string("\0\0\n[Desktop Entry", 18));
    EXPECT_EQ(run(command), "4 a.desktop | t -- sh -c echo hi");
}

// vela-terminal itself: --print, then running the command.
TEST_F(Terminal, Program)
{
    const char* binary = getenv("VELA_TERMINAL_PROGRAM");
    ASSERT_NE(binary, nullptr);
    const fs::path term = root / "bin/t";
    std::ofstream(term) << "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" << (root / "ran").string() << "\"\n";
    fs::permissions(term, fs::perms::owner_all);
    terminal("a.desktop", "t");
    const std::string envs = "env -i HOME='" + root.string() + "' PATH='" + path + "' XDG_DATA_DIRS='" + dataDirs
        + "' XDG_CONFIG_HOME='" + configHome + "' XDG_DATA_HOME='" + dataHome + "' ";
    FILE* out = popen((envs + "'" + binary + "' --print -- echo 'a b'").c_str(), "r");
    ASSERT_NE(out, nullptr);
    std::string text;
    std::array<char, 256> chunk {};
    while (fgets(chunk.data(), chunk.size(), out)) {
        text += chunk.data();
    }
    EXPECT_EQ(pclose(out), 0);
    EXPECT_EQ(text, "source 4\nid a.desktop\n" + term.string() + "\n-e\necho\na b\n");
    EXPECT_EQ(system((envs + "'" + binary + "' -- echo 'a b'").c_str()), 0);
    std::ifstream ran(root / "ran");
    std::string args((std::istreambuf_iterator<char>(ran)), std::istreambuf_iterator<char>());
    EXPECT_EQ(args, "-e\necho\na b\n");
    // Nothing installed: a message and 1.
    fs::remove(term);
    EXPECT_NE(system((envs + "'" + binary + "' 2>/dev/null").c_str()), 0);
}
