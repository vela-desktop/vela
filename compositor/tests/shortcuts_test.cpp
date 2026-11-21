// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The matcher and Vela's shortcut table (common/shortcuts,
// docs/keyboard.md §9.2): exact modifiers, letters by symbol on AZERTY and
// QWERTZ, the Latin fallback, positions for the digit row, the combination
// format, and every row of docs/shortcuts.md resolving to its action.

extern "C" {
#include "custom.h"
#include "shortcuts.h"
#include "table.h"
}
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <linux/input-event-codes.h>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace {

// A keyboard with a keymap, whose keys are pressed by evdev code.
struct Keyboard {
    xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap *keymap = nullptr;
    xkb_state *state = nullptr;
    explicit Keyboard(const char *layout, const char *options = nullptr)
    {
        xkb_rule_names names {};
        names.rules = "evdev";
        names.model = "pc105";
        names.layout = layout;
        names.options = options;
        keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        state = keymap ? xkb_state_new(keymap) : nullptr;
    }
    ~Keyboard()
    {
        xkb_state_unref(state);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
    }
    void hold(uint32_t code) { xkb_state_update_key(state, code + 8, XKB_KEY_DOWN); }
    void release(uint32_t code) { xkb_state_update_key(state, code + 8, XKB_KEY_UP); }
    void tap(uint32_t code)
    {
        hold(code);
        release(code);
    }
    void group(xkb_layout_index_t layout) { xkb_state_update_mask(state, 0, 0, 0, 0, 0, layout); }
    vela_press press(uint32_t code)
    {
        vela_press out {};
        vela_press_from_state(state, code + 8, &out);
        return out;
    }
    // The key whose level 1 in the first layout is `sym`.
    uint32_t key(xkb_keysym_t sym)
    {
        for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap); code <= xkb_keymap_max_keycode(keymap); ++code) {
            const xkb_keysym_t *syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(keymap, code, 0, 0, &syms) > 0 && syms[0] == sym && code >= 8) {
                return code - 8;
            }
        }
        return 0;
    }
};

vela_modes normal()
{
    vela_modes modes {};
    modes.numbers = VELA_SUPER_NUMBERS_APPS;
    return modes;
}

std::string format(const vela_combo &combo)
{
    char text[128];
    return vela_combo_format(&combo, text, sizeof(text)) ? text : "";
}

std::string normalized(const char *text)
{
    vela_combo combo {};
    return vela_combo_parse(text, &combo) ? format(combo) : "";
}

} // namespace

TEST(Shortcuts, ModifiersAreExact)
{
    Keyboard us("us");
    ASSERT_NE(us.keymap, nullptr);
    const vela_modes modes = normal();
    us.hold(KEY_LEFTMETA);
    vela_press press = us.press(KEY_E);
    EXPECT_EQ(press.mods, VELA_MOD_SUPER);
    const vela_binding *row = vela_binding_taken(&press, &modes);
    ASSERT_NE(row, nullptr);
    EXPECT_STREQ(row->arg, "files");
    // Win+Shift+E is not Win+E any more, nor Win+Shift+L Win+L.
    us.hold(KEY_LEFTSHIFT);
    press = us.press(KEY_E);
    EXPECT_EQ(press.mods, VELA_MOD_SUPER | VELA_MOD_SHIFT);
    EXPECT_EQ(vela_binding_taken(&press, &modes), nullptr);
    press = us.press(KEY_L);
    EXPECT_EQ(vela_binding_taken(&press, &modes), nullptr);
    us.release(KEY_LEFTSHIFT);
    // Caps Lock and Num Lock don't count.
    us.tap(KEY_CAPSLOCK);
    us.tap(KEY_NUMLOCK);
    press = us.press(KEY_L);
    EXPECT_EQ(press.mods, VELA_MOD_SUPER);
    row = vela_binding_taken(&press, &modes);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->action, VELA_ACTION_LOCK);
}

TEST(Shortcuts, AltGrIsNotAModifier)
{
    Keyboard it("it");
    it.hold(KEY_RIGHTALT); // AltGr
    EXPECT_EQ(it.press(KEY_E).mods, 0);
    it.hold(KEY_LEFTCTRL);
    EXPECT_EQ(it.press(KEY_E).mods, VELA_MOD_CTRL);
    // With us, the right Alt is an Alt.
    Keyboard us("us");
    us.hold(KEY_RIGHTALT);
    EXPECT_EQ(us.press(KEY_E).mods, VELA_MOD_ALT);
}

TEST(Shortcuts, LettersByTheirSymbol)
{
    // AZERTY: the Z is on us's W key; QWERTZ: on us's Y key.
    Keyboard fr("fr");
    EXPECT_EQ(fr.press(KEY_W).sym, XKB_KEY_z);
    EXPECT_EQ(fr.press(KEY_Q).sym, XKB_KEY_a);
    Keyboard de("de");
    EXPECT_EQ(de.press(KEY_Y).sym, XKB_KEY_z);
    EXPECT_EQ(de.press(KEY_Z).sym, XKB_KEY_y);
    // Lowercase whatever Shift does.
    de.hold(KEY_LEFTSHIFT);
    EXPECT_EQ(de.press(KEY_Y).sym, XKB_KEY_z);
    const vela_press shifted = de.press(KEY_Y);
    EXPECT_FALSE(vela_press_is_positional(&shifted));
    // A Latin-1 letter is a symbol too.
    de.release(KEY_LEFTSHIFT);
    const vela_press umlaut = de.press(KEY_LEFTBRACE);
    EXPECT_EQ(umlaut.sym, XKB_KEY_udiaeresis);
    EXPECT_FALSE(vela_press_is_positional(&umlaut));
}

TEST(Shortcuts, LatinFallbackForOtherScripts)
{
    // ru active after us: the letter of us, the first layout with one.
    Keyboard both("us,ru");
    both.group(1);
    EXPECT_EQ(both.press(KEY_A).sym, XKB_KEY_a);
    // ru alone: us's.
    Keyboard ru("ru");
    EXPECT_EQ(ru.press(KEY_A).sym, XKB_KEY_a);
    // Greek's Win+E opens Files.
    Keyboard gr("gr");
    gr.hold(KEY_LEFTMETA);
    vela_press press = gr.press(KEY_E);
    const vela_modes modes = normal();
    const vela_binding *row = vela_binding_taken(&press, &modes);
    ASSERT_NE(row, nullptr);
    EXPECT_STREQ(row->arg, "files");
}

TEST(Shortcuts, DigitRowAndSpecialKeysByPosition)
{
    Keyboard fr("fr");
    fr.hold(KEY_LEFTSHIFT);
    vela_press press = fr.press(KEY_1);
    EXPECT_TRUE(vela_press_is_positional(&press));
    vela_combo combo = vela_combo_from_press(&press);
    EXPECT_EQ(combo.kind, VELA_KEY_CODE);
    EXPECT_EQ(format(combo), "Shift+AE01");
    fr.release(KEY_LEFTSHIFT);
    press = fr.press(KEY_F5);
    EXPECT_EQ(format(vela_combo_from_press(&press)), "FK05");
    press = fr.press(KEY_ENTER);
    EXPECT_EQ(format(vela_combo_from_press(&press)), "RTRN");
    press = fr.press(KEY_KP1);
    EXPECT_TRUE(vela_press_is_positional(&press));
    // A letter is its symbol.
    fr.hold(KEY_LEFTCTRL);
    press = fr.press(KEY_W);
    EXPECT_EQ(format(vela_combo_from_press(&press)), "Ctrl+z");
    combo = vela_combo_from_press(&press);
    EXPECT_TRUE(vela_combo_matches(&combo, &press));
    press.mods |= VELA_MOD_SHIFT;
    EXPECT_FALSE(vela_combo_matches(&combo, &press));
}

TEST(Shortcuts, CombinationText)
{
    EXPECT_EQ(normalized("Ctrl+Alt+t"), "Ctrl+Alt+t");
    EXPECT_EQ(normalized("alt+CTRL+T"), "Ctrl+Alt+t");
    EXPECT_EQ(normalized("Super+Shift+k"), "Shift+Super+k");
    EXPECT_EQ(normalized("Win+FK01"), "Super+FK01");
    EXPECT_EQ(normalized("Super+F1"), "Super+FK01");
    EXPECT_EQ(normalized("Super+1"), "Super+AE01");
    EXPECT_EQ(normalized("Super+Return"), "Super+RTRN");
    EXPECT_EQ(normalized("Ctrl+plus"), "Ctrl+plus");
    EXPECT_EQ(normalized("Ctrl+ESC"), "Ctrl+ESC");
    EXPECT_EQ(normalized("XF86Calculator"), "I148");
    for (const char *bad : { "", "Ctrl+", "Hyper+x", "Ctrl+nosuchkey", "Ctrl++", "+x" }) {
        EXPECT_EQ(normalized(bad), "") << bad;
    }
    // Only [A-Za-z0-9+_], whatever the key.
    for (const char *text : { "Ctrl+bracketleft", "Super+KPAD", "Alt+LSGT", "Ctrl+udiaeresis" }) {
        const std::string out = normalized(text);
        EXPECT_FALSE(out.empty()) << text;
        EXPECT_TRUE(std::regex_match(out, std::regex("[A-Za-z0-9+_]+"))) << out;
    }
}

namespace {

bool parse_line(const std::string &line, vela_custom_line *out)
{
    return vela_custom_line_parse(line.data(), line.size(), out);
}

} // namespace

// shortcuts.conf's lines (docs/keyboard.md §9.4).
TEST(Shortcuts, CustomLines)
{
    vela_custom_line line {};
    // The target points into the line: they live as long as the test.
    const std::string app = "Ctrl+Alt+t=app org.kde.konsole.desktop";
    const std::string command = "Shift+Super+k=command notify-send 'a = b'";
    ASSERT_TRUE(parse_line(app, &line));
    EXPECT_EQ(format(line.combo), "Ctrl+Alt+t");
    EXPECT_EQ(line.kind, VELA_CUSTOM_APP);
    EXPECT_EQ(std::string(line.target, line.target_length), "org.kde.konsole.desktop");
    ASSERT_TRUE(parse_line(command, &line));
    EXPECT_EQ(line.kind, VELA_CUSTOM_COMMAND);
    EXPECT_EQ(std::string(line.target, line.target_length), "notify-send 'a = b'");
    ASSERT_TRUE(parse_line("Super+FK01=open https://docs.example.org/", &line));
    EXPECT_EQ(line.kind, VELA_CUSTOM_OPEN);
    ASSERT_TRUE(parse_line("Super+FK02=open /home/user/a b.pdf", &line));
    for (const std::string bad : { "", "# Ctrl+Alt+t=command x", "Ctrl+Alt+t", "=command x", "Ctrl+Alt+t=command",
             "Ctrl+Alt+t=command ", "Ctrl+Alt+t=script x", "Hyper+t=command x", "Ctrl+Alt+t=app ../x.desktop",
             "Ctrl+Alt+t=app konsole", "Ctrl+Alt+t=open notes.txt", "Ctrl+Alt+t=command a\tb",
             "Ctrl+Alt+t=command a\rb", "Ctrl+Alt+t=command  x" }) {
        EXPECT_FALSE(parse_line(bad, &line)) << bad;
    }
    // At most 4096 bytes.
    std::string longest = "Super+k=command " + std::string(4096 - 16, 'x');
    EXPECT_TRUE(parse_line(longest, &line));
    EXPECT_FALSE(parse_line(longest + "x", &line));
}

// Win+number in the table says what keyboard.c does (super_numbers.c).
TEST(Shortcuts, NumberRowsAgreeWithTheNumberKeys)
{
    Keyboard us("us");
    for (vela_super_numbers mode : { VELA_SUPER_NUMBERS_APPS, VELA_SUPER_NUMBERS_DESKTOPS, VELA_SUPER_NUMBERS_OFF }) {
        vela_modes modes = normal();
        modes.numbers = mode;
        for (int bits = 0; bits < 16; ++bits) {
            vela_press press = us.press(KEY_3);
            press.mods = (uint8_t)bits;
            const vela_super_modifiers held { (bits & VELA_MOD_SUPER) != 0, (bits & VELA_MOD_CTRL) != 0,
                (bits & VELA_MOD_ALT) != 0, (bits & VELA_MOD_SHIFT) != 0 };
            const enum vela_super_number_action action = vela_super_number_action(mode, held);
            const vela_binding *row = vela_binding_find(&press, &modes, VELA_STAGE_NORMAL);
            const bool numbered = row && row->action == VELA_ACTION_TASKBAR_NUMBER;
            EXPECT_EQ(numbered, action != VELA_SUPER_NUMBER_NONE) << mode << " " << bits;
            if (numbered && action != VELA_SUPER_NUMBER_SWITCH && action != VELA_SUPER_NUMBER_MOVE) {
                EXPECT_STREQ(row->arg, vela_super_number_verb(action));
            }
        }
    }
}

TEST(Shortcuts, ModesGateTheirRows)
{
    Keyboard us("us");
    vela_modes modes = normal();
    us.hold(KEY_LEFTALT);
    vela_press press = us.press(KEY_E);
    EXPECT_EQ(vela_binding_taken(&press, &modes), nullptr) << "Alt+E is the apps' in the real session";
    modes.nested = true;
    ASSERT_NE(vela_binding_taken(&press, &modes), nullptr);
    us.release(KEY_LEFTALT);
    us.hold(KEY_LEFTMETA);
    press = us.press(KEY_MINUS);
    EXPECT_EQ(vela_binding_taken(&press, &modes), nullptr) << "Win+minus without the magnifier";
    modes.magnifier = true;
    ASSERT_NE(vela_binding_taken(&press, &modes), nullptr);
    us.hold(KEY_LEFTCTRL);
    press = us.press(KEY_C);
    EXPECT_EQ(vela_binding_taken(&press, &modes), nullptr);
    modes.color_filter_shortcut = true;
    ASSERT_NE(vela_binding_taken(&press, &modes), nullptr);
}

// ------------------------------------------------- docs/shortcuts.md --

namespace {

struct Expected {
    vela_action action;
    const char *arg = nullptr;
};

using Cell = std::vector<Expected>;

Expected shell(const char *arg)
{
    return { VELA_ACTION_SHELL, arg };
}

// Each table row by its action: what each combination of the normal cell,
// then of the nested one, does, in their order. A row not here is a
// mouse or Super-alone row and must have no keys.
const std::map<std::string, std::pair<Cell, Cell>> expected = {
    { "App launcher", { {}, { shell("toggle-start") } } },
    { "Activate a taskbar app / open its window previews",
        { { { VELA_ACTION_TASKBAR_NUMBER, "activate" }, { VELA_ACTION_TASKBAR_NUMBER, "activate" },
              { VELA_ACTION_TASKBAR_NUMBER, "activate" } },
            {} } },
    { "Open a new instance of a taskbar app", { { { VELA_ACTION_TASKBAR_NUMBER, "new" } }, {} } },
    { "Open a taskbar app's jump list", { { { VELA_ACTION_TASKBAR_NUMBER, "jump" } }, {} } },
    { "Task View", { { shell("task-view") }, { shell("task-view") } } },
    { "Switch windows", { { { VELA_ACTION_SWITCHER_NEXT } }, { { VELA_ACTION_SWITCHER_NEXT } } } },
    { "Snap left / right",
        { { { VELA_ACTION_SNAP_LEFT }, { VELA_ACTION_SNAP_RIGHT } },
            { { VELA_ACTION_SNAP_LEFT }, { VELA_ACTION_SNAP_RIGHT } } } },
    { "Maximize / restore, then minimize",
        { { { VELA_ACTION_SNAP_UP }, { VELA_ACTION_SNAP_DOWN } }, { { VELA_ACTION_MAXIMIZE } } } },
    { "Snap layouts", { { { VELA_ACTION_SNAP_LAYOUTS } }, { { VELA_ACTION_SNAP_LAYOUTS } } } },
    { "Previous / next desktop",
        { { { VELA_ACTION_DESKTOP_PREVIOUS }, { VELA_ACTION_DESKTOP_NEXT } },
            { { VELA_ACTION_DESKTOP_PREVIOUS }, { VELA_ACTION_DESKTOP_NEXT } } } },
    { "New desktop", { { { VELA_ACTION_DESKTOP_NEW } }, { { VELA_ACTION_DESKTOP_NEW } } } },
    { "Close desktop", { { { VELA_ACTION_DESKTOP_CLOSE } }, {} } },
    { "Show desktop (also: the far-right taskbar corner, if enabled in Settings → Personalization → Taskbar)",
        { { shell("show-desktop") }, { shell("show-desktop") } } },
    { "Window menu", { { { VELA_ACTION_WINDOW_MENU } }, { { VELA_ACTION_WINDOW_MENU } } } },
    { "Close window", { { { VELA_ACTION_CLOSE_WINDOW } }, { { VELA_ACTION_CLOSE_WINDOW } } } },
    { "Files", { { shell("files") }, { shell("files") } } },
    { "Settings", { { shell("settings") }, { shell("settings") } } },
    { "Quick settings", { { shell("quick-settings") }, { shell("quick-settings") } } },
    { "Notification center", { { shell("notification-center") }, { shell("notification-center") } } },
    { "Clipboard history", { { shell("clipboard") }, {} } },
    { "Sound output and volume mixer", { { shell("sound-output") }, {} } },
    { "Snipping tool", { { shell("snip"), shell("snip") }, { shell("snip") } } },
    { "Power-user menu", { { shell("winx") }, { shell("winx") } } },
    { "Terminal", { { { VELA_ACTION_TERMINAL } }, { { VELA_ACTION_TERMINAL } } } },
    { "Lock", { { { VELA_ACTION_LOCK } }, { { VELA_ACTION_LOCK } } } },
    { "Next keyboard layout", { { { VELA_ACTION_NEXT_LAYOUT } }, {} } },
    { "Magnifier in", { { { VELA_ACTION_ZOOM_IN } }, {} } },
    { "Magnifier out", { { { VELA_ACTION_ZOOM_OUT } }, {} } },
    { "Magnifier off", { { { VELA_ACTION_MAGNIFIER_OFF } }, {} } },
    { "Toggle color filters", { { { VELA_ACTION_COLOR_FILTERS } }, {} } },
    { "Play / pause, stop", { { shell("media play-pause"), shell("media stop") }, {} } },
    { "Previous / next track", { { shell("media previous"), shell("media next") }, {} } },
    { "Volume down / up", { { shell("volume down"), shell("volume up") }, {} } },
    { "Mute", { { shell("volume mute") }, {} } },
    { "Switch virtual console", { { { VELA_ACTION_SWITCH_VT }, { VELA_ACTION_SWITCH_VT } }, {} } },
    { "Quit Vela", { { { VELA_ACTION_QUIT } }, {} } },
};

// The sections whose tables are Vela's (not Files').
const char *const sections[] = { "Windows and multitasking", "Shell and apps", "Input and accessibility",
    "Media and volume", "Session and system" };

using Combo = std::vector<std::string>;

// The combinations of a cell: "<kbd>Super</kbd>+<kbd>←</kbd> / <kbd>→</kbd>"
// is Super+← and Super+→; "1…9, 0" the first, the last and 0; "or" a new one.
std::vector<Combo> combos(std::string cell)
{
    cell = std::regex_replace(cell, std::regex("\\+number"), "+<kbd>1</kbd>");
    std::vector<Combo> out;
    const std::regex kbd("<kbd>([^<]*)</kbd>");
    size_t end = 0;
    for (auto it = std::sregex_iterator(cell.begin(), cell.end(), kbd); it != std::sregex_iterator(); ++it) {
        const std::string between = cell.substr(end, (size_t)it->position() - end);
        const std::string key = (*it)[1];
        end = (size_t)(it->position() + it->length());
        if (out.empty() || between.find(" or ") != std::string::npos) {
            out.push_back({ key });
        } else if (between == "+") {
            out.back().push_back(key);
        } else {
            // "/", "…", ",": the same modifiers, another key.
            Combo next = out.back();
            next.back() = key;
            out.push_back(next);
        }
    }
    return out;
}

const std::map<std::string, uint32_t> codes = {
    { "Super", KEY_LEFTMETA },
    { "Ctrl", KEY_LEFTCTRL },
    { "Alt", KEY_LEFTALT },
    { "Shift", KEY_LEFTSHIFT },
    { "←", KEY_LEFT },
    { "→", KEY_RIGHT },
    { "↑", KEY_UP },
    { "↓", KEY_DOWN },
    { "Tab", KEY_TAB },
    { "Enter", KEY_ENTER },
    { "Space", KEY_SPACE },
    { "Esc", KEY_ESC },
    { "PrtSc", KEY_SYSRQ },
};
const std::map<std::string, xkb_keysym_t> symbols = {
    { "+", XKB_KEY_plus },
    { "-", XKB_KEY_minus },
    { "⏯", XKB_KEY_XF86AudioPlay },
    { "⏹", XKB_KEY_XF86AudioStop },
    { "⏮", XKB_KEY_XF86AudioPrev },
    { "⏭", XKB_KEY_XF86AudioNext },
    { "🔉", XKB_KEY_XF86AudioLowerVolume },
    { "🔊", XKB_KEY_XF86AudioRaiseVolume },
    { "🔇", XKB_KEY_XF86AudioMute },
};

// The key's evdev code on an Italian keyboard (+ and - unshifted there).
uint32_t code_of(Keyboard &keyboard, const std::string &name)
{
    if (codes.count(name)) {
        return codes.at(name);
    }
    if (symbols.count(name)) {
        return keyboard.key(symbols.at(name));
    }
    if (name.size() >= 2 && name[0] == 'F' && isdigit((unsigned char)name[1])) {
        const int n = std::stoi(name.substr(1));
        return n <= 10 ? KEY_F1 + n - 1 : KEY_F11 + n - 11;
    }
    if (name.size() == 1 && isdigit((unsigned char)name[0])) {
        return name == "0" ? KEY_0 : KEY_1 + (uint32_t)(name[0] - '1');
    }
    if (name.size() == 1 && isalpha((unsigned char)name[0])) {
        return keyboard.key(xkb_keysym_to_lower(xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE)));
    }
    return 0;
}

const vela_binding *resolve(const Combo &combo, const vela_modes &modes)
{
    Keyboard keyboard("it");
    for (size_t i = 0; i + 1 < combo.size(); ++i) {
        keyboard.hold(code_of(keyboard, combo[i]));
    }
    const uint32_t code = code_of(keyboard, combo.back());
    if (!code) {
        ADD_FAILURE() << "unknown key " << combo.back();
        return nullptr;
    }
    const vela_press press = keyboard.press(code);
    return vela_binding_taken(&press, &modes);
}

std::vector<std::string> split_row(const std::string &line)
{
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream in(line.substr(1));
    while (std::getline(in, cell, '|')) {
        cells.push_back(std::regex_replace(cell, std::regex("^\\s+|\\s+$"), ""));
    }
    return cells;
}

void check(const std::string &action, const std::string &cell, const Cell &want, const vela_modes &modes)
{
    // Super alone opens Start: not a row of the table (docs/keyboard.md §10).
    std::vector<Combo> found = combos(cell);
    found.erase(std::remove(found.begin(), found.end(), Combo { "Super" }), found.end());
    ASSERT_EQ(found.size(), want.size()) << action << ": " << cell;
    for (size_t i = 0; i < found.size(); ++i) {
        const vela_binding *row = resolve(found[i], modes);
        ASSERT_NE(row, nullptr) << action << ": combination " << i + 1 << " of " << cell;
        EXPECT_EQ(row->action, want[i].action) << action << ": " << cell;
        if (want[i].arg) {
            EXPECT_STREQ(row->arg, want[i].arg) << action << ": " << cell;
        }
    }
}

} // namespace

TEST(Shortcuts, EveryRowOfTheDocumentResolvesToItsAction)
{
    std::ifstream file(VELA_DOCS_DIR "/shortcuts.md");
    ASSERT_TRUE(file.is_open());
    vela_modes real = normal();
    // The rows that need a mode say so in the document: the magnifier on,
    // the color-filter shortcut enabled.
    real.magnifier = true;
    real.color_filter_shortcut = true;
    vela_modes nested = real;
    nested.nested = true;
    std::string line;
    std::string section;
    size_t rows = 0;
    while (std::getline(file, line)) {
        if (line.rfind("## ", 0) == 0) {
            section = line.substr(3);
            continue;
        }
        const bool ours = std::find(std::begin(sections), std::end(sections), section) != std::end(sections);
        if (!ours || line.rfind("| ", 0) != 0 || line.find("---") != std::string::npos) {
            continue;
        }
        const std::vector<std::string> cells = split_row(line);
        if (cells.empty() || cells[0] == "Action") {
            continue;
        }
        const auto it = expected.find(cells[0]);
        if (it == expected.end()) {
            EXPECT_TRUE(combos(line).empty() || line.find("drag") != std::string::npos)
                << "a row of docs/shortcuts.md the test doesn't know: " << cells[0];
            continue;
        }
        ++rows;
        check(cells[0], cells[1], it->second.first, real);
        if (cells.size() > 2) {
            check(cells[0], cells[2], it->second.second, nested);
        }
    }
    EXPECT_EQ(rows, expected.size()) << "every expected row is in the document";
}
