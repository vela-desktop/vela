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

    def test_x11_trace_can_be_enabled_without_restarting(self):
        self.assertEqual(self.vela.query("trace-x11"), [])
        self.vela.command("trace-x11 on")
        self.assertEqual(self.vela.query("trace-x11"), [])
        self.open_x11(300, 200)
        events = self.vela.query("trace-x11")
        self.assertTrue(any("event=map" in event for event in events), events)
        self.assertTrue(any("event=buffer-size" in event for event in events), events)
        self.assertTrue(all("xid=0x" in event for event in events), events)
        self.vela.command("trace-x11 off")
        self.assertTrue(self.vela.query("trace-x11"))

    def test_menu_stays_where_the_app_puts_it(self):
        self.open_x11("--menu", "100,120", 300, 200)
        time.sleep(0.4)
        image = self.vela.pixels()
        self.assertEqual(image.at(160, 160), GREEN)
        self.assertEqual(image.at(105, 125), GREEN)
        self.assertNotEqual(image.at(95, 160), GREEN)


if __name__ == "__main__":
    unittest.main()
