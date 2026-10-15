# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Vela's title bar (docs/renderer.md §9) for apps that accept it through
xdg-decoration: 32 high, above the content, with the title written by the
compositor and the three buttons; closing, maximizing with a double click.
It's found on screen, not from the state."""

import os
import unittest

from harness import Session

BAR = 32
BUTTON = 46


class Decorations(unittest.TestCase):
    def setUp(self):
        self.vela = Session()
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def open(self):
        window = self.vela.open_window(400, 300, decorated=True)
        state = self.vela.wait_for(lambda s: s["focused"] == window)
        w = state.window(window)
        image = self.vela.pixels()
        # From the top, in the column halfway along the title: wallpaper and
        # shadow are black, the bar isn't.
        column = w["x"] + 200
        top = next(y for y in range(image.height) if image.at(column, y) != (0, 0, 0))
        return window, w, image, top

    def test_bar_above_the_content(self):
        window, w, image, top = self.open()
        # The app's content starts right below: vela-pattern encodes the pixel
        # coordinates (R = x, G = y).
        self.assertEqual(image.at(w["x"] + 100, top + BAR), (100, 0, 0))
        self.assertEqual(image.at(w["x"] + 100, top + BAR - 1)[:2] != (100, 0), True)
        # The title ("vela-pattern") is written: antialiasing, several colors.
        self.assertGreaterEqual(len(image.colors(w["x"] + 12, top, w["x"] + 200, top + BAR)), 4)
        # And the button glyphs.
        for i in range(1, 4):
            left = w["x"] + w["w"] - i * BUTTON
            self.assertGreaterEqual(len(image.colors(left, top, left + BUTTON, top + BAR)), 2, f"button {i}")

    def test_close_button(self):
        window, w, image, top = self.open()
        self.vela.input("move", w["x"] + w["w"] - BUTTON // 2, top + BAR // 2, "sleep", "50", "click")
        self.vela.wait_for(lambda s: not s.has(window), what="closed by its close button")

    def test_double_click_on_the_title_maximizes(self):
        window, w, image, top = self.open()
        self.vela.input("move", w["x"] + 150, top + BAR // 2, "sleep", "50", "click", "sleep", "60", "click")
        self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")

    def test_window_without_decoration_request_gets_no_bar(self):
        # Apps that never bind xdg-decoration (kitty) draw borderless, unless
        # listed: the content starts at the very top of the window.
        window = self.vela.open_window(400, 300, app_id="undecorated-app")
        state = self.vela.wait_for(lambda s: s["focused"] == window)
        w = state.window(window)
        image = self.vela.pixels()
        column = w["x"] + 100
        top = next(y for y in range(image.height) if image.at(column, y) != (0, 0, 0))
        self.assertEqual(image.at(column, top), (100, 0, 0))

    def test_listed_window_without_decoration_request_gets_a_bar(self):
        os.makedirs(os.path.dirname(self.vela.config_path()), exist_ok=True)
        with open(self.vela.config_path(), "w") as f:
            f.write("decoration-ssd=listed-app\n")
        self.vela.command("reload-config")
        window = self.vela.open_window(400, 300, app_id="listed-app")
        state = self.vela.wait_for(lambda s: s["focused"] == window)
        w = state.window(window)
        image = self.vela.pixels()
        # The bar is there: 32 high, the content starts right below it.
        column = w["x"] + 100
        top = next(y for y in range(image.height) if image.at(column, y) != (0, 0, 0))
        self.assertEqual(image.at(column, top + BAR), (100, 0, 0))
        self.assertEqual(image.at(column, top + BAR - 1)[:2] != (100, 0), True)


if __name__ == "__main__":
    unittest.main()
