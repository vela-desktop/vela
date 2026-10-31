# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""X11 apps at native resolution (x11_root.c, docs/renderer.md §3.9): on a
2560x1440 output at 125% Xwayland sees 2560x1440, not the 2048x1152 logical
units, so a game finds the native mode in RandR and is drawn 1:1. Every X11
window is in pixels of its output and the apps get Xft.dpi to scale
themselves. The window is tools/vela-x11, with a checkerboard of single
pixels that only a 1:1 copy leaves intact."""

import os
import re
import subprocess
import time
import unittest

from harness import Session, tool

GREEN = (0, 192, 0)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
BAR = 32 # Vela's title bar, logical


def checker(x, y):
    return WHITE if (x + y) % 2 == 0 else BLACK


class X11Native(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(tool("vela-x11")):
            self.skipTest("vela-x11 not built (needs xcb)")
        self.vela = Session(scale=1.25, size="2560x1440@60")
        self.vela.start()
        if "Xwayland not available" in self.vela.log_text():
            self.vela.stop()
            self.skipTest("Xwayland not available")
        display = self.vela.wait_for_log("Xwayland on DISPLAY=", lambda line: line.split("DISPLAY=")[1].split()[0])
        self.env = dict(self.vela.client_env, DISPLAY=display)

    def tearDown(self):
        self.vela.stop()

    def open_x11(self, *args):
        """Opens vela-x11 and returns (view id, X window id, clicks file)."""
        xid_path = os.path.join(self.vela.directory, f"xid-{len(self.vela.clients)}")
        clicks = os.path.join(self.vela.directory, f"clicks-{len(self.vela.clients)}")
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen(
            [tool("vela-x11"), "--id-file", xid_path, "--clicks", clicks, *map(str, args)], env=self.env))
        state = self.vela.wait_for(lambda s: {w["id"] for w in s.windows} - before, timeout=15,
                                   what="the X11 window")
        view = next(iter({w["id"] for w in state.windows} - before))
        self.vela.wait_still(view)
        with open(xid_path, encoding="ascii") as f:
            return view, f.read().strip(), clicks

    def x11(self, *args):
        return subprocess.check_output([tool("vela-x11"), *map(str, args)], env=self.env, text=True)

    def geometry(self, xid):
        return tuple(map(int, self.x11("--geometry", xid).split()))

    def randr_outputs(self):
        """{name: (x, y, width, height, [modes])} as an X11 app sees them."""
        outputs = {}
        current = None
        listing = subprocess.check_output(["xrandr"], env=self.env, text=True, stderr=subprocess.DEVNULL)
        for line in listing.splitlines():
            match = re.match(r"(\S+) connected (?:primary )?(\d+)x(\d+)\+(\d+)\+(\d+)", line)
            if match:
                name, w, h, x, y = match.groups()
                current = outputs[name] = (int(x), int(y), int(w), int(h), [])
            elif current and re.match(r"\s+\d+x\d+", line):
                current[4].append(line.split()[0])
        return outputs

    def wait_geometry(self, xid, expected):
        self.vela.wait_for(lambda s: self.geometry(xid) == expected, timeout=3, read_state=False,
                           what=f"X11 geometry {expected}")

    def clicks(self, path, count):
        """Button event number `count` (presses and releases) of vela-x11."""
        def lines():
            try:
                with open(path, encoding="ascii") as f:
                    return f.read().splitlines()
            except FileNotFoundError:
                return []
        self.vela.wait_for(lambda s: len(lines()) >= count, read_state=False, what=f"{count} X11 clicks")
        return tuple(map(int, lines()[count - 1].split()))

    def needs_xrandr(self):
        try:
            subprocess.run(["xrandr", "--version"], capture_output=True, check=True)
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("xrandr not installed")

    # ------------------------------------------------------------ tests --

    def test_randr_offers_the_native_resolution(self):
        self.needs_xrandr()
        self.open_x11(300, 200)
        outputs = self.randr_outputs()
        self.assertEqual(list(outputs), ["HEADLESS-1"])
        x, y, width, height, modes = outputs["HEADLESS-1"]
        self.assertEqual((x, y, width, height), (0, 0, 2560, 1440))
        self.assertEqual(modes[0], "2560x1440")
        self.assertEqual(self.x11("--root-size").split(), ["2560", "1440"])
        resources = subprocess.check_output(["xrdb", "-query"], env=self.env, text=True)
        self.assertIn("Xft.dpi:\t120", resources)

    def test_fullscreen_game_is_drawn_one_to_one(self):
        view, xid, _ = self.open_x11("--checker", "--start-fullscreen", 640, 480)
        self.vela.wait_for(lambda s: s.window(view)["fullscreen"], what="fullscreen")
        self.wait_geometry(xid, (0, 0, 2560, 1440))
        state = self.vela.state()
        w = state.window(view)
        self.assertEqual((w["x"], w["y"], w["w"], w["h"]), (0, 0, 2048, 1152))
        # A buffer as large as the output, 1:1: straight to the screen (until
        # the capture below, which composites).
        self.vela.wait_for(lambda s: s.output["scanout"], what="direct scanout of the game")
        image = self.vela.pixels()
        wrong = [(x, y) for y in range(0, 1440, 3) for x in range(2560) if image.at(x, y) != checker(x, y)]
        self.assertEqual(wrong[:5], [], f"{len(wrong)} pixels differ from the game's")

    def test_window_is_in_pixels_of_its_output(self):
        view, xid, clicks = self.open_x11("--checker", 400, 300)
        w = self.vela.state().window(view)
        # 400x300 pixels are 320x240 units at 125%, below Vela's bar.
        self.assertEqual((w["w"], w["h"]), (320, 240 + BAR))
        x, y, width, height = self.geometry(xid)
        self.assertEqual((width, height), (400, 300))
        self.assertEqual((x, y), (round(w["x"] * 1.25), round((w["y"] + BAR) * 1.25)))
        time.sleep(0.3)
        image = self.vela.pixels()
        left, top = x, y # the root is the output's pixels here
        corner = 10 # the window's rounded bottom corners
        wrong = [(px, py) for py in range(top, top + 300) for px in range(left, left + 400)
                 if not (py >= top + 300 - corner and (px < left + corner or px >= left + 400 - corner))
                 and image.at(px, py) != checker(px - left, py - top)]
        self.assertEqual(wrong[:5], [], f"{len(wrong)} pixels differ from the window's")

        # The pointer in the window's pixels, where X11 apps expect it.
        self.vela.input("move", w["x"] + 100, w["y"] + BAR + 50, "sleep", "100", "click")
        self.assertEqual(self.clicks(clicks, 1), (125, 62, x + 125, y + 62))
        self.vela.input("move", w["x"] + 319, w["y"] + BAR + 239, "sleep", "100", "click")
        self.assertEqual(self.clicks(clicks, 3), (398, 298, x + 398, y + 298))
        # Held, the button keeps the pointer with the window even outside it.
        self.vela.input("move", w["x"] + 20, w["y"] + BAR + 20, "sleep", "100", "down", "sleep", "50",
                        "move", w["x"] + 400, w["y"] + BAR + 300, "sleep", "100", "up")
        self.assertEqual(self.clicks(clicks, 5), (25, 25, x + 25, y + 25))
        self.assertEqual(self.clicks(clicks, 6), (500, 375, x + 500, y + 375))

        # Maximized: the usable area in pixels.
        self.vela.keys("super+Up")
        state = self.vela.wait_for(lambda s: s.window(view)["maximized"], what="maximized")
        w = state.window(view)
        self.wait_geometry(xid, (round(w["x"] * 1.25), round((w["y"] + BAR) * 1.25), round(w["w"] * 1.25),
                                 round((w["h"] - BAR) * 1.25)))

    def test_menu_is_placed_in_pixels(self):
        self.open_x11("--menu", "1000,500", 300, 200)
        time.sleep(0.4)
        image = self.vela.pixels()
        # 120x80 pixels where the app put them.
        for x, y in ((1000, 500), (1119, 500), (1000, 579), (1119, 579)):
            self.assertEqual(image.at(x, y), GREEN, (x, y))
        for x, y in ((999, 540), (1120, 540), (1060, 499), (1060, 580)):
            self.assertNotEqual(image.at(x, y), GREEN, (x, y))

    def test_each_output_in_its_own_pixels(self):
        # The user's desk: 1080p at 100% above 1440p at 125%.
        self.needs_xrandr()
        self.vela.command("test-output add 1920x1080")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="second output")
        self.vela.randr("--output", "HEADLESS-2", "--pos", "0,-1080", "--scale", "1")
        self.vela.wait_for(lambda s: any(o["name"] == "HEADLESS-1" and o["y"] == 1080 for o in s["outputs"]),
                           what="the layout")
        self.vela.input("move", 1000, 1600) # opens on the 125% output
        view, xid, _ = self.open_x11(400, 300)
        outputs = self.randr_outputs()
        self.assertEqual(outputs["HEADLESS-1"][:4], (0, 1350, 2560, 1440))
        self.assertEqual(outputs["HEADLESS-1"][4][0], "2560x1440")
        self.assertEqual(outputs["HEADLESS-2"][:4], (0, 0, 1920, 1080))
        self.assertEqual(outputs["HEADLESS-2"][4][0], "1920x1080")
        self.assertEqual(self.geometry(xid)[2:], (400, 300))

        # Onto the 100% output: the same logical size, so as many pixels.
        self.drag(view, -1100)
        state = self.vela.wait_for(lambda s: s.window(view)["output"] == "HEADLESS-2", what="on the 100% output")
        w = state.window(view)
        self.assertEqual((w["w"], w["h"]), (320, 240 + BAR))
        self.wait_geometry(xid, (w["x"], w["y"] + BAR, 320, 240))
        self.x11("--fullscreen", xid, "on")
        self.vela.wait_for(lambda s: s.window(view)["fullscreen"], what="fullscreen at 100%")
        self.wait_geometry(xid, (0, 0, 1920, 1080))
        self.x11("--fullscreen", xid, "off")
        self.vela.wait_for(lambda s: not s.window(view)["fullscreen"], what="windowed at 100%")

        # Back at 125%.
        self.drag(view, 1100)
        state = self.vela.wait_for(lambda s: s.window(view)["output"] == "HEADLESS-1", what="on the 125% output")
        w = state.window(view)
        x, y = round(w["x"] * 1.25), 1350 + round((w["y"] + BAR - 1080) * 1.25)
        self.wait_geometry(xid, (x, y, 400, 300))
        self.x11("--fullscreen", xid, "on")
        self.vela.wait_for(lambda s: s.window(view)["fullscreen"], what="fullscreen at 125%")
        self.wait_geometry(xid, (0, 1350, 2560, 1440))

    def drag(self, view, dy):
        """Moves the window by dy with Super+drag."""
        w = self.vela.state().window(view)
        x, y = w["x"] + 50, w["y"] + 50
        self.vela.input("move", x, y, "keydown", "super", "down", "sleep", "50", "move", x, y + dy // 2,
                        "sleep", "50", "move", x, y + dy, "sleep", "100", "up", "keyup", "super")

if __name__ == "__main__":
    unittest.main()
