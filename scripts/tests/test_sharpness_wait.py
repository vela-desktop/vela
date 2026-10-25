# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for the event-driven sharpness settling gate (no GPU needed)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import sharpness_wait  # noqa: E402


class FakeClock:
    def __init__(self):
        self.value = 0.0

    def clock(self):
        return self.value

    def sleep(self, seconds):
        self.value += seconds


def state(frames=1, x=100, mapped=True):
    return {
        "outputs": [{"name": "HEADLESS-1", "frames": frames}],
        "windows": ([{"id": "pattern", "x": x, "y": 50, "w": 401, "h": 301,
                     "maximized": False, "minimized": False, "snap": None}]
                    if mapped else []),
    }


class SharpnessWaitTests(unittest.TestCase):
    def wait(self, read, clock, timeout=1.0):
        return sharpness_wait.wait_for_settled(
            read, timeout=timeout, quiet=0.15, poll=0.05,
            clock=clock.clock, sleep=clock.sleep)

    def test_requires_mapped_window_and_quiet_frames(self):
        clock = FakeClock()
        samples = [state(mapped=False), state(frames=2), state(frames=3),
                   state(frames=3), state(frames=3), state(frames=3)]
        def read():
            return samples.pop(0) if samples else state(frames=3)
        self.wait(read, clock)
        self.assertGreaterEqual(clock.clock(), 0.30)
        self.assertLess(clock.clock(), 0.50)

    def test_geometry_changes_restart_settling(self):
        clock = FakeClock()
        samples = [state(x=10), state(x=20), state(x=30), state(x=30)]
        def read():
            return samples.pop(0) if samples else state(x=30)
        self.wait(read, clock)
        self.assertGreaterEqual(clock.clock(), 0.25)

    def test_continuous_rendering_never_qualifies(self):
        clock = FakeClock()
        frame = 0
        def read():
            nonlocal frame
            frame += 1
            return state(frames=frame)
        with self.assertRaises(TimeoutError):
            self.wait(read, clock, timeout=0.35)

    def test_unmapped_window_never_qualifies(self):
        clock = FakeClock()
        with self.assertRaises(TimeoutError):
            self.wait(lambda: state(mapped=False), clock, timeout=0.35)

    def test_transient_socket_error_can_recover(self):
        clock = FakeClock()
        attempts = 0
        def read():
            nonlocal attempts
            attempts += 1
            if attempts < 3:
                raise OSError("socket not ready")
            return state()
        self.wait(read, clock)
        self.assertGreaterEqual(attempts, 5)


if __name__ == "__main__":
    unittest.main()
