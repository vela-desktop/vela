# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Shell pieces (wlr-layer-shell) with vela-panel in place of the Qt shell:
the wallpaper, the taskbar reserving its space and blurring what's behind it
(ext-background-effect, docs/renderer.md §8.3); and damage: a frame drawn
only where something changed must be identical to one redrawn in full
(VELA_DEBUG_DAMAGE=1)."""

import subprocess
import time
import unittest

from harness import Session, tool

BAR = 48


def shell(vela, blur=True):
    """Wallpaper and taskbar, and the time to appear."""
    for args in (["wallpaper"], ["taskbar"] + (["--blur"] if blur else [])):
        vela.clients.append(subprocess.Popen([tool("vela-panel"), *args], env=vela.client_env))
    vela.wait_for(lambda s: s.output["frames"] > 0)
    time.sleep(0.6)


def drag_over_the_taskbar(vela):
    """A decorated window dragged halfway over the taskbar; the screen at the end."""
    vela.open_window(401, 301)
    window = vela.open_window(300, 200, decorated=True)
    w = vela.state().window(window)
    out = vela.state().output
    x, y = w["x"] + 60, w["y"] + 10
    steps = ["move", x, y, "keydown", "super", "down"]
    for i in range(1, 21):
        steps += ["move", x + i * 7, y + i * (out["h"] - 120 - y) // 20, "sleep", "10"]
    vela.input(*steps, "up", "keyup", "super", "move", 5, 5)
    time.sleep(0.5)
    return vela.pixels()


class Layers(unittest.TestCase):
    def test_taskbar_reserves_its_space(self):
        with Session() as vela:
            shell(vela)
            window = vela.open_window()
            vela.keys("super+Up")
            state = vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")
            w, out = state.window(window), state.output
            self.assertEqual((w["y"], w["h"]), (out["y"], out["h"] - BAR))

    def test_taskbar_blurs_what_is_behind(self):
        with Session() as vela:
            shell(vela, blur=False)
            plain = vela.pixels()
        with Session() as vela:
            shell(vela, blur=True)
            blurred = vela.pixels()
        # The wallpaper has sharp bands every 24 rows: behind the bar, only
        # when blurred does it become a gradient, with many colors.
        column = lambda image: {image.at(900, y) for y in range(image.height - BAR, image.height)}
        self.assertLessEqual(len(column(plain)), 4)
        self.assertGreaterEqual(len(column(blurred)), 12)
        # Outside the bar nothing changes.
        self.assertEqual(plain.rows[:plain.height - BAR - 40], blurred.rows[:blurred.height - BAR - 40])


class Damage(unittest.TestCase):
    def check(self, scale):
        with Session(scale=scale) as vela:
            shell(vela)
            partial = drag_over_the_taskbar(vela)
        with Session(scale=scale, env={"VELA_DEBUG_DAMAGE": "1"}) as vela:
            shell(vela)
            full = drag_over_the_taskbar(vela)
        different = sum(1 for y in range(full.height) if partial.rows[y] != full.rows[y])
        self.assertEqual(different, 0, f"{different} rows differ from a full redraw")

    def test_partial_redraw_equals_full_redraw(self):
        self.check(1.0)

    def test_partial_redraw_equals_full_redraw_at_125(self):
        self.check(1.25)

    def test_native_wayland_damage_with_mixed_125_and_100_outputs(self):
        """The compositor, wallpaper and Wayland apps share one damage path.

        Compare incremental repaint against whole-frame repaint with a real
        125%/100% output layout. XWayland is deliberately not involved.
        """
        captures = []
        for full in ("0", "1"):
            with Session(scale="HEADLESS-1=1.25,HEADLESS-2=1",
                         env={"VELA_DEBUG_DAMAGE": full, "VELA_XWAYLAND_BACKEND": "compat"}) as vela:
                vela.command("test-output add 1920x1080")
                vela.wait_for(lambda state: len(state["outputs"]) == 2,
                              what="mixed Wayland output connected")
                vela.randr("--output", "HEADLESS-2", "--scale", "1",
                           "--pos", "1536,0")
                vela.wait_for(
                    lambda state: any(o["name"] == "HEADLESS-2" and o["scale"] == 1
                                      and o["x"] == 1536 for o in state["outputs"]),
                    what="100 percent secondary right of 125 percent primary")
                shell(vela)
                primary = drag_over_the_taskbar(vela)
                secondary = vela.pixels(output=1)
                captures.append((primary, secondary))
        for output in range(2):
            partial, full = captures[0][output], captures[1][output]
            self.assertEqual((partial.width, partial.height), (full.width, full.height))
            different = sum(a != b for a, b in zip(partial.rows, full.rows))
            self.assertEqual(different, 0,
                             f"{different} native Wayland rows differ on output {output}")


if __name__ == "__main__":
    unittest.main()


def to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_srgb(c):
    c = max(0.0, c)
    return round(255 * (c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055))


class BlurPolicy(unittest.TestCase):
    """docs/visual-coherence.md §6.7: every client asking for blur gets the
    blur; Vela's acrylic recipe (saturation 1.25 and the gray tint) is only for
    Vela's surfaces, layer surfaces named vela-... Over a flat wallpaper the
    blur leaves the color as it is, so each pixel under a 50% black panel is
    computable: the wallpaper's for a client, the acrylic value for Vela."""

    WALLPAPER = (0x40, 0x80, 0xc0)
    TINT = (0.11, 0.11, 0.12, 0.55) # the dark shell's (scene.c), straight sRGB and amount

    def expected(self, acrylic):
        """Per channel, the lowest and highest value the dither allows: it adds
        up to 1/255 either way in linear light, before the panel."""
        c = [to_linear(v / 255) for v in self.WALLPAPER]
        if acrylic:
            luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
            c = [luma + (v - luma) * 1.25 for v in c]
            amount = self.TINT[3]
            c = [v * (1 - amount) + to_linear(t) * amount for v, t in zip(c, self.TINT[:3])]
        under = 1 - 128 / 255 # the panel's black at 50%
        return [(to_srgb((v - 1 / 255) * under) - 1, to_srgb((v + 1 / 255) * under) + 1) for v in c]

    def test_acrylic_only_for_vela(self):
        for namespace, acrylic in (("vela-test", True), ("test", False)):
            with self.subTest(namespace=namespace), Session() as vela:
                color = "%02x%02x%02x" % self.WALLPAPER
                for args in (["wallpaper", "--color", color],
                             ["taskbar", "--blur", "--clear", "--namespace", namespace]):
                    vela.clients.append(subprocess.Popen([tool("vela-panel"), *args], env=vela.client_env))
                vela.wait_for(lambda s: s.output["frames"] > 0)
                time.sleep(0.6)
                image = vela.pixels()
                expected = self.expected(acrylic)
                for y in range(image.height - BAR + 8, image.height - 8, 6):
                    for x in range(40, image.width - 40, 97):
                        pixel = image.at(x, y)
                        self.assertTrue(all(lo <= v <= hi for v, (lo, hi) in zip(pixel, expected)),
                                        f"{namespace} at ({x}, {y}): {pixel}, expected within {expected}")
