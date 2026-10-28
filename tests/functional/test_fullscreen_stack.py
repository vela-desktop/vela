# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Fullscreen visibility regressions: check pixels, not only keyboard focus."""

import subprocess
import time
import unittest

from harness import Session, tool


def game(vela, x11=False):
    if x11:
        # Xwayland is a separate display: the Wayland-only harness environment
        # intentionally has no DISPLAY. Reuse the discovery used by test_x11.
        display = vela.wait_for_log("Xwayland on DISPLAY=",
                                    lambda line: line.split("DISPLAY=", 1)[1].split()[0])
        env = dict(vela.client_env, DISPLAY=display)
        command = [tool("vela-x11"), "--start-fullscreen", "640", "480"]
    else:
        env = None
        command = [tool("vela-pattern"), "--fullscreen-parent", "640", "480"]
    identifier = vela.open_window(command=command, env=env)
    vela.wait_for(lambda s: s.window(identifier)["fullscreen"] and s["focused"] == identifier,
                  what="fullscreen game focused")
    return identifier


def center(window):
    return int(window["x"] + window["w"] / 2), int(window["y"] + window["h"] / 2)


class FullscreenStack(unittest.TestCase):
    def test_new_app_is_visibly_above_fullscreen(self):
        with Session(size="1280x720@60") as vela:
            fullscreen = game(vela)
            before = vela.pixels()
            app = vela.open_window(320, 240)
            state = vela.wait_for(lambda s: s["focused"] == app, what="new app focused")
            x, y = center(state.window(app))
            self.assertTrue(state.window(fullscreen)["fullscreen"])
            self.assertNotEqual(vela.pixels().at(x, y), before.at(x, y))

    def test_alt_tab_shows_app_then_restores_fullscreen(self):
        with Session(size="1280x720@60") as vela:
            app = vela.open_window(320, 240)
            fullscreen = game(vela)
            x, y = center(vela.state().window(app))
            game_pixel = vela.pixels().at(x, y)
            vela.keys("alt+Tab")
            vela.wait_for(lambda s: s["focused"] == app, what="Alt+Tab to app")
            self.assertTrue(vela.state().window(fullscreen)["fullscreen"])
            self.assertNotEqual(vela.pixels().at(x, y), game_pixel)
            vela.keys("alt+Tab")
            vela.wait_for(lambda s: s["focused"] == fullscreen, what="Alt+Tab to game")
            self.assertEqual(vela.pixels().at(x, y), game_pixel)

    def test_background_windows_stay_visible_when_game_loses_foreground(self):
        with Session(size="1280x720@60") as vela:
            first = vela.open_window(220, 180, decorated=True)
            w = vela.state().window(first)
            vela.input("move", w["x"] + 40, w["y"] + 35,
                       "keydown", "super", "down", "move", 190, 160,
                       "sleep", "100", "up", "keyup", "super")
            vela.wait_still(first)
            second = vela.open_window(220, 180)
            first_point = center(vela.state().window(first))
            second_point = center(vela.state().window(second))
            self.assertNotEqual(first_point, second_point)
            fullscreen = game(vela)
            baseline = vela.pixels()
            vela.keys("alt+Tab")
            state = vela.wait_for(lambda s: s["focused"] == second, what="second app focused")
            self.assertTrue(state.window(fullscreen)["fullscreen"])
            # A foreground app must not be the only one visible: other
            # already-open, nonoverlapping windows also sit above the game.
            self.assertNotEqual(vela.pixels().at(*first_point), baseline.at(*first_point))
            self.assertNotEqual(vela.pixels().at(*second_point), baseline.at(*second_point))

    def test_start_panel_exposes_taskbar_then_returns_to_game(self):
        with Session(size="1280x720@60") as vela:
            taskbar = subprocess.Popen([tool("vela-panel"), "taskbar"], env=vela.client_env)
            vela.clients.append(taskbar)
            time.sleep(0.4)
            fullscreen = game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)
            # TOP/ON_DEMAND layer surface models the actual Start menu, whose
            # mapping should also reveal the passive TOP taskbar on this output.
            menu = subprocess.Popen([tool("vela-panel"), "menu"], env=vela.client_env)
            vela.clients.append(menu)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and vela.pixels().at(x, y) == immersive:
                    self.assertIsNone(menu.poll(), "test menu exited before mapping")
                    time.sleep(0.08)
                self.assertNotEqual(vela.pixels().at(x, y), immersive)
                self.assertTrue(vela.state().window(fullscreen)["fullscreen"])
            finally:
                menu.terminate()
                menu.wait(timeout=3)
            vela.wait_for(lambda s: s["focused"] == fullscreen, what="game focused after Start closes")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and vela.pixels().at(x, y) != immersive:
                time.sleep(0.08)
            self.assertEqual(vela.pixels().at(x, y), immersive)

    def test_xwayland_fullscreen_allows_visible_wayland_app(self):
        with Session(size="1280x720@60") as vela:
            fullscreen = game(vela, x11=True)
            before = vela.pixels()
            app = vela.open_window(300, 200)
            state = vela.wait_for(lambda s: s["focused"] == app, what="app above X11 game")
            self.assertTrue(state.window(fullscreen)["fullscreen"])
            x, y = center(state.window(app))
            self.assertNotEqual(vela.pixels().at(x, y), before.at(x, y))

    def test_secondary_window_crosses_fullscreen_output(self):
        with Session(size="1280x720@60") as vela:
            vela.command("test-output add 1280x720")
            state = vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two monitors")
            secondary = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
            app = vela.open_window(320, 240, decorated=True)
            w = vela.state().window(app)
            src_x, src_y = w["x"] + 50, w["y"] + 40
            dest_x, dest_y = secondary["x"] + 240, secondary["y"] + 140
            vela.input("move", src_x, src_y, "keydown", "super", "down",
                       "move", dest_x, dest_y, "sleep", "100", "up", "keyup", "super")
            vela.wait_for(lambda s: s.window(app)["output"] == "HEADLESS-2",
                          what="app moved to second monitor")
            vela.input("move", 20, 20)
            fullscreen = game(vela)
            self.assertEqual(vela.state().window(fullscreen)["output"], "HEADLESS-1")
            baseline = vela.pixels()
            w = vela.state().window(app)
            vela.input("move", w["x"] + 70, w["y"] + 80, "down", "up")
            vela.wait_for(lambda s: s["focused"] == app, what="app focused on secondary")
            self.assertEqual(vela.pixels().at(450, 350), baseline.at(450, 350))
            w = vela.state().window(app)
            drag = subprocess.Popen([
                tool("vela-input"), "move", str(w["x"] + 70), str(w["y"] + 50),
                "keydown", "super", "down", "move", "450", "350",
                "sleep", "2000", "up", "keyup", "super"
            ], env=vela.client_env)
            try:
                state = vela.wait_for(lambda s: s.window(app)["output"] == "HEADLESS-1",
                                      what="app dragged over fullscreen game")
                x, y = center(state.window(app))
                self.assertIsNone(drag.poll(), "must inspect pixels during the drag, not after dropping")
                self.assertEqual(state["focused"], app)
                self.assertTrue(state.window(fullscreen)["fullscreen"])
                self.assertNotEqual(vela.pixels().at(x, y), baseline.at(x, y))
            finally:
                self.assertEqual(drag.wait(timeout=4), 0)
            self.assertNotEqual(vela.pixels().at(x, y), baseline.at(x, y))
            vela.input("move", 10, 10, "down", "up")
            vela.wait_for(lambda s: s["focused"] == fullscreen, what="game refocused")
            self.assertEqual(vela.pixels().at(450, 350), baseline.at(450, 350))


if __name__ == "__main__":
    unittest.main()
