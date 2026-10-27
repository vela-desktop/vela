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
import re
import threading
import time
import unittest

from harness import Session


def profile_summary(label, values):
    """Nearest-rank percentiles, no third-party dependencies or changed assertions."""
    if not values:
        print(f"capture-profile {label}: no samples", flush=True)
        return
    ordered = sorted(values)
    p95 = ordered[(95 * len(ordered) + 99) // 100 - 1]
    print(f"capture-profile {label}: n={len(ordered)} min_ms={ordered[0]:.3f} "
          f"p50_ms={ordered[(len(ordered) - 1) // 2]:.3f} p95_ms={p95:.3f} "
          f"max_ms={ordered[-1]:.3f} sum_ms={sum(ordered):.3f}", flush=True)


class Captures(unittest.TestCase):
    def test_captures_during_animations(self):
        # Optional, separately dispatched diagnostic comparisons. The normal
        # GPU suite retains the same 2560x1440@180 workload and strict oracle.
        hz = int(os.environ.get("VELA_CAPTURE_TEST_HZ", "180"))
        if hz not in (60, 120, 180):
            raise ValueError("VELA_CAPTURE_TEST_HZ must be 60, 120 or 180")
        resolution = os.environ.get("VELA_CAPTURE_TEST_SIZE", "2560x1440")
        if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", resolution):
            raise ValueError("VELA_CAPTURE_TEST_SIZE must be WIDTHxHEIGHT")
        profiling = os.environ.get("VELA_CAPTURE_PROFILE") == "1"
        with Session(size=f"{resolution}@{hz}") as vela:
            window = vela.open_window(400, 300, decorated=True)
            vela.wait_still(window)
            path = os.path.join(vela.directory, "capture.png")
            stop = threading.Event()
            captures = 0
            capture_error = None
            client_times = []

            def capture():
                # The compositor copies the whole output whatever the region:
                # a small one only keeps PNG encoding out of the measure.
                nonlocal captures, capture_error
                try:
                    while not stop.is_set():
                        started = time.perf_counter() if profiling else 0.0
                        vela.screenshot(path, region=(0, 0, 8, 8))
                        if profiling:
                            client_times.append((time.perf_counter() - started) * 1000.0)
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
            if profiling:
                print(f"capture-profile test resolution={resolution} hz={hz} "
                      f"captures={captures} missed_vblanks={missed}", flush=True)
                profile_summary("screenshot-client", client_times)
                timings = {key: [] for key in ("staging", "record", "submit", "wait", "copy", "total")}
                for line in vela.log_text().splitlines():
                    if "capture-profile staging-memory " in line:
                        print(line[line.index("capture-profile staging-memory "):], flush=True)
                    if "capture-profile readback " in line:
                        for key, value in re.findall(r"\b(staging|record|submit|wait|copy|total)_ms=([0-9.]+)", line):
                            timings[key].append(float(value))
                for key, values in timings.items():
                    profile_summary(f"readback-{key}", values)
            # Uncached readback can be slow, so verify repeated successful
            # captures there without imposing the cached path's rate/latency bound.
            uncached = "Vulkan readback staging: uncached fallback" in vela.log_text()
            self.assertGreaterEqual(captures, 3 if uncached else 20, "too few captures to tell")
            if not uncached:
                self.assertLess(missed, captures, f"{missed} vblanks missed during {captures} captures")


if __name__ == "__main__":
    unittest.main()
