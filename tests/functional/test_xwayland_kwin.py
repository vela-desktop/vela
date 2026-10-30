# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Experimental KWin-style integrated XWayland acceptance test.

Never run automatically against the stable compatibility backend. The per-X11
xdg-output adapter and scene transforms are in development; developers enable
these strict checks only when investigating the new path on a GPU runner.
"""
import os
import re
import subprocess
import time
import unittest

from harness import Session, tool


class KwinStyleX11(unittest.TestCase):
    def setUp(self):
        if os.environ.get("VELA_ASSERT_KWIN_XWAYLAND") != "1":
            self.skipTest("KWin-style native XWayland protocol mode is experimental")
        if not os.path.exists(tool("vela-x11")):
            self.skipTest("vela-x11 requires the XCB test package")
        self.vela = Session(
            scale="HEADLESS-1=1.25,HEADLESS-2=1",
            size="2560x1440@60",
            env={"VELA_XWAYLAND_BACKEND": "compat",
                 "VELA_XWAYLAND_XDG_OUTPUT_EXPERIMENT": "1",
                 "VELA_XWAYLAND_SCALE_TRACE": "1"})
        self.vela.start()
        self.vela.command("test-output add 1920x1080")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 2,
                           what="second mixed-scale Wayland output")
        self.vela.randr("--output", "HEADLESS-2", "--scale", "1",
                        "--pos", "2048,0")
        self.vela.wait_for(lambda s: any(
            o["name"] == "HEADLESS-2" and o["scale"] == 1
            for o in s["outputs"]), what="second output at 100 percent")
        display = self.vela.wait_for_log(
            "Xwayland on DISPLAY=", lambda line: line.split("DISPLAY=")[1].split()[0])
        self.env = dict(self.vela.client_env, DISPLAY=display)

    def tearDown(self):
        if hasattr(self, "vela"):
            self.vela.stop()

    def test_native_modes_and_pointer_stay_aligned(self):
        # Trigger lazy XWayland with a normal 320x240 X11 window.
        xid_path = os.path.join(self.vela.directory, "xid")
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen(
            [tool("vela-x11"), "--id-file", xid_path, "320", "240"], env=self.env))
        self.vela.wait_for(lambda state: bool(
            {w["id"] for w in state.windows} - before),
            timeout=15, what="managed X11 window")
        window = next(iter({w["id"] for w in self.vela.state().windows} - before))
        self.vela.wait_still(window)

        deadline = time.monotonic() + 8
        while not os.path.exists(xid_path) and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertTrue(os.path.exists(xid_path))
        with open(xid_path, encoding="ascii") as file:
            xid = file.read().strip()

        # A 125% 2560x1440 output should expose the *physical* dimensions.
        output = subprocess.check_output(["xrandr", "--query"], env=self.env, text=True)
        self.assertRegex(output, r"(?m)^\s+2560x1440\s")
        self.assertRegex(self.vela.log_text(),
                         r"Experimental KWin-style per-XWayland-client xdg-output adapter")

        # A normal client 320px wide occupies 256 Wayland logical units.
        state = self.vela.state()
        self.assertAlmostEqual(state.window(window)["w"], 256, delta=2)

        def x11_pointer(x, y):
            self.vela.input("move", x, y, "sleep", "100")
            query = subprocess.check_output(
                [tool("vela-x11"), "--pointer", xid], env=self.env, text=True)
            sx, sy, same_screen = map(int, query.split())
            self.assertEqual(same_screen, 1)
            return sx, sy

        view = state.window(window)
        cx, cy = view["x"] + view["w"] // 2, view["y"] + view["h"] // 2
        left = x11_pointer(cx - 20, cy)
        right = x11_pointer(cx + 20, cy)
        self.assertAlmostEqual(right[0] - left[0], 50, delta=2,
                               msg="125% X11 pointer coords disagree with rendered content")
        self.vela.keys("super+Up")
        self.vela.wait_for(lambda s: s.window(window)["maximized"],
                           what="X11 window maximized through integrated XWM")
        self.vela.keys("super+Down")
        self.vela.wait_for(lambda s: not s.window(window)["maximized"],
                           what="X11 window restored through integrated XWM")
