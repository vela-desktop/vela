# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Capturing the screen doesn't slow it down.

A capture into shared memory is copied by the GPU into a staging buffer, which
the compositor then reads with the CPU, inside the output's frame. From
uncached memory, the kind uploads use, reading a 1440p output
took longer than a frame at 180 Hz: about three missed vblanks per capture,
for as long as a screen recording or a screen share lasted. Readback staging
is cached (render/renderer.c), and reading it takes a fraction of a frame."""

import os
import threading
import time
import unittest

from harness import Session


class Captures(unittest.TestCase):
    def test_captures_during_animations_dont_miss_vblanks(self):
        with Session(size="2560x1440@180") as vela:
            window = vela.open_window(400, 300, decorated=True)
            vela.wait_still(window)
            path = os.path.join(vela.directory, "capture.png")
            stop = threading.Event()
            captures = 0

            def capture():
                # The compositor copies the whole output whatever the region:
                # a small one only keeps PNG encoding out of the measure.
                nonlocal captures
                while not stop.is_set():
                    vela.screenshot(path, region=(0, 0, 8, 8))
                    captures += 1
                    time.sleep(0.05)

            before = vela.state().output["missed"]
            thread = threading.Thread(target=capture)
            thread.start()
            try:
                for _ in range(4):
                    vela.command(f"window {window} maximize")
                    time.sleep(0.35)
                    vela.command(f"window {window} restore")
                    time.sleep(0.35)
            finally:
                stop.set()
                thread.join()
            missed = vela.state().output["missed"] - before
            # Uncached: about 120 missed for 38 captures, every time. Cached:
            # usually none, a few dozen at worst on a busy machine.
            self.assertGreaterEqual(captures, 20, "too few captures to tell")
            self.assertLess(missed, captures, f"{missed} vblanks missed during {captures} captures")


if __name__ == "__main__":
    unittest.main()
