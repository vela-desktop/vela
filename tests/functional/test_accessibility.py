# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Accessibility and keyboard (a11y.c, keyboard.c): the shell's quick
settings (night light, color filters, magnifier, sticky keys) with the state
the shell receives and what stays in vela.conf; scheduled night light;
sticky keys; Super alone opening Start; the magnifier with Win+plus and
Win+Esc. The shell is a FakeShell collecting the messages."""

import json
import os
import subprocess
import tempfile
import time
import unittest

from harness import Session, tool

SHIFT = 1 # WLR_MODIFIER_SHIFT


def config_home(lines):
    """An XDG_CONFIG_HOME with vela.conf already written."""
    home = tempfile.mkdtemp(prefix="vela-a11y-")
    os.makedirs(os.path.join(home, "vela"))
    with open(os.path.join(home, "vela", "vela.conf"), "w") as conf:
        conf.write("".join(line + "\n" for line in lines))
    return home


def clock(minutes):
    minutes %= 24 * 60
    return f"{minutes // 60:02d}:{minutes % 60:02d}"


class Accessibility(unittest.TestCase):
    def start(self, conf=()):
        self.vela = Session(env={"XDG_CONFIG_HOME": config_home(conf)})
        self.vela.start()
        self.shell = self.vela.fake_shell()

    def tearDown(self):
        self.vela.stop()

    def a11y(self):
        return self.vela.query("accessibility")

    def conf(self):
        with open(self.vela.config_path()) as conf:
            return conf.read()

    def test_quick_settings(self):
        """Verify quick settings, workaround defaults, and persistence."""
        self.start()
        self.assertFalse(self.a11y()["nightLightNvidiaFix"])  # workaround off by default
        self.assertEqual({key: self.a11y()[key] for key in ("nightLight", "colorFilter", "magnifier", "stickyKeys")},
                         {"nightLight": False, "colorFilter": False, "magnifier": False, "stickyKeys": False})
        for command, key, setting in (("night-light", "nightLight", "night-light=yes"),
                                      ("color-filter", "colorFilter", "color-filters=yes"),
                                      ("magnifier", "magnifier", None),
                                      ("sticky-keys", "stickyKeys", "sticky-keys=yes")):
            self.shell.clear()
            self.vela.command(f"{command} toggle")
            announced = json.loads(self.shell.wait_for("accessibility ").split(" ", 1)[1])
            self.assertTrue(announced[key], command)
            self.assertTrue(self.a11y()[key], command)
            if setting:
                self.assertIn(setting, self.conf())
        # Off again, in the file too.
        for command, key in (("night-light", "nightLight"), ("color-filter", "colorFilter"),
                             ("magnifier", "magnifier"), ("sticky-keys", "stickyKeys")):
            self.vela.command(f"{command} off")
            self.vela.wait_for(lambda _: not self.a11y()[key], what=f"{command} off", read_state=False)
        self.assertIn("night-light=no", self.conf())
        self.assertIn("color-filters=no", self.conf())

    def test_night_light_schedule(self):
        # Scheduled between two times around now: on already at startup.
        now = time.localtime()
        minutes = now.tm_hour * 60 + now.tm_min
        self.start([f"night-light-schedule=hours", f"night-light-from={clock(minutes - 60)}",
                    f"night-light-to={clock(minutes + 60)}"])
        self.assertTrue(self.a11y()["nightLight"])
        # A range that doesn't include now: it turns off at "reload-config".
        with open(self.vela.config_path(), "w") as conf:
            conf.write(f"night-light=yes\nnight-light-schedule=hours\nnight-light-from={clock(minutes + 120)}\n"
                       f"night-light-to={clock(minutes + 180)}\n")
        self.vela.command("reload-config")
        self.vela.wait_for(lambda _: not self.a11y()["nightLight"], what="night light off by the schedule",
                           read_state=False)

    def test_night_light_headless_fallback_pixels(self):
        # A headless output has no DRM gamma LUT. The Vulkan fallback must
        # tint rendered pixels, then restore the originals when turned off.
        self.start(["night-light-temperature=1700"])
        identifier = self.vela.open_window(400, 300,
                                           command=[tool("vela-pattern"), "--scale1", "400", "300"])
        window = self.vela.state().window(identifier)
        before = self.vela.pixels()
        # The scale1 test pattern has stable pure-white pixels in this area.
        left, top = int(window["x"]), int(window["y"])
        samples = [(left + x, top + y) for y in range(80, 110) for x in range(230, 270)
                   if before.at(left + x, top + y) == (255, 255, 255)]
        self.assertTrue(samples, "test pattern should have a stable white pixel")
        x, y = samples[len(samples) // 2]
        original = before.at(x, y)

        self.vela.command("night-light on")
        self.vela.wait_for(lambda _: self.vela.pixels().at(x, y)[2] < original[2] - 10,
                           what="Vulkan night-light fallback to reduce blue", read_state=False)

        self.vela.command("night-light off")
        self.vela.wait_for(
            lambda _: all(abs(actual - expected) <= 3
                          for actual, expected in zip(self.vela.pixels().at(x, y), original)),
            what="Vulkan night-light fallback to restore the original pixel", read_state=False)

        # Interrupt another fade while it is still warming the output:
        # disabling must not leave a partially tinted cached buffer behind.
        self.vela.command("night-light on")
        self.vela.wait_for(lambda _: self.vela.pixels().at(x, y)[2] < original[2] - 3,
                           what="a partial Night Light transition", read_state=False)
        self.vela.command("night-light off")
        self.vela.wait_for(
            lambda _: all(abs(actual - expected) <= 3
                          for actual, expected in zip(self.vela.pixels().at(x, y), original)),
            what="restoration after interrupting Night Light", read_state=False)

    def test_night_light_nvidia_fix_config_reload(self):
        """Verify the opt-in NVIDIA fallback reloads without restarting."""
        # The experimental override is opt-in, persists in vela.conf and can be
        # switched without restarting the compositor. Headless always renders
        # using Vulkan; real DRM/NVIDIA validation needs physical hardware.
        self.start(["night-light-nvidia-fix=yes", "night-light-temperature=1700"])
        self.assertTrue(self.a11y()["nightLightNvidiaFix"])
        self.vela.command("night-light on")
        self.vela.wait_for(lambda _: self.a11y()["nightLight"], what="Night light enabled",
                           read_state=False)
        with open(self.vela.config_path(), "w") as conf:
            conf.write("night-light=yes\nnight-light-temperature=1700\nnight-light-nvidia-fix=no\n")
        self.vela.command("reload-config")
        self.vela.wait_for(lambda _: not self.a11y()["nightLightNvidiaFix"],
                           what="experimental NVIDIA fix disabled by reload",
                           read_state=False)
        self.assertTrue(self.a11y()["nightLight"])
        self.assertIn("night-light-nvidia-fix=no", self.conf())

    def test_night_light_nvidia_fix_headless_pixels_after_toggles(self):
        """Validate tinted pixels after a newly rendered output frame."""
        # Headless has no hardware LUT, but switching the option while warm
        # must preserve the visible tint and a final fade must restore every
        # pixel. The actual DRM gamma -> Vulkan transition needs hardware.
        # The older key, still read without night-light-temperature: 1700 K.
        self.start(["night-light-strength=100", "night-light-nvidia-fix=yes"])
        identifier = self.vela.open_window(400, 300,
                                           command=[tool("vela-pattern"), "--scale1", "400", "300"])
        window = self.vela.state().window(identifier)
        before = self.vela.pixels()
        left, top = int(window["x"]), int(window["y"])
        samples = [(left + x, top + y) for y in range(80, 110) for x in range(230, 270)
                   if before.at(left + x, top + y) == (255, 255, 255)]
        self.assertTrue(samples, "test pattern should contain a stable white pixel")
        x, y = samples[len(samples) // 2]
        original = before.at(x, y)

        self.vela.command("night-light on")
        self.vela.wait_for(lambda _: self.vela.pixels().at(x, y)[2] < original[2] - 10,
                           what="the experimental shader to warm the screen", read_state=False)
        for enabled in (False, True):
            # The accessibility state can change before the compositor redraws.
            # Record the frame count before reload to detect a fresh frame.
            frames_before_reload = self.vela.state()["outputs"][0]["frames"]
            with open(self.vela.config_path(), "w") as conf:
                conf.write("night-light=yes\nnight-light-temperature=1700\n"
                           f"night-light-nvidia-fix={'yes' if enabled else 'no'}\n")
            self.vela.command("reload-config")
            self.vela.wait_for(lambda _: self.a11y()["nightLightNvidiaFix"] == enabled,
                               what=f"experimental fallback set to {enabled}", read_state=False)
            self.vela.wait_for(lambda state: state["outputs"][0]["frames"] > frames_before_reload,
                               what=f"a post-reload output frame with fallback set to {enabled}")
            self.assertLess(self.vela.pixels().at(x, y)[2], original[2] - 10)

        self.vela.command("night-light off")
        self.vela.wait_for(
            lambda _: all(abs(actual - expected) <= 3
                          for actual, expected in zip(self.vela.pixels().at(x, y), original)),
            what="neutral pixels after a shader fade reaches zero", read_state=False)

    def modifiers_while(self, actions, at_ms):
        """The keyboard modifiers ("modifiers" request) at the moments
        `at_ms` while vela-input runs `actions`: its virtual keyboard
        lives only as long as it runs."""
        process = subprocess.Popen([tool("vela-input"), *map(str, actions)], env=self.vela.client_env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        start = time.monotonic()
        seen = []
        for at in at_ms:
            time.sleep(max(0.0, start + at / 1000 - time.monotonic()))
            seen.append(self.vela.query("modifiers"))
        process.wait(10)
        return seen

    def test_sticky_keys(self):
        self.start(["sticky-keys=yes"])
        # Shift pressed and released applies to the next key, then it's let go.
        latched, after = self.modifiers_while(
            ["keydown", "shift", "keyup", "shift", "sleep", "800", "key", "a", "sleep", "1200"], [400, 1500])
        self.assertEqual(latched & SHIFT, SHIFT)
        self.assertEqual(after & SHIFT, 0)
        # Win the first time stays for the next key (Start doesn't open), the
        # second time opens Start like the key alone.
        self.vela.keys("super")
        time.sleep(0.3)
        self.assertEqual(self.shell.received("toggle-start"), [])
        self.vela.keys("super")
        self.shell.wait_for("toggle-start")

    def test_super_alone_opens_start(self):
        self.start()
        self.vela.keys("super")
        self.shell.wait_for("toggle-start")
        # With another key it's a shortcut, not Start.
        self.shell.clear()
        self.vela.keys("super+d")
        self.shell.wait_for("show-desktop")
        self.assertEqual(self.shell.received("toggle-start"), [])

    def test_magnifier_keys(self):
        self.start()
        self.vela.open_window(400, 300)
        self.vela.input("move", 700, 400)
        time.sleep(0.2)
        before = self.vela.pixels()
        self.vela.keys("super+plus")
        self.vela.wait_for(lambda _: self.a11y()["magnifier"], what="the magnifier open", read_state=False)
        time.sleep(0.6) # the animation
        self.assertNotEqual(self.vela.pixels().rows, before.rows)
        self.vela.keys("super+Escape")
        self.vela.wait_for(lambda _: not self.a11y()["magnifier"], what="the magnifier closed", read_state=False)
        time.sleep(0.6)
        self.assertEqual(self.vela.pixels().rows, before.rows)

    def test_magnifier_reopens_at_the_pointer(self):
        # The point kept still while zooming is the one under the pointer, also
        # when the last zoom had moved its view elsewhere. Read from the pattern
        # window's pixels, whose color is their position (red x, green y).
        self.start()
        window = self.vela.open_window(400, 300)
        self.vela.keys("super+Up")
        self.vela.wait_for(lambda s: s.window(window)["maximized"], what="the pattern maximized")
        self.vela.wait_still(window)
        w = self.vela.state().window(window)
        self.vela.input("move", 400, 300, "sleep", 100)
        self.vela.command("magnifier on")
        time.sleep(0.5)
        self.vela.input("move", 1800, 1050, "sleep", 300) # the view follows the pointer
        self.vela.command("magnifier off")
        time.sleep(0.5) # the zoom-out animation
        self.vela.input("move", 400, 300, "sleep", 100)
        self.vela.command("magnifier on")
        time.sleep(0.5)
        image = self.vela.pixels()
        # At 2x around (400, 300), the screen's (x, y) shows (200 + x/2, 150 + y/2).
        for x, y in ((360, 260), (300, 200)):
            red, green, _ = image.at(x, y)
            expected = ((200 + x // 2 - w["x"]) & 255, (150 + y // 2 - w["y"]) & 255)
            self.assertLessEqual(max(abs(red - expected[0]), abs(green - expected[1])), 2,
                                 f"screen ({x}, {y}) shows {(red, green)}, expected {expected}")


if __name__ == "__main__":
    unittest.main()
