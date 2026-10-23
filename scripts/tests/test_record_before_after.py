"""The timeline of record-before-after.py: which recorded frame each tick of
the video shows."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "record-before-after.py"
spec = importlib.util.spec_from_file_location("vela_record_before_after", SCRIPT)
assert spec and spec.loader
script = importlib.util.module_from_spec(spec)
spec.loader.exec_module(script)


def take(directory: Path, times: list[float], start: float, end: float):
    """A 1×1 recording whose frame i is the pixel (i, i, i, 0)."""
    (directory / "frames.raw").write_bytes(b"".join(bytes((i, i, i, 0)) for i in range(len(times))))
    (directory / "frames.txt").write_text("1 1 bgr0\n" + "".join(f"{t:.9f}\n" for t in times))
    return script.Take(directory, start, end, presented=len(times))


class TimelineTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory)

    def shown(self, recording, duration, speed):
        return [frame[0] for frame in recording.frames(duration, speed)]

    def test_each_tick_shows_the_last_frame_presented(self):
        fps, lead = script.FPS, script.LEAD
        # Frames at 120 Hz from the lead's start (a little before each tick,
        # never exactly on it): two per tick at full speed.
        times = [100 - lead + (i - 0.1) / 120 for i in range(120)]
        recording = take(self.directory, times, 100, 100.5)
        self.assertEqual(self.shown(recording, 10 / fps, 1.0), [0, 2, 4, 6, 8, 10, 12, 14, 16, 18])

    def test_slower_repeats_frames(self):
        times = [100 - script.LEAD + (i - 0.1) / script.FPS for i in range(10)]
        recording = take(self.directory, times, 100, 100.1)
        self.assertEqual(self.shown(recording, 8 / script.FPS, 0.25), [0, 0, 0, 0, 1, 1, 1, 1])

    def test_a_still_screen_holds_its_frame(self):
        # Nothing presented between 0.1 and 0.5 s: frame 1 stays.
        times = [100 - script.LEAD - 0.001, 100 - script.LEAD + 0.099, 100 - script.LEAD + 0.499]
        recording = take(self.directory, times, 100, 100.2)
        shown = self.shown(recording, 0.6, 1.0)
        self.assertEqual(shown[:6], [0] * 6)
        self.assertEqual(set(shown[6:30]), {1})
        self.assertEqual(set(shown[30:]), {2})

    def test_before_the_first_frame_shows_the_first(self):
        times = [100.0, 100.1]
        recording = take(self.directory, times, 100, 100.1)
        self.assertEqual(self.shown(recording, 3 / script.FPS, 1.0), [0, 0, 0])


class AlignTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory)

    def recording(self, pixels, times, start):
        (self.directory / "frames.raw").write_bytes(b"".join(bytes((p, p, p, 0)) for p in pixels))
        (self.directory / "frames.txt").write_text("1 1 bgr0\n" + "".join(f"{t:.9f}\n" for t in times))
        return script.Take(self.directory, start, start + 1, presented=len(times))

    def test_starts_at_the_first_new_image_after_the_command(self):
        # A change before the command doesn't count, nor a frame that
        # repeats the image before it.
        recording = self.recording([0, 5, 5, 5, 9, 9], [0.0, 0.5, 1.0, 1.1, 1.2, 1.3], start=0.9)
        recording.align()
        self.assertEqual(recording.start, 1.2)

    def test_without_a_change_it_keeps_the_command(self):
        recording = self.recording([3, 3, 3], [0.0, 1.0, 2.0], start=0.5)
        recording.align()
        self.assertEqual(recording.start, 0.5)


if __name__ == "__main__":
    unittest.main()
