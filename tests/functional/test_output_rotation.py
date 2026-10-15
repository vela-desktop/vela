# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Mixed landscape/portrait regression tests for the real output-management path.

Requires a Vulkan-capable GPU: the functional runner skips on GPU-less CI.
The assertions check layout, window placement AND rendered pixels; a correct
logical output box alone cannot prove the rotated output actually draws.
"""

import os
import unittest
from functools import lru_cache

from harness import Image, Session
from test_layers import drag_over_the_taskbar, shell


@lru_cache(maxsize=4)
def pattern_rows(width, height):
    """Independent RGB oracle for vela-pattern's asymmetric coordinate image."""
    red = bytes(x & 255 for x in range(width))
    blue = bytes(((x >> 8) & 15) << 4 for x in range(width))
    rows = []
    for y in range(height):
        row = bytearray(width * 3)
        row[0::3] = red
        row[1::3] = bytes([y & 255]) * width
        row[2::3] = bytes(b | ((y >> 8) & 15) for b in blue)
        rows.append(bytes(row))
    return rows


class MixedOutputRotation(unittest.TestCase):
    """Exercise real output commits and their pixels in isolated GPU sessions."""

    def setUp(self):
        """Start a landscape output with a fractional-scale hotplug default."""
        self.vela = Session(scale="HEADLESS-1=1,HEADLESS-2=1.5", size="1920x1080@60")
        self.vela.start()

    def tearDown(self):
        """Stop clients and require a clean compositor exit after each scenario."""
        self.vela.stop()

    def add_output(self, name="HEADLESS-2"):
        """Hotplug a real headless output and wait for its advertised identity."""
        self.vela.command("test-output add 1920x1080")
        return self.vela.wait_for(lambda s: any(o["name"] == name for o in s["outputs"]),
                                  what=f"{name} connected")

    @staticmethod
    def output(state, name):
        """Select output state by name rather than assuming layout order."""
        return next(o for o in state["outputs"] if o["name"] == name)

    def assert_disjoint(self, state):
        """Output rectangles must not overlap, regardless of sign or orientation."""
        enabled = [o for o in state["outputs"] if o["enabled"]]
        for i, left in enumerate(enabled):
            for right in enabled[i + 1:]:
                separated = (left["x"] + left["w"] <= right["x"]
                             or right["x"] + right["w"] <= left["x"]
                             or left["y"] + left["h"] <= right["y"]
                             or right["y"] + right["h"] <= left["y"])
                self.assertTrue(separated, f"output boxes overlap: {left} / {right}")

    def rotate(self, angle, x, y=0, scale=1):
        """Apply orientation, scale and position and await their logical geometry."""
        self.vela.randr("--output", "HEADLESS-2", "--scale", str(scale),
                        "--pos", f"{x},{y}", "--transform", str(angle))
        expected = ("normal", "90", "180", "270", "flipped", "flipped-90",
                    "flipped-180", "flipped-270").index(str(angle))
        width, height = (1080, 1920) if expected & 1 else (1920, 1080)
        width, height = int(width / scale), int(height / scale)
        return self.vela.wait_for(
            lambda s: (lambda o: (o["x"], o["y"], o["w"], o["h"], o["transform"])
                       == (x, y, width, height, expected))(self.output(s, "HEADLESS-2")),
            what=f"rotation {angle} applied and layout resized")

    def drag_to_output(self, window, state, name="HEADLESS-2"):
        """Move the mapped window to the named output through virtual mouse input."""
        start = state.window(window)
        second = self.output(state, name)
        sx, sy = start["x"] + 50, start["y"] + 50
        tx, ty = second["x"] + second["w"] // 2, second["y"] + 200
        self.vela.input("move", sx, sy, "keydown", "super", "down", "sleep", "50",
                        "move", (sx + tx) // 2, (sy + ty) // 2, "sleep", "50",
                        "move", tx, ty, "sleep", "100", "up", "keyup", "super")
        return self.vela.wait_for(lambda s: s.window(window)["output"] == name,
                                  what=f"window moved to {name}")

    def assert_picture_stable(self, index=1):
        """Require identical pixels across distinct presented output frames."""
        name = f"HEADLESS-{index + 1}"
        images = []
        for _ in range(3):
            frames = self.output(self.vela.state(), name)["frames"]
            images.append(self.vela.pixels(output=index))
            self.vela.wait_for(lambda s: self.output(s, name)["frames"] > frames,
                               what="a distinct presented frame for the stability capture")
        for image in images[1:]:
            self.assertEqual((image.width, image.height), (images[0].width, images[0].height))
            self.assertEqual(image.rows, images[0].rows, "static portrait output flickered")

    def assert_pattern(self, width, height, index=1):
        """Compare every interior pixel with the known pattern, allowing client resize."""
        expected = pattern_rows(width, height)
        # Ignore only window edge antialiasing; the entire interior must be exact.
        inset = 16
        last = None

        def matches(_):
            """Capture and compare the upright interior after asynchronous commits."""
            nonlocal last
            last = self.vela.pixels(output=index)
            return ((last.width, last.height) == (width, height)
                    and all(actual[inset * 3:-inset * 3] == wanted[inset * 3:-inset * 3]
                            for actual, wanted in zip(last.rows[inset:-inset], expected[inset:-inset])))

        self.vela.wait_for(matches, what="upright coordinate pattern on the rotated output", read_state=False)
        return last

    def maximize_on_output(self, name="HEADLESS-2"):
        """Map and maximize the pattern on the named output, keeping the cursor elsewhere."""
        window = self.vela.open_window(300, 200)
        self.drag_to_output(window, self.vela.state(), name)
        self.vela.keys("super+up")
        self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized pattern window")
        self.vela.input("move", 100, 100)
        return window

    def test_portrait_90_renders_maximized_window_without_overlap(self):
        """A portrait window must draw correctly and leave the landscape output black."""
        self.add_output()
        state = self.rotate("90", 1920, scale=1)
        self.assert_disjoint(state)
        window = self.maximize_on_output()
        state = self.vela.state()
        second = self.output(state, "HEADLESS-2")
        w = state.window(window)
        self.assertEqual((w["x"], w["y"], w["w"], w["h"]),
                         (second["x"], second["y"], second["w"], second["h"]))
        self.assert_pattern(1080, 1920)
        # A window on output 2 must not be painted over output 1.
        self.assertEqual(self.vela.pixels(output=0).at(960, 540), (0, 0, 0))
        self.assert_picture_stable()

    def test_270_and_rotation_changes_keep_the_image_and_layout(self):
        """Every transform of an existing window must match an independent pixel oracle."""
        self.add_output()
        self.rotate("normal", 1920, scale=1)
        window = self.maximize_on_output()
        for angle, expected in (("90", (1080, 1920)), ("270", (1080, 1920)),
                                ("180", (1920, 1080)), ("normal", (1920, 1080)),
                                ("flipped", (1920, 1080)), ("flipped-90", (1080, 1920)),
                                ("flipped-180", (1920, 1080)), ("flipped-270", (1080, 1920))):
            with self.subTest(angle=angle):
                state = self.rotate(angle, 1920, scale=1)
                self.assert_disjoint(state)
                second = self.output(state, "HEADLESS-2")
                self.vela.wait_for(
                    lambda s: (lambda w: w["maximized"] and (w["x"], w["y"], w["w"], w["h"])
                               == (second["x"], second["y"], second["w"], second["h"]))(s.window(window)),
                    what=f"maximized window resized after {angle} rotation")
                # The PNG is upright, so correct 90/270 pictures can be equal.
                # Reinterpreting a stale raw 90 buffer at 270 rotates its asymmetric
                # pattern by 180 degrees and fails this independently computed oracle.
                self.assert_pattern(*expected)
                self.assert_picture_stable()

    def test_portrait_left_of_landscape_at_fractional_scale(self):
        """Negative layout coordinates and 150% scaling must preserve exact pixels."""
        self.add_output()
        state = self.rotate("90", -720, scale=1.5)
        self.assert_disjoint(state)
        second = self.output(state, "HEADLESS-2")
        self.assertEqual((second["x"], second["y"], second["w"], second["h"]),
                         (-720, 0, 720, 1280))
        self.maximize_on_output()
        self.assert_pattern(1080, 1920)

    def test_four_outputs_mixed_orientation(self):
        """Four outputs must preserve geometry and render three simultaneous coordinate patterns."""
        self.add_output()
        self.vela.randr("--output", "HEADLESS-2", "--pos", "1920,0")
        self.add_output("HEADLESS-3")
        self.vela.randr("--output", "HEADLESS-3", "--scale", "1",
                        "--pos", "-1080,0", "--transform", "90")
        self.add_output("HEADLESS-4")
        self.vela.randr("--output", "HEADLESS-4", "--scale", "1",
                        "--pos", "3200,0", "--transform", "270")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 4
                                  and self.output(s, "HEADLESS-3")["w"] == 1080
                                  and self.output(s, "HEADLESS-4")["w"] == 1080,
                                  what="two landscape and two portrait outputs")
        self.assert_disjoint(state)
        for name, expected in {
            "HEADLESS-1": (0, 0, 1920, 1080, 0),
            "HEADLESS-2": (1920, 0, 1280, 720, 0),
            "HEADLESS-3": (-1080, 0, 1080, 1920, 1),
            "HEADLESS-4": (3200, 0, 1080, 1920, 3),
        }.items():
            o = self.output(state, name)
            self.assertEqual((o["x"], o["y"], o["w"], o["h"], o["transform"]), expected, name)
        for name in ("HEADLESS-2", "HEADLESS-3", "HEADLESS-4"):
            self.maximize_on_output(name)
        for index, size in ((1, (1920, 1080)), (2, (1080, 1920)), (3, (1080, 1920))):
            self.assert_pattern(*size, index=index)
        self.assertEqual(self.vela.pixels(output=0).at(960, 540), (0, 0, 0))

    def test_capture_region_uses_upright_portrait_coordinates(self):
        """A crop near the portrait bottom must use transformed dimensions and pixels."""
        self.add_output()
        self.rotate("90", 1920)
        self.maximize_on_output()
        self.assert_pattern(1080, 1920)
        path = os.path.join(self.vela.directory, "crop.png")
        x, y, width, height = 1000, 1800, 64, 80
        self.vela.screenshot(path, output=1, region=(x, y, width, height))
        image = Image.read(path)
        self.assertEqual((image.width, image.height), (width, height))
        expected = [row[x * 3:(x + width) * 3] for row in pattern_rows(1080, 1920)[y:y + height]]
        self.assertEqual(image.rows, expected)


class RotatedDamage(unittest.TestCase):
    """Compare partial updates, blur and window movement with independent full redraws."""

    def test_partial_redraw_matches_full_on_rotated_outputs(self):
        """Portrait damage and blurred layers must agree at integer and fractional scales."""
        for angle, scale in (("90", 1), ("270", 1.25), ("flipped-90", 1), ("flipped-270", 1.25)):
            with self.subTest(angle=angle, scale=scale):
                images = []
                for full in ("0", "1"):
                    with Session(scale=scale, env={"VELA_DEBUG_DAMAGE": full}) as vela:
                        vela.randr("--output", "HEADLESS-1", "--transform", angle)
                        shell(vela)
                        images.append(drag_over_the_taskbar(vela))
                self.assertEqual((images[0].width, images[0].height),
                                 (images[1].width, images[1].height))
                different = sum(a != b for a, b in zip(images[0].rows, images[1].rows))
                self.assertEqual(different, 0, f"{different} portrait rows differ from a full redraw")


if __name__ == "__main__":
    unittest.main()
