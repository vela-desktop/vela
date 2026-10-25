# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The animation clock (docs/visual-coherence.md §6.2): each output's frame
shows every animation at its own predicted presentation time, and what can't
be undone waits for the wall clock. Read from the VELA_DEBUG_ANIMATION log,
with two outputs at 75 and 180 Hz, with late latching and without."""

import os
from pathlib import Path
import re
import time
import unittest

from harness import Session

FRAME = re.compile(r"animation: frame (\S+) present ([\d.]+) wall ([\d.]+)")
PROGRESS = re.compile(r"animation: (\S+) #(\d+) start ([\d.]+) duration ([\d.]+) curve ([\d.,-]+) "
                      r"x ([\d.]+) progress ([\d.]+)")
HZ = {"HEADLESS-1": 75, "HEADLESS-2": 180}
RETIRED = re.compile(r"animation: (\S+) #(\d+) retired wall ([\d.]+) end ([\d.]+)")
RADIUS = re.compile(r"animation: morph #(\d+) radius ([\d.]+)")


def bezier(curve, x):
    """cubic-bezier(x1, y1, x2, y2) at x, solved by bisection."""
    x1, y1, x2, y2 = curve
    def at(p1, p2, t):
        return 3 * p1 * t * (1 - t) ** 2 + 3 * p2 * t * t * (1 - t) + t ** 3
    lo, hi = 0.0, 1.0
    for _ in range(60):
        t = (lo + hi) / 2
        if at(x1, x2, t) < x:
            lo = t
        else:
            hi = t
    return at(y1, y2, (lo + hi) / 2)


class AnimationClock(unittest.TestCase):
    def test_each_output_samples_its_own_time(self):
        for latch in ("1", "0"):
            with self.subTest(latch=latch), Session(size="1920x1080@75",
                                                    env={"VELA_DEBUG_ANIMATION": "1", "VELA_LATCH": latch}) as vela:
                vela.command("test-output add 1920x1080@180")
                vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two outputs")
                retired = 0

                def settle(what):
                    nonlocal retired
                    retired += 1
                    vela.wait_for(lambda _: vela.log_text().count(" retired wall ") >= retired, read_state=False,
                                  what=f"{what} retired")

                window = vela.open_window()
                settle("open")
                vela.command(f"window {window} minimize")
                settle("minimize")
                vela.command(f"window {window} restore")
                settle("restore")
                vela.command("workspace new switch")
                settle("the switch to the new desktop")
                vela.command("workspace switch 0")
                settle("the switch back")
                vela.command(f"window {window} close")
                settle("close")

                # Nothing animates: one second later, nothing is presented.
                time.sleep(1.0)
                before = [o["frames"] for o in vela.state()["outputs"]]
                time.sleep(1.0)
                self.assertEqual([o["frames"] for o in vela.state()["outputs"]], before, "frames while idle")
                self.check_log(vela.log_text())

    def check_log(self, log):
        output = present = None
        last = {} # (output, serial) -> progress
        sampled = {} # serial -> outputs that drew it
        frames = {} # (serial, output) -> how many frames showed it
        timeline = {} # (serial, output) -> predicted presentation time, progress
        durations = {}
        names = {}
        retirements = {}
        for line in log.splitlines():
            if m := FRAME.search(line):
                output, present = m[1], float(m[2])
            elif m := PROGRESS.search(line):
                name, serial = m[1], int(m[2])
                start, duration, x, progress = float(m[3]), float(m[4]), float(m[6]), float(m[7])
                curve = [float(v) for v in m[5].split(",")]
                where = f"{name} #{serial} on {output} at {present}"
                # The state at this output's own time, not another's.
                self.assertAlmostEqual(x, min(1.0, max(0.0, (present - start) / duration)), delta=1e-6, msg=where)
                self.assertAlmostEqual(progress, bezier(curve, x), delta=1e-5, msg=where)
                self.assertGreaterEqual(progress, last.get((output, serial), 0.0), where)
                last[(output, serial)] = progress
                sampled.setdefault(serial, set()).add(output)
                frames[(serial, output)] = frames.get((serial, output), 0) + 1
                timeline.setdefault((serial, output), []).append((present, x))
                durations[serial] = duration
                names[serial] = name
            elif m := RETIRED.search(line):
                serial, wall, end = int(m[2]), float(m[3]), float(m[4])
                self.assertGreaterEqual(wall, end, f"{m[1]} #{serial} retired before its end")
                self.assertNotIn(serial, retirements, f"{m[1]} #{serial} retired twice")
                retirements[serial] = wall
        self.assertEqual({names[s] for s in sampled}, {"open", "minimize", "restore", "desktop", "close"})
        for serial, outputs in sampled.items():
            self.assertIn(serial, retirements, f"{names[serial]} #{serial} never retired")
            self.assertEqual(outputs, {"HEADLESS-1", "HEADLESS-2"}, f"{names[serial]} #{serial}")
        # One frame per vblank, also on an output with nothing to draw (until
        # the wall clock passes the end: a few more).
        for (serial, output), count in frames.items():
            limit = durations[serial] * HZ[output] / 1000 + 4
            if count > limit:
                # Keep the original strict bound. Capture whether the extra
                # samples came from duplicate predicted timestamps, too many
                # scheduled vblanks, or frames after the animation's end.
                samples = timeline[(serial, output)]
                timestamps = [t for t, _ in samples]
                repeat = sum(a == b for a, b in zip(timestamps, timestamps[1:]))
                intervals = [(b - a) for a, b in zip(timestamps, timestamps[1:])]
                self.fail(
                    f"{names[serial]} #{serial} on {output}: {count} frames > {limit}; "
                    f"duration={durations[serial]}ms; repeat={repeat}; "
                    f"present range={timestamps[0]:.6f}..{timestamps[-1]:.6f}ms; "
                    f"interval min/max={min(intervals, default=0):.4f}/"
                    f"{max(intervals, default=0):.4f}ms; "
                    f"sample first/last={samples[:3]}/{samples[-3:]}")


class Durations(unittest.TestCase):
    """docs/visual-coherence.md §6.4: every window animation lasts one of the
    four durations of motion.h (the ones Theme.qml shares), times Settings'
    animation-duration (100: Vela's)."""

    TOKENS = {"open": "SLOW", "close": "NORMAL", "minimize": "SLOW", "restore": "SLOW", "morph": "SLOW",
              "desktop": "LONG", "snap-preview": "FAST", "magnifier": "FAST"}

    def play(self, vela, factor=1.0):
        """Every window animation once; their logged durations."""
        mark = len(vela.log_text())
        window = vela.open_window(400, 300, decorated=True)
        vela.wait_for(lambda s: s["focused"] == window)
        pause = 0.5 * factor
        for action in ("maximize", "restore", "minimize", "restore"):
            vela.command(f"window {window} {action}")
            time.sleep(pause)
        vela.command("workspace new switch")
        time.sleep(pause)
        vela.command("workspace switch 0")
        time.sleep(pause)
        vela.command("magnifier on")
        time.sleep(pause)
        vela.command("magnifier off")
        time.sleep(pause)
        # The snap preview: the bar dragged to the left edge and held there.
        w = vela.state().window(window)
        x, y = w["x"] + 200, w["y"] + 16
        vela.input("move", x, y, "sleep", "50", "down", "sleep", "50", "move", x, y + 10, "sleep", "50",
                   "move", 0, 400, "sleep", "300", "move", 700, 400, "sleep", "50", "up")
        vela.wait_for(lambda _: "animation: snap-preview " in vela.log_text()[mark:], read_state=False,
                      what="the snap preview")
        time.sleep(pause)
        vela.command(f"window {window} close")
        vela.wait_for(lambda _: "animation: close " in vela.log_text()[mark:], read_state=False, what="close")
        time.sleep(pause)
        durations = {}
        for m in PROGRESS.finditer(vela.log_text()[mark:]):
            durations.setdefault(m[1], set()).add(float(m[4]))
        return durations

    def check(self, durations, factor):
        motion_h = (Path(__file__).resolve().parents[2] / "compositor/src/motion.h").read_text()
        scale = {m[1]: float(m[2]) for m in re.finditer(r"#define VELA_MOTION_(\w+)_MS ([\d.]+)", motion_h)}
        self.assertEqual(set(durations), set(self.TOKENS))
        for name, token in self.TOKENS.items():
            self.assertEqual(durations[name], {scale[token] * factor}, f"{name}: {token.lower()} x {factor}")

    def test_each_animation_takes_its_token(self):
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            self.check(self.play(vela), 1.0)

    def test_the_duration_setting_scales_each_one(self):
        # As Settings' slider does: vela.conf, then reload-config.
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            for percent in (200, 50):
                with self.subTest(percent=percent):
                    os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
                    with open(vela.config_path(), "w") as conf:
                        conf.write(f"animation-duration={percent}\n")
                    vela.command("reload-config")
                    self.check(self.play(vela, percent / 100), percent / 100)


GEOMETRY = re.compile(r"animation: (\S+) #(\d+) geometry ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+) opacity ([-\d.]+)")


class AnimationEffects(unittest.TestCase):
    """docs/visual-coherence.md §6.5: with animation-effects=no nothing moves
    (each animation's logged geometry stays put, at scale 1) and what fades
    takes at most `fast`; the magnifier zooms at once and maximizing doesn't
    morph. Settings' vela.conf and reload-config switch it, nothing restarts."""

    def switch(self, vela, on):
        os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
        with open(vela.config_path(), "w") as conf:
            conf.write(f"animation-effects={'yes' if on else 'no'}\n")
        vela.command("reload-config")

    def test_switched_off_nothing_moves(self):
        motion_h = (Path(__file__).resolve().parents[2] / "compositor/src/motion.h").read_text()
        fast, slow = (float(re.search(rf"#define VELA_MOTION_{token}_MS ([\d.]+)", motion_h)[1])
                      for token in ("FAST", "SLOW"))
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            self.switch(vela, False)
            window = vela.open_window(400, 300, decorated=True)
            vela.wait_for(lambda s: s["focused"] == window)
            for action in ("maximize", "restore", "minimize", "restore"):
                vela.command(f"window {window} {action}")
                time.sleep(0.3)
            vela.command("workspace new switch")
            time.sleep(0.3)
            vela.command("workspace switch 0")
            time.sleep(0.3)
            vela.command("magnifier on")
            time.sleep(0.3)
            vela.command("magnifier off")
            time.sleep(0.3)
            w = vela.state().window(window)
            x, y = w["x"] + 200, w["y"] + 16
            vela.input("move", x, y, "sleep", "50", "down", "sleep", "50", "move", x, y + 10, "sleep", "50",
                       "move", 0, 400, "sleep", "300", "move", 700, 400, "sleep", "50", "up")
            vela.wait_for(lambda _: "animation: snap-preview " in vela.log_text(), read_state=False,
                          what="the snap preview")
            time.sleep(0.3)
            vela.command(f"window {window} close")
            vela.wait_for(lambda _: "animation: close " in vela.log_text(), read_state=False, what="close")
            time.sleep(0.3)
            log = vela.log_text()

            durations = {}
            for m in PROGRESS.finditer(log):
                durations.setdefault(m[1], set()).add(float(m[4]))
            self.assertEqual(set(durations), {"open", "minimize", "restore", "desktop", "magnifier", "snap-preview",
                                              "close"}, "no morph")
            for name, values in durations.items():
                self.assertEqual(values, {0.0} if name == "magnifier" else {fast}, name)
            geometry = {}
            for m in GEOMETRY.finditer(log):
                geometry.setdefault((m[1], m[2]), set()).add(tuple(float(v) for v in m.group(3, 4, 5, 6)))
            self.assertTrue({name for name, _ in geometry} >= {"open", "minimize", "restore", "desktop",
                                                                "snap-preview", "close"})
            for (name, serial), places in geometry.items():
                self.assertEqual(len(places), 1, f"{name} #{serial} moved: {sorted(places)}")
                self.assertEqual(next(iter(places))[2:], (1.0, 1.0), f"{name} #{serial} scaled")

            # Back on, without restarting: a new window rises again.
            self.switch(vela, True)
            before = len(log)
            vela.open_window(400, 300)
            opened = [m for m in GEOMETRY.finditer(vela.log_text()[before:]) if m[1] == "open"]
            self.assertGreater(len({m[4] for m in opened}), 1, "the window rose")
            self.assertTrue(all(float(m[4]) == slow for m in PROGRESS.finditer(vela.log_text()[before:])
                                if m[1] == "open"))


SURFACES = re.compile(r"animation: (\S+) #\d+ geometry .* surfaces (\d+)")


class EveryTrigger(unittest.TestCase):
    """Each window animation from every way a person starts it: Vela's title
    bar (its buttons, a double click), the keyboard, dragging to an edge. Each
    one must start its animation, and a snapshot must hold the window's images
    (an empty one animates nothing visible)."""

    def expect(self, names, what, *actions):
        mark = len(self.vela.log_text())
        for action in actions:
            action()

        def started(_):
            text = self.vela.log_text()[mark:]
            return all(f"animation: {name} #" in text for name in names)
        self.vela.wait_for(started, read_state=False, what=f"{what}: {', '.join(names)}")
        time.sleep(0.5) # the animation ends
        for m in SURFACES.finditer(self.vela.log_text()[mark:]):
            self.assertGreater(int(m[2]), 0, f"{what}: the {m[1]} snapshot has none of the app's images")

    def bar(self, window):
        """The title bar's middle and buttons (46 units wide, at the right)."""
        w = self.vela.state().window(window)
        right, y = w["x"] + w["w"], w["y"] + 15
        return {"middle": (w["x"] + w["w"] // 2, y), "close": (right - 23, y),
                "maximize": (right - 69, y), "minimize": (right - 115, y)}

    def click(self, window, part, double=False):
        x, y = self.bar(window)[part]
        actions = ["move", x, y, "sleep", 300, "down", "sleep", 30, "up"]
        if double:
            actions += ["sleep", 60, "down", "sleep", 30, "up"]
        return lambda: self.vela.input(*actions, "sleep", 400) # no double click with the next one

    def drag_to(self, window, x, y):
        bx, by = self.bar(window)["middle"]
        return lambda: self.vela.input("move", bx, by, "sleep", 300, "down", "sleep", 50, "move", bx, by + 10,
                                       "sleep", 50, "move", x, y, "sleep", 300, "up", "sleep", 400)

    def keys(self, *combos):
        return lambda: self.vela.keys(*combos)

    def command(self, line):
        return lambda: self.vela.command(line)

    def test_every_trigger_animates(self):
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            self.vela = vela
            mark = len(vela.log_text())
            window = vela.open_window(500, 360, decorated=True)
            self.assertIn("animation: open #", vela.log_text()[mark:])
            vela.wait_for(lambda s: s["focused"] == window)

            self.expect(["morph"], "maximize button", self.click(window, "maximize"))
            self.expect(["morph"], "restore button", self.click(window, "maximize"))
            self.expect(["morph"], "double click on the bar", self.click(window, "middle", double=True))
            self.expect(["morph"], "double click again", self.click(window, "middle", double=True))
            self.expect(["morph"], "Win+Up", self.keys("super+Up"))
            self.expect(["morph"], "Win+Down from maximized", self.keys("super+Down"))
            self.expect(["morph"], "Win+Left", self.keys("super+Left"))
            self.expect(["morph"], "Win+Down from a half", self.keys("super+Down"))
            self.expect(["morph"], "Win+Up from a quarter", self.keys("super+Up"))
            self.expect(["morph"], "Win+Right from the left half", self.keys("super+Right"))
            self.expect(["snap-preview", "morph"], "dragged to the left edge", self.drag_to(window, 0, 500))
            self.expect(["morph"], "Win+Right back", self.keys("super+Right"))
            self.expect(["snap-preview", "morph"], "dragged to the top", self.drag_to(window, 960, 0))
            self.expect(["morph"], "Win+Down", self.keys("super+Down"))
            self.expect(["minimize"], "minimize button", self.click(window, "minimize"))
            self.expect(["restore"], "restored", self.command(f"window {window} restore"))
            self.expect(["minimize"], "Win+Down from normal", self.keys("super+Down"))
            self.expect(["restore"], "restored again", self.command(f"window {window} restore"))
            self.expect(["desktop"], "Win+Ctrl+D", self.keys("super+ctrl+d"))
            self.expect(["desktop"], "Win+Ctrl+Left", self.keys("super+ctrl+Left"))
            self.expect(["magnifier"], "Win+Plus", self.keys("super+plus"))
            self.expect(["magnifier"], "Win+Esc", self.keys("super+Escape"))
            self.expect(["close"], "close button", self.click(window, "close"))
            other = vela.open_window(500, 360, decorated=True)
            vela.wait_for(lambda s: s["focused"] == other)
            self.expect(["close"], "Alt+F4", self.keys("alt+F4"))


class MorphCorners(unittest.TestCase):
    """docs/visual-coherence.md §6.3: maximizing, the window's corners go away
    while it morphs into the new frame; restoring, they come back. They never
    grow with the snapshot."""

    def test_corners_follow_the_morph(self):
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            window = vela.open_window()
            done = set()
            for action, first, last in (("maximize", 8.0, 0.0), ("restore", 0.0, 8.0)):
                vela.command(f"window {window} {action}")

                def morphed(_):
                    return {int(m[2]) for m in RETIRED.finditer(vela.log_text()) if m[1] == "morph"} - done
                vela.wait_for(morphed, read_state=False, what=f"{action}: the morph retired")
                serial = morphed(None).pop()
                done.add(serial)
                radii = [float(m[2]) for m in RADIUS.finditer(vela.log_text()) if int(m[1]) == serial]
                self.assertAlmostEqual(radii[0], first, places=3, msg=action)
                self.assertAlmostEqual(radii[-1], last, places=3, msg=action)
                steps = [b - a for a, b in zip(radii, radii[1:])]
                self.assertTrue(all(step * (last - first) >= 0 for step in steps), f"{action}: {radii}")


if __name__ == "__main__":
    unittest.main()
