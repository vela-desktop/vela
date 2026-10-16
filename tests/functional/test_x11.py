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

    def x11_geometry(self, xid):
        # Query the X11 server, not the Wayland compositor's scene geometry.
        # They can disagree when Vela resizes the X11 window *before* moving
        # its scene node into the final fullscreen/restored position.
        result = subprocess.check_output([tool("vela-x11"), "--geometry", xid],
                                         env=self.env, text=True)
        return tuple(map(int, result.split()))

    def assert_x11_geometry_matches(self, view_id, xid, expected_size=True):
        def matches(state):
            w = state.window(view_id)
            x, y, width, height = self.x11_geometry(xid)
            # Vela's frame rectangle can include its server-side title bar.
            # The X11 surface top-left must match horizontally, while in
            # fullscreen (no title bar) it must also match vertically.
            return x == w["x"] and (not expected_size or width == w["w"]) and (
                not w["fullscreen"] or y == w["y"])
        self.vela.wait_for(matches, timeout=3, what="X11 root coordinates agree with Vela scene")

    def test_x11_fullscreen_on_primary_with_secondary_above(self):
        # Vela's Wayland output space can use negative positions, but Wine's
        # XRandR root must have an origin at zero and include both displays.
        # The previously failing layout exposed (0,-1080) to XWayland and
        # left Isaac rendering at the upper monitor's 1920x1080 resolution.
        path = os.path.join(self.vela.directory, "negative-output-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000",
                                "--start-fullscreen", 300, 200)
        with open(path, encoding="ascii") as f:
            xid = f.read().strip()
        self.vela.command("test-output add 1920x1080")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="second Xwayland output")
        self.vela.randr("--output", "HEADLESS-2", "--pos", "0,-1080", "--scale", "1")
        state = self.vela.wait_for(lambda s: any(
            o["name"] == "HEADLESS-1" and o["y"] == 1080 for o in s["outputs"]),
            what="root-aligned output positions")
        self.assertEqual((state.window(view_id)["x"], state.window(view_id)["y"]), (0, 1080))
        self.vela.wait_for(lambda s: subprocess.check_output(
            [tool("vela-x11"), "--root-size"], env=self.env, text=True).strip() == "1920 2160",
            what="Xwayland root covers both displays")
        self.assertEqual(self.x11_geometry(xid), (0, 1080, 1920, 1080))
        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.vela.wait_for(lambda s: not s.window(view_id)["fullscreen"], what="windowed above-layout Wine")
        self.x11_control("--set-wine-style", xid, "0x96000000")
        state = self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"],
                                   what="fullscreen above-layout Wine")
        self.assertEqual((state.window(view_id)["x"], state.window(view_id)["y"]), (0, 1080))
        self.assertEqual(self.x11_geometry(xid), (0, 1080, 1920, 1080))

    def test_wine_popup_to_decorated_restores_fullscreen(self):
        # Wine can toggle a Windows fullscreen popup into a normal decorated
        # window without sending an EWMH fullscreen request or X11 resize.
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda state: state.window(view_id)["fullscreen"], what="fullscreen popup")
        self.assert_x11_geometry_matches(view_id, xid)

        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.vela.wait_for(lambda state: not state.window(view_id)["fullscreen"],
                           what="Wine changed popup to decorated window")
        self.assertLess(self.vela.state().window(view_id)["w"], self.vela.state().output["w"])
        self.assert_x11_geometry_matches(view_id, xid)

        self.x11_control("--set-wine-style", xid, "0x96000000")
        self.vela.wait_for(lambda state: state.window(view_id)["fullscreen"],
                           what="Wine changed decorated window to fullscreen popup")
        # Regression: previously the fullscreen buffer was configured at the
        # OLD centered windowed coordinates while the scene node moved to 0,0.
        self.assert_x11_geometry_matches(view_id, xid)

    def test_managed_x11_maximize_and_restore_sync_native_position(self):
        path = os.path.join(self.vela.directory, "maximize-xid")
        view_id = self.open_x11("--id-file", path, 400, 320)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.vela.keys("super+Up")
        self.vela.wait_for(lambda state: state.window(view_id)["maximized"], what="maximized X11 window")
        self.assert_x11_geometry_matches(view_id, xid)
        self.vela.keys("super+Down")
        self.vela.wait_for(lambda state: not state.window(view_id)["maximized"], what="restored X11 window")
        self.assert_x11_geometry_matches(view_id, xid)

    def assert_fullscreen_remains(self, view_id, expected):
        # A single immediate state read can predate the asynchronous XWM
        # PropertyNotify. Observe the state across multiple compositor ticks.
        for _ in range(15):
            self.assertEqual(self.vela.state().window(view_id)["fullscreen"], expected)
            time.sleep(0.03)

    def test_wine_redundant_fullscreen_off_does_not_disarm_return(self):
        # Wine may echo an EWMH "off" when the compositor is already windowed.
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"], what="fullscreen Wine popup")
        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.vela.wait_for(lambda s: not s.window(view_id)["fullscreen"], what="windowed Wine state")
        self.x11_control("--fullscreen", xid, "off")
        self.assert_fullscreen_remains(view_id, False)
        self.x11_control("--set-wine-style", xid, "0x96000000")
        self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"],
                           what="redundant EWMH off preserves Wine fullscreen return")

    def test_wine_intermediate_style_does_not_disarm_return(self):
        # Between WS_OVERLAPPEDWINDOW and WS_POPUP Wine may publish a style
        # which is neither decorated nor popup.
        path = os.path.join(self.vela.directory, "wine-xid")
        view_id = self.open_x11("--id-file", path, "--wine-style", "0x96000000", 300, 200)
        with open(path, encoding="ascii") as xids:
            xid = xids.read().strip()
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"], what="fullscreen popup")
        self.x11_control("--set-wine-style", xid, "0x16cf0000")
        self.vela.wait_for(lambda s: not s.window(view_id)["fullscreen"], what="windowed state")
        self.x11_control("--set-wine-style", xid, "0x16000000")
        self.assert_fullscreen_remains(view_id, False)
        self.x11_control("--set-wine-style", xid, "0x96000000")
        self.vela.wait_for(lambda s: s.window(view_id)["fullscreen"],
                           what="Wine popup return after intermediate style")

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
        self.assert_fullscreen_remains(view_id, False)
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

    def test_screen_sized_override_redirect_stays_below_the_taskbar(self):
        """An override-redirect window as large as the screen, opened after
        the app's window (the Steam overlay over a game), neither covers the taskbar nor keeps the
        keyboard: the app's window keeps it and Alt+Tab still switches."""
        self.vela.clients.append(subprocess.Popen([tool("vela-panel"), "taskbar"], env=self.vela.client_env))
        time.sleep(0.4)
        window = self.open_x11("--cover", 300, 200)
        time.sleep(0.4)
        state = self.vela.wait_for(lambda s: s["focused"] == window, what="the X11 window keeps the keyboard")
        out = state.output
        image = self.vela.pixels()
        self.assertEqual(image.at(out["w"] // 2, out["h"] // 2), GREEN)  # it is shown
        self.assertNotEqual(image.at(out["w"] // 2, out["h"] - 24), GREEN)  # the taskbar is above it
        other = self.vela.open_window(300, 200)
        self.vela.wait_for(lambda s: s["focused"] == other, what="the new window has focus")
        self.vela.keys("alt+Tab")
        self.vela.wait_for(lambda s: s["focused"] == window, what="Alt+Tab back to the X11 window")


if __name__ == "__main__":
    unittest.main()
