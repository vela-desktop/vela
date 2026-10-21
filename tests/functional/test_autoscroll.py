# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Middle-click autoscroll (autoscroll.h): on by default; explicitly set
middle-click-autoscroll=no to preserve app middle-click behavior. With it on,
the click starts it and reaches nobody, and the app gets
fine continuous axis events, faster farther from the start, none in the dead
zone, no motion; a click, Esc, the release after a drag or the window losing
the keyboard end it with axis_stop. Terminals are left out. At 120 Hz the
events come once per frame, evenly."""

import os
import statistics
import subprocess
import time
import unittest

from harness import Session, tool

BTN_LEFT, BTN_MIDDLE = 272, 274


def read_log(path):
    events = []
    with open(path) as log:
        for line in log:
            parts = line.split()
            if len(parts) >= 2:
                events.append((int(parts[0]), parts[1], parts[2:]))
    return events


class Autoscroll(unittest.TestCase):
    def start(self, enabled=True, size="1920x1080@60", extra=""):
        self.vela = Session(size=size)
        self.vela.start()
        os.makedirs(os.path.dirname(self.vela.config_path()), exist_ok=True)
        with open(self.vela.config_path(), "w") as config:
            config.write(("" if enabled else "middle-click-autoscroll=no\n") + extra)
        self.vela.command("reload-config")

    def tearDown(self):
        self.vela.stop()

    def open(self, app_id, *flags):
        log = os.path.join(self.vela.directory, f"{app_id}.log")
        window = self.vela.open_window(command=[tool("vela-pattern"), "--app-id", app_id, "--pointer-log", log,
                                                *flags, "600", "500"])
        self.vela.wait_for(lambda s: s["focused"] == window, what=f"{app_id} active")
        # The middle of the new window.
        box = next(w for w in self.vela.state()["windows"] if w["id"] == window)
        self.center = (box["x"] + box["w"] // 2, box["y"] + box["h"] // 2)
        return window, log

    def events(self, log, since=0, kind=None):
        return [e for e in read_log(log) if e[0] >= since and (kind is None or e[1] == kind)]

    def now_us(self):
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000

    def begin(self, log):
        self.vela.input("move", *self.center, "sleep", "50", "click", "middle")
        self.vela.wait_for(lambda s: s["autoscroll"], what="autoscroll on")
        # The click reached nobody.
        self.assertFalse([e for e in self.events(log, kind="button") if e[2][0] == str(BTN_MIDDLE)])

    def scrolled(self, log, dy, seconds=0.5):
        """Moves dy below the start, waits, returns the vertical axis values."""
        self.vela.input("move", self.center[0], self.center[1] + dy)
        time.sleep(0.1)  # the cursor is there
        since = self.now_us()
        time.sleep(seconds)
        return [float(e[2][1]) for e in self.events(log, since, "axis") if e[2][0] == "0"]

    def test_explicitly_disabled(self):
        self.start(enabled=False)
        _, log = self.open("as-a")
        self.vela.input("move", *self.center, "sleep", "50", "click", "middle", "sleep", "50", "rel", 0, 100)
        time.sleep(0.3)
        self.assertFalse(self.vela.state()["autoscroll"])
        buttons = [e[2] for e in self.events(log, kind="button")]
        self.assertIn([str(BTN_MIDDLE), "1"], buttons)
        self.assertIn([str(BTN_MIDDLE), "0"], buttons)
        self.assertFalse(self.events(log, kind="axis"))

    def test_enabled_by_default(self):
        # No setting written: a middle click should activate autoscroll.
        self.start()
        _, log = self.open("as-a")
        self.begin(log)
        self.assertTrue(self.vela.state()["autoscroll"])

    def test_scrolls_and_click_ends(self):
        self.start()
        _, log = self.open("as-a")
        self.begin(log)
        start = self.now_us()
        # Dead zone: nothing.
        self.assertEqual(self.scrolled(log, 8, 0.3), [])
        slow = self.scrolled(log, 60)
        fast = self.scrolled(log, 200)
        self.assertGreater(len(slow), 10)
        self.assertTrue(all(v > 0 for v in slow + fast), "down scrolls down")
        self.assertGreater(sum(fast), 2 * sum(slow), "farther is faster")
        self.assertTrue(any(abs(v - round(v)) > 1e-3 for v in slow), f"fine steps: {slow[:5]}")
        self.assertFalse([e for e in self.events(log, kind="discrete") + self.events(log, kind="value120")])
        self.assertEqual({e[2][0] for e in self.events(log, start, "source")}, {"2"})  # continuous
        self.assertFalse(self.events(log, start, "motion"), "no motion while on")
        # Up scrolls up.
        self.assertTrue(all(v < 0 for v in self.scrolled(log, -100, 0.2)))
        # A left click ends it, with axis_stop, and reaches nobody.
        before = self.now_us()
        self.vela.input("click", "left")
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        time.sleep(0.2)
        self.assertTrue(self.events(log, before, "stop"))
        self.assertFalse(self.events(log, kind="button"))
        # Off: nothing more scrolls.
        quiet = self.now_us()
        time.sleep(0.2)
        self.assertFalse(self.events(log, quiet, "axis"))

    def test_hold_and_drag(self):
        self.start()
        _, log = self.open("as-a")
        # One vela-input: its virtual mouse going away would release the button.
        held = subprocess.Popen([tool("vela-input"), "move", *map(str, self.center), "sleep", "50", "down", "middle",
                                 "sleep", "50", "rel", "0", "80", "sleep", "1000", "up", "middle", "sleep", "300"],
                                env=self.vela.client_env)
        self.vela.wait_for(lambda s: s["autoscroll"], what="autoscroll on")
        time.sleep(0.4)
        self.assertTrue(self.vela.state()["autoscroll"])
        self.assertTrue(self.events(log, kind="axis"))
        held.wait(5)  # released after a drag: off
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        self.assertTrue(self.events(log, kind="stop"))
        self.assertFalse(self.events(log, kind="button"))

    def hold(self, *between):
        """Presses the middle button at the center, does `between`, releases;
        one vela-input in the background (its mouse going away releases)."""
        return subprocess.Popen([tool("vela-input"), "move", *map(str, self.center), "sleep", "50", "down", "middle",
                                 *map(str, between), "up", "middle", "sleep", "300"], env=self.vela.client_env)

    def middle_buttons(self, log):
        return [e[2] for e in self.events(log, kind="button") if e[2][0] == str(BTN_MIDDLE)]

    def test_click_off_hold_on(self):
        self.start(extra="autoscroll-click=no\n")
        _, log = self.open("as-a")
        # A short click is the app's: press and release, nothing else.
        self.vela.input("move", *self.center, "sleep", "50", "click", "middle")
        time.sleep(0.4)
        self.assertFalse(self.vela.state()["autoscroll"])
        self.assertEqual(self.middle_buttons(log), [[str(BTN_MIDDLE), "1"], [str(BTN_MIDDLE), "0"]])
        # Held and dragged: it scrolls, the app gets no press, the release ends it.
        clicks = len(self.middle_buttons(log))
        held = self.hold("sleep", "50", "rel", "0", "80", "sleep", "800")
        self.vela.wait_for(lambda s: s["autoscroll"], what="autoscroll on")
        time.sleep(0.3)
        self.assertTrue(self.events(log, kind="axis"))
        held.wait(5)
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        self.assertEqual(len(self.middle_buttons(log)), clicks)
        # Held still past 200 ms: on too (scrolling starts when it moves).
        held = self.hold("sleep", "400")
        self.vela.wait_for(lambda s: s["autoscroll"], what="autoscroll on, held still")
        held.wait(5)
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        self.assertEqual(len(self.middle_buttons(log)), clicks)

    def test_click_on_hold_off(self):
        self.start(extra="autoscroll-hold=no\n")
        _, log = self.open("as-a")
        held = self.hold("sleep", "50", "rel", "0", "80", "sleep", "300")
        self.vela.wait_for(lambda s: s["autoscroll"], what="autoscroll on")
        held.wait(5)
        time.sleep(0.2)
        # Released after a drag: still on (only a click or Esc ends it).
        self.assertTrue(self.vela.state()["autoscroll"])
        self.vela.input("click", "left")
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        self.assertFalse(self.events(log, kind="button"))

    def test_click_off_hold_off(self):
        self.start(extra="autoscroll-click=no\nautoscroll-hold=no\n")
        _, log = self.open("as-a")
        held = self.hold("sleep", "50", "rel", "0", "80", "sleep", "400")
        time.sleep(0.3)
        self.assertFalse(self.vela.state()["autoscroll"])
        held.wait(5)
        self.assertEqual(self.middle_buttons(log), [[str(BTN_MIDDLE), "1"], [str(BTN_MIDDLE), "0"]])
        self.assertFalse(self.events(log, kind="axis"))

    def test_escape_ends(self):
        self.start()
        _, log = self.open("as-a")
        self.begin(log)
        since = self.now_us()
        self.vela.keys("esc")
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        time.sleep(0.1)
        # Linux evdev KEY_ESC=1: neither press nor release may leak.
        self.assertFalse([e for e in self.events(log, since, "key") if e[2][0] == "1"])
        # The next Escape is an ordinary app key, both press and release.
        since = self.now_us()
        self.vela.keys("esc")
        time.sleep(0.1)
        self.assertEqual([e[2][1] for e in self.events(log, since, "key") if e[2][0] == "1"], ["1", "0"])

    def test_extended_button_release_not_leaked(self):
        self.start()
        _, log = self.open("as-a")
        self.begin(log)
        # BTN_TRIGGER_HAPPY1=704 is well outside the old 32-bit mouse range.
        self.vela.input("click", "704")
        self.vela.wait_for(lambda s: not s["autoscroll"], what="extended button ends autoscroll")
        time.sleep(0.1)
        self.assertFalse([e for e in self.events(log, kind="button") if e[2][0] == "704"])
        # Once autoscroll is off, the same button is forwarded in both directions.
        self.vela.input("click", "704")
        time.sleep(0.1)
        self.assertEqual([e[2][1] for e in self.events(log, kind="button") if e[2][0] == "704"], ["1", "0"])

    def test_relative_motion_is_swallowed(self):
        self.start()
        _, log = self.open("as-a")
        self.begin(log)
        since = self.now_us()
        self.vela.input("rel", "0", "80")
        time.sleep(0.2)
        self.assertFalse(self.events(log, since, "relative"), "relative pointer events leaked during autoscroll")
        self.vela.input("click", "left")
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")
        since = self.now_us()
        self.vela.input("rel", "0", "-20")
        time.sleep(0.1)
        self.assertTrue(self.events(log, since, "relative"), "ordinary relative motion was suppressed")

    def test_pending_hold_swallows_activation_motion(self):
        self.start(extra="autoscroll-click=no\n")
        _, log = self.open("as-a")
        # A 4px+ relative move starts the pending hold gesture: that very
        # motion must not leak through the relative-pointer protocol.
        held = self.hold("sleep", "50", "rel", "0", "80", "sleep", "500")
        self.vela.wait_for(lambda s: s["autoscroll"], what="held autoscroll active")
        time.sleep(0.15)
        self.assertFalse(self.events(log, kind="relative"))
        held.wait(5)
        self.vela.wait_for(lambda s: not s["autoscroll"], what="held autoscroll off")

    def test_shortcuts_inhibitor_preserves_middle_click(self):
        self.start()
        _, log = self.open("as-a", "--inhibit-shortcuts")
        self.vela.wait_for(lambda state: self.events(log, kind="inhibitor"),
                           what="shortcuts inhibitor activation")
        self.vela.input("move", *self.center, "sleep", "50", "click", "middle")
        time.sleep(0.1)
        self.assertFalse(self.vela.state()["autoscroll"])
        self.assertEqual(self.middle_buttons(log), [[str(BTN_MIDDLE), "1"], [str(BTN_MIDDLE), "0"]])

    def test_terminals_left_out(self):
        self.start()
        _, log = self.open("foot")
        self.vela.input("move", *self.center, "sleep", "50", "click", "middle")
        time.sleep(0.3)
        self.assertFalse(self.vela.state()["autoscroll"])
        self.assertIn([str(BTN_MIDDLE), "1"], [e[2] for e in self.events(log, kind="button")])

    def test_focus_change_ends(self):
        self.start()
        self.open("as-b")
        _, log = self.open("as-a")
        self.begin(log)
        subprocess.run([tool("vela-windows"), "activate", "as-b"], env=self.vela.client_env, check=True,
                       stdout=subprocess.DEVNULL)
        self.vela.wait_for(lambda s: not s["autoscroll"], what="autoscroll off")

    def test_even_frames_at_120hz(self):
        self.start(size="1920x1080@120")
        _, log = self.open("as-a")
        self.begin(log)
        self.vela.input("move", self.center[0], self.center[1] + 100)
        time.sleep(0.2)
        since = self.now_us()
        time.sleep(1.0)
        frames = [e[0] for e in self.events(log, since, "frame")]
        gaps = [(b - a) / 1000 for a, b in zip(frames, frames[1:])]
        period = 1000 / 120
        mean = statistics.mean(gaps)
        print(f"\n120 Hz: {len(frames)} frames in 1 s, gap mean {mean:.3f} ms, "
              f"min {min(gaps):.3f}, max {max(gaps):.3f}, stdev {statistics.pstdev(gaps):.3f}")
        self.assertGreater(len(frames), 110)
        self.assertAlmostEqual(mean, period, delta=0.5)
        self.assertLess(max(gaps), 2 * period)
        values = [float(e[2][1]) for e in self.events(log, since, "axis") if e[2][0] == "0"]
        # The same step every frame (to the protocol's 1/256 px).
        self.assertLess(max(values) - min(values), 2 / 256 + 1e-9)


if __name__ == "__main__":
    unittest.main()
