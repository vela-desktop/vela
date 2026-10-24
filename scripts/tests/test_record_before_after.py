"""The timeline of record-before-after.py: which recorded frame each tick of
the video shows."""
from __future__ import annotations

import importlib.util
import io
import math
import subprocess
import types
from unittest import mock
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


class TitleBarRegionTests(unittest.TestCase):
    def test_region_includes_the_ten_unit_downward_drag(self):
        window = {"x": 100, "y": 80, "w": 520}
        vela = mock.Mock()
        vela.open_window.return_value = 42
        vela.wait_for.return_value.window.return_value = window
        with mock.patch.object(script.time, "sleep"):
            region, _ = script.TitleBar().prepare(types.SimpleNamespace(vela=vela))
        lower_edge = region[1] + region[3]
        expected = math.ceil((window["y"] + script.BAR + 10) * script.TitleBar.scale) + 4
        self.assertGreaterEqual(lower_edge, expected)


class DesktopIsolationTests(unittest.TestCase):
    def test_both_buses_use_the_private_address_even_if_scene_overrides(self):
        address = "unix:path=/tmp/vela-test-private-bus"
        scene = types.SimpleNamespace(env={"DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/host-bus"})
        env = script.Desktop.bus_environment(address, scene)
        self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], address)
        self.assertEqual(env["DBUS_SYSTEM_BUS_ADDRESS"], address)


class WorktreeTests(unittest.TestCase):
    """A real small git repository verifies that failed picks are not cached."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vela-worktree-test-")
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.repo = root / "repo"
        self.repo.mkdir()
        self.cache = root / "cache"
        self._original_root, self._original_cache = script.ROOT, script.CACHE
        script.ROOT, script.CACHE = self.repo, self.cache
        self.addCleanup(self.restore_paths)
        self.git("init", "-q")
        self.git("config", "user.name", "Vela test")
        self.git("config", "user.email", "test@example.invalid")
        (self.repo / "example.txt").write_text("before\n")
        self.git("add", "example.txt")
        self.git("commit", "-qm", "base")
        self.base = self.git("rev-parse", "HEAD")
        (self.repo / "example.txt").write_text("after\n")
        self.git("add", "example.txt")
        self.git("commit", "-qm", "pick")
        self.pick = self.git("rev-parse", "HEAD")

    def restore_paths(self):
        script.ROOT, script.CACHE = self._original_root, self._original_cache

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.repo, check=True,
                              capture_output=True, text=True).stdout.strip()

    def build(self):
        real_run = subprocess.run

        def run(args, *positional, **keywords):
            if args[0] == "cmake":
                if "-B" in args:
                    folder = Path(args[args.index("-B") + 1])
                    folder.mkdir(parents=True, exist_ok=True)
                    (folder / "build.ninja").touch()
                return subprocess.CompletedProcess(args, 0)
            return real_run(args, *positional, **keywords)

        with mock.patch.object(script.subprocess, "run", side_effect=run):
            return script.build(self.base, jobs=1, picks=[self.pick])

    def test_cached_pick_is_reused_only_with_exact_git_diff(self):
        out = self.build()
        tree = out.parent
        self.assertEqual((tree / "example.txt").read_text(), "after\n")
        self.assertTrue(script.cached_worktree_valid(tree, self.base, [self.pick]))
        (tree / "example.txt").write_text("tampered\n")
        self.assertFalse(script.cached_worktree_valid(tree, self.base, [self.pick]))
        self.assertEqual(self.build(), out)
        self.assertEqual((tree / "example.txt").read_text(), "after\n")
        self.assertTrue(script.cached_worktree_valid(tree, self.base, [self.pick]))

    def test_interrupted_pick_is_reapplied(self):
        tree = self.build().parent
        (tree / ".vela-before-after-ready.json").unlink()
        self.assertFalse(script.cached_worktree_valid(tree, self.base, [self.pick]))
        self.build()
        self.assertTrue(script.cached_worktree_valid(tree, self.base, [self.pick]))


class RecorderLifecycleTests(unittest.TestCase):
    def test_scene_failure_kills_and_reaps_recorder(self):
        class FakeProcess:
            def __init__(self):
                self.stdout = io.StringIO("recording\n")
                self.status = None
                self.killed = False
                self.reaped = False

            def poll(self):
                return self.status

            def kill(self):
                self.killed = True
                self.status = -9

            def wait(self, timeout=None):
                self.reaped = True
                return self.status

        recorder = FakeProcess()
        vela = mock.Mock()
        vela.state.return_value = types.SimpleNamespace(output={"name": "HEADLESS-1", "frames": 10})
        vela.client_env = {}
        desktop = types.SimpleNamespace(vela=vela)
        scene = mock.Mock()
        scene.prepare.return_value = (None, None)
        scene.lead = 0
        scene.act.side_effect = ValueError("failed scene")
        with (tempfile.TemporaryDirectory() as folder,
              mock.patch.object(script.subprocess, "Popen", return_value=recorder),
              mock.patch.object(script.time, "sleep")):
            with self.assertRaisesRegex(ValueError, "failed scene"):
                script.record(desktop, scene, Path(folder))
        self.assertTrue(recorder.killed)
        self.assertTrue(recorder.reaped)
        self.assertTrue(recorder.stdout.closed)


if __name__ == "__main__":
    unittest.main()
