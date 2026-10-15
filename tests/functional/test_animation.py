# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The animation clock (docs/visual-coherence.md §6.2): each output's frame
shows every animation at its own predicted presentation time, and what can't
be undone waits for the wall clock. Read from the VELA_DEBUG_ANIMATION log,
with two outputs at 75 and 180 Hz, with late latching and without."""

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
            self.assertLessEqual(count, durations[serial] * HZ[output] / 1000 + 4, f"{names[serial]} #{serial} on {output}")


class Durations(unittest.TestCase):
    """docs/visual-coherence.md §6.4: every window animation lasts one of the
    four durations of motion.h (the ones Theme.qml shares)."""

    TOKENS = {"open": "SLOW", "close": "NORMAL", "minimize": "SLOW", "restore": "SLOW", "morph": "SLOW",
              "desktop": "LONG", "snap-preview": "FAST", "magnifier": "FAST"}

    def test_each_animation_takes_its_token(self):
        motion_h = (Path(__file__).resolve().parents[2] / "compositor/src/motion.h").read_text()
        scale = {m[1]: float(m[2]) for m in re.finditer(r"#define VELA_MOTION_(\w+)_MS ([\d.]+)", motion_h)}
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            window = vela.open_window(400, 300, decorated=True)
            vela.wait_for(lambda s: s["focused"] == window)
            for action in ("maximize", "restore", "minimize", "restore"):
                vela.command(f"window {window} {action}")
                time.sleep(0.5)
            vela.command("workspace new switch")
            time.sleep(0.5)
            vela.command("workspace switch 0")
            time.sleep(0.5)
            vela.command("magnifier on")
            time.sleep(0.3)
            vela.command("magnifier off")
            time.sleep(0.3)
            # The snap preview: the bar dragged to the left edge and held there.
            w = vela.state().window(window)
            x, y = w["x"] + 200, w["y"] + 16
            vela.input("move", x, y, "sleep", "50", "down", "sleep", "50", "move", x, y + 10, "sleep", "50",
                       "move", 0, 400, "sleep", "300", "move", 700, 400, "sleep", "50", "up")
            vela.wait_for(lambda _: "animation: snap-preview " in vela.log_text(), read_state=False,
                          what="the snap preview")
            time.sleep(0.4)
            vela.command(f"window {window} close")
            vela.wait_for(lambda _: "animation: close " in vela.log_text(), read_state=False, what="close")
            time.sleep(0.4)
            durations = {}
            for m in PROGRESS.finditer(vela.log_text()):
                durations.setdefault(m[1], set()).add(float(m[4]))
        self.assertEqual(set(durations), set(self.TOKENS))
        for name, token in self.TOKENS.items():
            self.assertEqual(durations[name], {scale[token]}, f"{name}: {token.lower()}")


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
