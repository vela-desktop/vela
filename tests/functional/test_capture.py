# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Capture correctness during animations, and speed when cached readback is available.

A capture into shared memory is copied by the GPU into a staging buffer, which
the compositor then reads with the CPU, inside the output's frame. From
uncached memory, the kind uploads use, reading a 1440p output
took longer than a frame at 180 Hz: about three missed vblanks per capture,
for as long as a screen recording or a screen share lasted. Readback staging
is preferably cached (render/renderer.c); supported GPUs read it in a fraction of a frame."""

import os
import threading
import time
import unittest

from harness import Session


class Captures(unittest.TestCase):
    def test_captures_during_animations(self):
        with Session(size="2560x1440@180") as vela:
            window = vela.open_window(400, 300, decorated=True)
            vela.wait_still(window)
            path = os.path.join(vela.directory, "capture.png")
            stop = threading.Event()
            captures = 0
            capture_error = None
            diagnostic = os.environ.get("VELA_READBACK_COPY_DIAG") == "1"
            capture_times = []

            def capture():
                # The compositor copies the whole output whatever the region:
                # a small one only keeps PNG encoding out of the measure.
                nonlocal captures, capture_error
                try:
                    while not stop.is_set():
                        started = time.perf_counter() if diagnostic else 0.0
                        vela.screenshot(path, region=(0, 0, 8, 8))
                        if diagnostic:
                            capture_times.append((time.perf_counter() - started) * 1000.0)
                        captures += 1
                        time.sleep(0.05)
                except Exception as error:
                    capture_error = error

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
            if capture_error is not None:
                raise capture_error
            missed = vela.state().output["missed"] - before
            if diagnostic:
                ordered = sorted(capture_times)
                median = ordered[(len(ordered) - 1) // 2] if ordered else 0.0
                print(f"readback-copy diagnostic workers={os.environ.get('VELA_READBACK_COPY_THREADS', '1')} "
                      f"captures={captures} missed_vblanks={missed} screenshot_p50_ms={median:.3f}",
                      flush=True)
            # Uncached readback can be slow, so verify repeated successful
            # captures there without imposing the cached path's rate/latency bound.
            uncached = "Vulkan readback staging: uncached fallback" in vela.log_text()
            self.assertGreaterEqual(captures, 3 if uncached else 20, "too few captures to tell")
            if not uncached:
                self.assertLess(missed, captures, f"{missed} vblanks missed during {captures} captures")


if __name__ == "__main__":
    unittest.main()
