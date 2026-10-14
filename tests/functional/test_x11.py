# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""X11 apps through Xwayland (xwayland.c): a managed window is a window like
any other (centered, with Vela's title bar, maximized and closed from the
keyboard); an override-redirect menu stays where the app puts it. The
window is tools/vela-x11, which draws only on Expose; Xwayland starts with
it, so its window is created while the X window manager is being set up."""

import os
import subprocess
import time
import unittest

from harness import Session, tool

ORANGE = (255, 128, 0)
GREEN = (0, 192, 0)


class X11(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(tool("vela-x11")):
            self.skipTest("vela-x11 not built (needs xcb)")
        self.vela = Session()
        self.vela.start()
        if "Xwayland not available" in self.vela.log_text():
            self.vela.stop()
            self.skipTest("Xwayland not available")
        display = self.vela.wait_for_log("Xwayland on DISPLAY=", lambda line: line.split("DISPLAY=")[1].split()[0])
        self.env = dict(self.vela.client_env, DISPLAY=display)

    def tearDown(self):
        self.vela.stop()

    def open_x11(self, *args):
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen([tool("vela-x11"), *map(str, args)], env=self.env))
        state = self.vela.wait_for(lambda s: {w["id"] for w in s.windows} - before, timeout=15,
                                   what="the X11 window")
        window = next(iter({w["id"] for w in state.windows} - before))
        self.vela.wait_still(window)
        return window

    def test_window_is_managed(self):
        window = self.open_x11(300, 200)
        state = self.vela.wait_for(lambda s: s["focused"] == window, what="the X11 window has focus")
        w = state.window(window)
        out = state.output
        self.assertEqual((w["app"], w["title"]), ("vela.x11", "vela-x11"))
        self.assertEqual(w["w"], 300)
        self.assertEqual(w["x"], out["x"] + (out["w"] - 300) // 2)
        # The app's content below Vela's title bar.
        image = self.vela.pixels()
        self.assertEqual(image.at(w["x"] + 150, w["y"] + w["h"] - 60), ORANGE)

        self.vela.keys("super+Up")
        state = self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")
        self.assertEqual(state.window(window)["w"], out["w"])
        self.vela.keys("super+Down")
        state = self.vela.wait_for(lambda s: not s.window(window)["maximized"], what="restored")
        self.assertEqual(state.window(window)["w"], 300)

        self.vela.keys("alt+F4")
        self.vela.wait_for(lambda s: not s.has(window), what="the X11 window closed")

    def x11_control(self, *args):
        subprocess.run([tool("vela-x11"), *map(str, args)], env=self.env, check=True)

    def test_wine_popup_to_decorated_restores_fullscreen(self):
        # Wine can toggle a Windows fullscreen popup into a normal decorated
        # window without sending an EWMH fullscreen request or X11 resize.
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda state: state.window(view_id)["fullscreen"], what="fullscreen popup")

        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.vela.wait_for(lambda state: not state.window(view_id)["fullscreen"],
                           what="Wine changed popup to decorated window")
        self.assertLess(self.vela.state().window(view_id)["w"], self.vela.state().output["w"])

        self.x11_control("--set-wine-style", xid, "0x96000000")
        self.vela.wait_for(lambda state: state.window(view_id)["fullscreen"],
                           what="Wine changed decorated window to fullscreen popup")

    def assert_fullscreen_remains(self, view_id, expected):
        # A single immediate state read can predate the asynchronous XWM
        # PropertyNotify. Observe the state across multiple compositor ticks.
        for _ in range(15):
            self.assertEqual(self.vela.state().window(view_id)["fullscreen"], expected)
            time.sleep(0.03)

    def test_wine_initial_fullscreen_size_restores_to_window(self):
        path = os.path.join(self.vela.directory, "wine-xid")
        output = self.vela.state().output
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000",
                                "--start-fullscreen", output["w"], output["h"])
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.vela.wait_for(lambda state: state.window(view_id)["fullscreen"],
                           what="window launched in fullscreen")
        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        state = self.vela.wait_for(lambda s: not s.window(view_id)["fullscreen"],
                                   what="fullscreen game became a window")
        self.assertLess(state.window(view_id)["w"], output["w"])
        self.assertLess(state.window(view_id)["h"], output["h"])

    def test_wine_popup_is_not_automatically_fullscreen(self):
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x16cf0000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--set-wine-style", xid, "0x96000000")
        self.assert_fullscreen_remains(view_id, False)

    def test_wine_manual_fullscreen_override_clears_restore_hint(self):
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"], what="fullscreen Wine window")
        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.vela.wait_for(lambda s: not s.window(view_id)["fullscreen"],
                           what="Wine-driven fullscreen exit")

        # A later explicit fullscreen on/off must disarm the old Wine hint.
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"], what="manual fullscreen on")
        self.x11_control("--fullscreen", xid, "off")
        self.vela.wait_for(lambda s: not s.window(view_id)["fullscreen"], what="manual fullscreen off")
        self.x11_control("--set-wine-style", xid, "0x96000000")
        self.assert_fullscreen_remains(view_id, False)

    def test_wine_deleted_style_invalidates_transition(self):
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--delete-wine-style", xid)
        # Do not infer a transition from a missing/unknown previous style.
        time.sleep(0.1)
        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.assert_fullscreen_remains(view_id, False)

    def test_wine_non_popup_changes_do_not_override_fullscreen(self):
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x16cf0000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda state: state.window(view_id)["fullscreen"], what="fullscreen window")
        # Changing unrelated style bits must not cancel fullscreen.
        self.x11_control("--set-wine-style", xid, "0x16cf0008")
        self.assert_fullscreen_remains(view_id, True)

    def test_menu_stays_where_the_app_puts_it(self):
        self.open_x11("--menu", "100,120", 300, 200)
        time.sleep(0.4)
        image = self.vela.pixels()
        self.assertEqual(image.at(160, 160), GREEN)
        self.assertEqual(image.at(105, 125), GREEN)
        self.assertNotEqual(image.at(95, 160), GREEN)


if __name__ == "__main__":
    unittest.main()
