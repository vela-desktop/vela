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


def layer_mapped(state, namespace):
    return any(item["namespace"] == namespace and item["mapped"] for item in state["layers"])


def panel(vela, kind, namespace):
    proc = subprocess.Popen([tool("vela-panel"), kind, "--namespace", namespace], env=vela.client_env)
    vela.clients.append(proc)
    vela.wait_for(lambda s: layer_mapped(s, namespace), what=f"{namespace} mapped")
    return proc


def wait_pixel_change(vela, x, y, baseline, timeout=0.45):
    # Start before waiting for shell IPC: the speculative preview lasts only
    # 500 ms, which may elapse while FakeShell's reader thread is scheduled.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pixel = vela.pixels().at(x, y)
        if pixel != baseline:
            return pixel
        time.sleep(0.01)
    raise AssertionError("the taskbar was never visible during Start preview")


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

    def test_covered_window_border_does_not_resize_under_game(self):
        # The game sits in the fullscreen layer, above the windows layer that
        # resize_border_at() walked: a drag on the invisible border of a window
        # under it must neither resize that window nor focus it. The arrows on
        # hover come from the same function; the state doesn't show the cursor.
        for x11 in (False, True):
            with self.subTest(x11=x11), Session(size="1280x720@60") as vela:
                window = vela.open_window(300, 200, decorated=True)
                before = vela.state().window(window)
                fullscreen = game(vela, x11=x11)
                grab_x, grab_y = before["x"] + before["w"] + 4, before["y"] + before["h"] // 2

                def drag():
                    vela.input("move", grab_x, grab_y, "sleep", "50", "down", "sleep", "50",
                               "move", grab_x + 50, grab_y, "sleep", "50", "move", grab_x + 100, grab_y,
                               "sleep", "100", "up")

                drag()
                time.sleep(0.2)
                state = vela.state()
                self.assertEqual(state.window(window)["w"], before["w"])
                self.assertEqual(state["focused"], fullscreen)
                self.assertTrue(state.window(fullscreen)["fullscreen"])
                # In front of the game again, the window has its border back.
                vela.keys("alt+Tab")
                vela.wait_for(lambda s: s["focused"] == window, what="Alt+Tab to the window")
                drag()
                vela.wait_for(lambda s: s.window(window)["w"] == before["w"] + 100, what="100 wider")

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

    def test_minimizing_unfocused_fullscreen_promotes_next_game(self):
        with Session(size="1280x720@60") as vela:
            vela.command("test-output add 1280x720")
            state = vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two monitors")
            secondary = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
            panel(vela, "taskbar", "fullscreen-taskbar")
            vela.input("move", 20, 20)
            first = game(vela)
            second = game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)
            vela.input("move", secondary["x"] + 200, secondary["y"] + 200)
            other = vela.open_window(240, 150)
            vela.wait_for(lambda s: s["focused"] == other, what="secondary app focused")
            self.assertEqual(vela.pixels().at(x, y), immersive)
            # The global keyboard focus stays on output 2 while output 1's
            # frontmost fullscreen view is minimized through the window API.
            vela.command(f"window {second} minimize")
            vela.wait_for(lambda s: s.window(second)["minimized"],
                          what="nonfocused fullscreen minimized")
            time.sleep(0.4)  # let the minimizing snapshot animation retire
            self.assertTrue(vela.state().window(first)["fullscreen"])
            self.assertEqual(vela.state()["focused"], other)
            self.assertEqual(vela.pixels().at(x, y), immersive)

    def test_super_reveals_taskbar_before_start_surface_maps(self):
        # A fake shell deliberately ACKs the toggle but never maps Start:
        # the taskbar must be painted *before* Qt supplies a menu buffer.
        # If the shell never maps the menu, the game becomes immersive again.
        with Session(size="1280x720@60") as vela:
            panel(vela, "taskbar", "fullscreen-taskbar")
            fullscreen = game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)

            # No reachable shell: do not leave the game exposed.
            vela.keys("super")
            self.assertEqual(vela.pixels().at(x, y), immersive)

            shell = vela.fake_shell()
            vela.keys("super")
            wait_pixel_change(vela, x, y, immersive)
            shell.wait_for("toggle-start")
            self.assertTrue(vela.state().window(fullscreen)["fullscreen"])

            # A shell failing to map must never strand the taskbar over a game.
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline and vela.pixels().at(x, y) != immersive:
                time.sleep(0.05)
            self.assertEqual(vela.pixels().at(x, y), immersive)

            # Super chords (including shortcuts and Super+drag) must NOT flash
            # the taskbar; only a completed Super tap can request the preview.
            shell.clear()
            vela.keys("super+z")
            vela.state()  # compositor-side roundtrip before socket-reader barrier
            shell.barrier()
            self.assertEqual(shell.received("toggle-start"), [])
            self.assertEqual(vela.pixels().at(x, y), immersive)

    def test_two_super_taps_restore_fullscreen_without_waiting_for_timeout(self):
        with Session(size="1280x720@60") as vela:
            panel(vela, "taskbar", "fullscreen-taskbar")
            fullscreen = game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)
            shell = vela.fake_shell()
            vela.keys("super")
            wait_pixel_change(vela, x, y, immersive)
            vela.keys("super")
            shell.barrier()
            self.assertEqual(len(shell.received("toggle-start")), 2)
            self.assertEqual(vela.pixels().at(x, y), immersive)
            self.assertTrue(vela.state().window(fullscreen)["fullscreen"])

    def test_clicking_fullscreen_game_after_start_restores_immersive_layer(self):
        with Session(size="1280x720@60") as vela:
            panel(vela, "taskbar", "fullscreen-taskbar")
            fullscreen = game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)
            menu = panel(vela, "menu", "fullscreen-menu")
            try:
                vela.wait_for(lambda s: s["focusedLayer"] == "fullscreen-menu", what="menu focus")
                self.assertNotEqual(vela.pixels().at(x, y), immersive)
                vela.input("move", 10, 10, "down", "up")
                vela.wait_for(lambda s: s["focused"] == fullscreen and s["focusedLayer"] is None,
                              what="game regains focus after menu")
                self.assertEqual(vela.pixels().at(x, y), immersive)
            finally:
                menu.terminate()
                menu.wait(timeout=3)

    def test_system_prompt_stays_visible_over_reactivated_fullscreen(self):
        with Session(size="1280x720@60") as vela:
            fullscreen = game(vela)
            prompt = vela.open_window(280, 180, app_id="pinentry")
            x, y = center(vela.state().window(prompt))
            baseline = vela.pixels().at(x, y)
            vela.input("move", 10, 10, "down", "up")
            vela.wait_for(lambda s: s["focused"] == fullscreen, what="game receives click")
            self.assertEqual(vela.pixels().at(x, y), baseline)

    def test_new_app_on_secondary_does_not_demote_primary_game(self):
        with Session(size="1280x720@60") as vela:
            vela.command("test-output add 1280x720")
            state = vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two monitors")
            secondary = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
            panel(vela, "taskbar", "fullscreen-taskbar")
            vela.input("move", 20, 20)
            fullscreen = game(vela)
            primary = vela.pixels()
            empty = vela.pixels(output=1)
            vela.input("move", secondary["x"] + 200, secondary["y"] + 200)
            app = vela.open_window(320, 240)
            state = vela.wait_for(lambda s: s["focused"] == app, what="new app on secondary")
            self.assertEqual(state.window(fullscreen)["output"], "HEADLESS-1")
            # Probe where the taskbar would show through if fullscreen were
            # incorrectly demoted because the new app started at (0, 0).
            self.assertEqual(vela.pixels().at(24, 708), primary.at(24, 708))
            x, y = center(state.window(app))
            x -= secondary["x"]
            y -= secondary["y"]
            self.assertNotEqual(vela.pixels(output=1).at(x, y), empty.at(x, y))

    def test_super_preview_survives_until_start_menu_closes(self):
        with Session(size="1280x720@60") as vela:
            panel(vela, "taskbar", "fullscreen-taskbar")
            game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)
            shell = vela.fake_shell()
            vela.keys("super")
            wait_pixel_change(vela, x, y, immersive)
            shell.wait_for("toggle-start")
            menu = panel(vela, "menu", "fullscreen-menu")
            try:
                vela.wait_for(lambda s: s["focusedLayer"] == "fullscreen-menu",
                              what="Start mapped and keyboard-focused")
                # Verify *after* preview's 500 ms fallback: the mapped menu
                # must keep the taskbar visible on its own.
                time.sleep(0.65)
                self.assertNotEqual(vela.pixels().at(x, y), immersive)
            finally:
                menu.terminate()
                menu.wait(timeout=3)
            vela.wait_for(lambda s: s["focused"] is not None,
                          what="keyboard focus restored to the game")
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline and vela.pixels().at(x, y) != immersive:
                time.sleep(0.05)
            self.assertEqual(vela.pixels().at(x, y), immersive)

    def test_start_panel_exposes_taskbar_then_returns_to_game(self):
        with Session(size="1280x720@60") as vela:
            panel(vela, "taskbar", "fullscreen-taskbar")
            fullscreen = game(vela)
            x, y = 24, vela.state().output["h"] - 12
            immersive = vela.pixels().at(x, y)
            # TOP/ON_DEMAND layer surface models the actual Start menu, whose
            # mapping should also reveal the passive TOP taskbar on this output.
            menu = panel(vela, "menu", "fullscreen-menu")
            try:
                vela.wait_for(lambda s: s["focusedLayer"] == "fullscreen-menu",
                              what="Start has the keyboard")
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
            empty_secondary = vela.pixels(output=1)
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
            secondary_pixel = vela.pixels(output=1)
            w = vela.state().window(app)
            vela.input("move", w["x"] + 70, w["y"] + 80, "down", "up")
            vela.wait_for(lambda s: s["focused"] == app, what="app focused on secondary")
            self.assertEqual(vela.pixels().at(450, 350), baseline.at(450, 350))
            w = vela.state().window(app)
            sx, sy = center(w)
            local_x, local_y = sx - secondary["x"], sy - secondary["y"]
            # The already-open app must actually be visible on output 1,
            # not merely unchanged there while a different output is sampled.
            self.assertNotEqual(secondary_pixel.at(local_x, local_y),
                                empty_secondary.at(local_x, local_y))
            self.assertEqual(vela.pixels(output=1).at(local_x, local_y),
                             secondary_pixel.at(local_x, local_y))
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
