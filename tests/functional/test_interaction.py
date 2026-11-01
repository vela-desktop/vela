# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Pointer and keyboard on windows (interact.c, bindings.c, snap.c): the
invisible resize borders, keyboard "Move" and "Size", the window menu (right
click on the title, Alt+Space), snapping by dragging against the edge with
Snap Assist, snap layouts (Win+Z) and snap groups. The shell is a FakeShell
collecting the messages."""

import os
import time
import unittest

from harness import Session, tool

MAGENTA = (255, 0, 255)  # vela-pattern's menu (xdg-popup)
GREEN = (0, 192, 0)  # vela-x11's menu (override-redirect)


class Interaction(unittest.TestCase):
    def setUp(self):
        self.vela = Session()
        self.vela.start()
        self.shell = self.vela.fake_shell()

    def tearDown(self):
        self.vela.stop()

    def frame(self, window):
        """The window's frame, title bar included."""
        w = self.vela.state().window(window)
        return w["x"], w["y"], w["w"], w["h"]

    def center(self, window):
        x, y, w, h = self.frame(window)
        return x + w // 2, y + h // 2

    def test_resize_from_invisible_border(self):
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, w, h = self.frame(window)
        # 4 pixels outside the right edge, at half height.
        grab_x, grab_y = x + w + 4, y + h // 2
        self.vela.input("move", grab_x, grab_y, "sleep", "50", "down", "sleep", "50",
                        "move", grab_x + 50, grab_y, "sleep", "50", "move", grab_x + 100, grab_y, "sleep", "100", "up")
        state = self.vela.wait_for(lambda s: s.window(window)["w"] == w + 100, what="100 wider")
        self.assertEqual(state.window(window)["x"], x)

    def drag_border(self, x, y):
        """Presses at (x, y) and drags 100 to the right."""
        self.vela.input("move", x, y, "sleep", "50", "down", "sleep", "50", "move", x + 50, y, "sleep", "50",
                        "move", x + 100, y, "sleep", "100", "up")
        time.sleep(0.2)

    def centered(self, width, height, title_bar=0):
        """Where a new window opens: centered on the output (its surface)."""
        output = self.vela.state().output
        return (output["w"] - width) // 2, (output["h"] - height - title_bar) // 2 + title_bar

    def test_menu_covers_the_border_below_it(self):
        # A menu of the window above, sticking out over the invisible border
        # of the one below: the menu gets the press, not the border.
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, w, h = self.frame(window)
        grab_x, grab_y = x + w + 4, y + h // 2
        left, top = self.centered(200, 150)
        self.vela.open_window(command=[tool("vela-pattern"), "--popup", f"{grab_x - 100 - left},{grab_y - 75 - top}",
                                       "200", "150"])
        self.vela.wait_for(lambda _: self.vela.pixels().at(grab_x, grab_y) == MAGENTA, what="the menu on the border",
                           read_state=False)
        self.drag_border(grab_x, grab_y)
        state = self.vela.state()
        self.assertEqual(state.window(window)["w"], w)
        self.assertNotEqual(state["focused"], window)

    def test_own_menu_covers_its_border(self):
        # The window's own menu, past its right edge: the menu again.
        left, top = self.centered(300, 200, title_bar=32)
        grab_x, grab_y = left + 300 + 4, top + 100
        window = self.vela.open_window(command=[tool("vela-pattern"), "--decorated", "--popup",
                                                f"{grab_x - 100 - left},{grab_y - 75 - top}", "300", "200"])
        x, y, w, h = self.frame(window)
        self.assertEqual((x + w + 4, y + h - 100), (grab_x, grab_y))
        self.vela.wait_for(lambda _: self.vela.pixels().at(grab_x, grab_y) == MAGENTA, what="the menu on the border",
                           read_state=False)
        self.drag_border(grab_x, grab_y)
        self.assertEqual(self.vela.state().window(window)["w"], w)

    def test_x11_menu_covers_the_border_below_it(self):
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, w, h = self.frame(window)
        grab_x, grab_y = x + w + 4, y + h // 2
        display = self.vela.wait_for_log("Xwayland on DISPLAY=", lambda line: line.split("DISPLAY=", 1)[1].split()[0])
        # The menu is 120x80, at a point of the screen.
        self.vela.open_window(command=[tool("vela-x11"), "--menu", f"{grab_x - 60},{grab_y - 40}", "200", "150"],
                              env=dict(self.vela.client_env, DISPLAY=display))
        self.vela.wait_for(lambda _: self.vela.pixels().at(grab_x, grab_y) == GREEN, what="the menu on the border",
                           read_state=False)
        self.drag_border(grab_x, grab_y)
        state = self.vela.state()
        self.assertEqual(state.window(window)["w"], w)
        self.assertNotEqual(state["focused"], window)

    def x11_clicks(self, *args):
        """Opens vela-x11 writing its button events to a file; (window, the
        file)."""
        display = self.vela.wait_for_log("Xwayland on DISPLAY=", lambda line: line.split("DISPLAY=", 1)[1].split()[0])
        clicks = os.path.join(self.vela.directory, f"clicks-{len(self.vela.clients)}")
        window = self.vela.open_window(command=[tool("vela-x11"), "--clicks", clicks, *map(str, args)],
                                       env=dict(self.vela.client_env, DISPLAY=display))
        return window, clicks

    def pattern_clicks(self, width, height):
        """Opens vela-pattern logging its button events; (window, its log)."""
        log = os.path.join(self.vela.directory, f"client-{len(self.vela.clients) + 1}.log")
        window = self.vela.open_window(command=[tool("vela-pattern"), "--log-pointer", str(width), str(height)])
        return window, log

    def click_reaches(self, path, lines):
        """A click where the pointer is, without moving it: `lines(text)` of
        the file at `path` count the press and the release."""
        self.vela.input("down", "sleep", "50", "up")

        def got_it(_):
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    return lines(f.read()) >= 2
            except FileNotFoundError:
                return False
        self.vela.wait_for(got_it, read_state=False, what="the press and the release")

    @staticmethod
    def pattern_buttons(text):
        return sum(line.startswith("vela-pattern: pointer-button 272 ") for line in text.splitlines())

    @staticmethod
    def x11_buttons(text):
        return len(text.splitlines())

    # What is under a pointer at rest changes: the pointer follows, as if it
    # had moved. Before, it stayed with what was there until the next motion.

    def test_pointer_follows_a_window_opening_under_it(self):
        below = self.vela.open_window(500, 400)
        self.vela.input("move", *self.center(below))
        window, log = self.pattern_clicks(300, 200)
        self.vela.wait_still(window)
        self.click_reaches(log, self.pattern_buttons)

    def test_pointer_follows_an_x11_window_opening_under_it(self):
        # Opening, it rises under the pointer: X11 places the click with
        # where in the window the pointer is, which must follow too.
        below = self.vela.open_window(500, 400)
        x, y = self.center(below)
        self.vela.input("move", x, y)
        window, clicks = self.x11_clicks(300, 200)
        self.vela.wait_still(window)
        self.click_reaches(clicks, self.x11_buttons)
        left, top, _, _ = self.frame(window)
        with open(clicks, encoding="ascii") as f:
            press = tuple(map(int, f.readline().split()))
        self.assertEqual(press, (x - left, y - top - 32, x, y))

    def test_pointer_follows_a_window_closing_above(self):
        window, log = self.pattern_clicks(500, 400)
        above = self.vela.open_window(300, 200)
        self.vela.input("move", *self.center(above))
        self.vela.command(f"window {above} close")
        self.vela.wait_for(lambda s: not any(w["id"] == above for w in s.windows), what="the window closed")
        self.click_reaches(log, self.pattern_buttons)

    def test_pointer_follows_a_game_going_fullscreen_over_a_border(self):
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, w, h = self.frame(window)
        self.vela.input("move", x + w + 4, y + h // 2)
        game, clicks = self.x11_clicks("--start-fullscreen", 640, 480)
        self.vela.wait_for(lambda s: s.window(game)["fullscreen"] and s["focused"] == game, what="the game in front")
        self.click_reaches(clicks, self.x11_buttons)

    def test_keyboard_move_and_cancel(self):
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, _, _ = self.frame(window)
        self.vela.command("window active move")
        self.vela.keys("Right", "Right", "Right", "Down", "Return")
        state = self.vela.wait_for(lambda s: s.window(window)["x"] == x + 30, what="moved 30 to the right")
        self.assertEqual(self.frame(window)[1], y + 10)
        # Esc: back to how it was.
        self.vela.command("window active move")
        self.vela.keys("Left", "Left", "Escape")
        time.sleep(0.2)
        self.assertEqual(self.vela.state().window(window)["x"], x + 30)

    def test_keyboard_resize(self):
        window = self.vela.open_window(300, 200, decorated=True)
        _, _, w, _ = self.frame(window)
        self.vela.command("window active resize")
        # The first key chooses the edge, the next ones move it (Ctrl: by one
        # unit).
        self.vela.keys("Right", "Right", "Right", "ctrl+Right", "Return")
        self.vela.wait_for(lambda s: s.window(window)["w"] == w + 21, what="21 wider")

    def test_media_keys_reach_the_shell_whatever_has_focus(self):
        window = self.vela.open_window(300, 200)
        self.vela.wait_for(lambda s: s["focused"] == window)
        self.vela.keys("XF86AudioPlay", "XF86AudioNext", "XF86AudioRaiseVolume", "XF86AudioLowerVolume",
                       "XF86AudioMute", "XF86AudioMicMute")
        self.shell.wait_for("volume mic-mute")
        self.assertEqual(self.shell.received("media") + self.shell.received("volume"),
                         ["media play-pause", "media next", "volume up", "volume down", "volume mute",
                          "volume mic-mute"])

    def test_held_volume_key_repeats(self):
        # With the keyboard's delay and rate (the virtual one: 600 ms, 25 a
        # second); released, it stops.
        self.vela.input("keydown", "XF86AudioRaiseVolume", "sleep", "1000", "keyup", "XF86AudioRaiseVolume")
        time.sleep(0.3)
        held = len(self.shell.received("volume up"))
        self.assertGreaterEqual(held, 6)
        self.assertLessEqual(held, 25)
        time.sleep(0.3)
        self.assertEqual(len(self.shell.received("volume up")), held)
        # A knob's notch (pressed and released at once) is one step.
        self.shell.clear()
        self.vela.keys("XF86AudioLowerVolume")
        time.sleep(0.6)
        self.assertEqual(self.shell.received("volume down"), ["volume down"])

    def test_win_ctrl_v_opens_the_sound_page(self):
        self.vela.keys("super+ctrl+v")
        self.shell.wait_for("sound-output")
        self.vela.keys("super+v")
        self.shell.wait_for("clipboard")
        self.assertEqual(self.shell.received("sound-output"), ["sound-output"])

    def test_window_menu(self):
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, _, _ = self.frame(window)
        self.vela.input("move", x + 120, y + 16, "sleep", "50", "click", "right")
        line = self.shell.wait_for("window-menu ")
        self.assertEqual(line.split()[1:3], [window, "HEADLESS-1"])
        self.assertEqual(line.split()[3:], [str(x + 120), str(y + 16), "0", "1", "0"])
        self.shell.clear()
        self.vela.keys("alt+space")
        line = self.shell.wait_for("window-menu ")
        self.assertEqual(line.split()[1], window)
        self.assertEqual(line.split()[-1], "1") # from the keyboard

    def test_drag_to_edge_snaps_and_offers_assist(self):
        other = self.vela.open_window(401, 301)
        window = self.vela.open_window(300, 200, decorated=True)
        x, y, _, _ = self.frame(window)
        grab_x, grab_y = x + 100, y + 16
        self.vela.input("move", grab_x, grab_y, "sleep", "50", "down", "sleep", "50",
                        "move", grab_x - 200, grab_y + 100, "sleep", "50", "move", 0, 500, "sleep", "300", "up")
        self.vela.wait_for(lambda s: s.window(window)["snap"] == [0, 0, 6, 12], what="snapped left")
        line = self.shell.wait_for("snap-assist ")
        self.assertIn(f'"window":"{window}"', line)
        self.assertIn(f'"candidates":["{other}"]', line)

    def test_snap_layouts_and_groups(self):
        first = self.vela.open_window(401, 301)
        second = self.vela.open_window(300, 200)
        self.vela.keys("super+z")
        line = self.shell.wait_for("snap-layouts ")
        self.assertEqual(line.split()[1:3], [second, "HEADLESS-1"])
        self.assertEqual(line.split()[-1], "1")
        # As from Snap Assist: the first on the left, the second next to it.
        self.vela.command(f"window {first} snap 0 0 6 12")
        self.vela.command(f"window {second} snap 6 0 12 12 quiet {first}")
        state = self.vela.wait_for(lambda s: s.window(second)["snapGroup"] != 0, what="a snap group")
        self.assertEqual(state.window(first)["snapGroup"], state.window(second)["snapGroup"])
        # The shell may still have an older "workspaces" line, without the
        # group: wait for the one that has it.
        self.shell.wait_for("workspaces ", containing='"snapGroups":[{"output":"HEADLESS-1"')
        # The group from the taskbar: all forward, the chosen one focused.
        self.vela.command(f"window {first} activate-group")
        self.vela.wait_for(lambda s: s["focused"] == first, what="the chosen window focused")
        # Moving one away ends the group of two.
        self.vela.command(f"window {second} restore")
        self.vela.wait_for(lambda s: s.window(first)["snapGroup"] == 0, what="the group dissolved")


if __name__ == "__main__":
    unittest.main()
