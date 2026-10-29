"""Offline regression tests for the single-build promo capture workflow."""
import importlib.util
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "record-promo.py"
spec = importlib.util.spec_from_file_location("vela_record_promo", SCRIPT)
assert spec and spec.loader
promo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(promo)


class PromoTests(unittest.TestCase):
    def test_scenes_are_unique_and_use_full_output(self):
        self.assertEqual(len(promo.SCENES), len(set(promo.SCENES)))
        for scene in promo.SCENES.values():
            self.assertEqual(scene.size, "1920x1080@60")
            self.assertEqual(scene.scale, 1.0)
            self.assertEqual(scene.speed, 1.0)

    def test_catalog_does_not_require_a_gpu_or_build(self):
        with mock.patch.object(promo.capture.harness, "gpu_available", side_effect=AssertionError("GPU required")):
            self.assertEqual(promo.main(["--list"]), 0)

    def test_scene_capture_produces_both_encodings_and_poster(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            take = types.SimpleNamespace(start=1.0, end=2.0,
                                         frames=mock.Mock(return_value=iter([b"abcd"])))
            with (mock.patch.object(promo, "lossless") as lossless,
                  mock.patch.object(promo.subprocess, "run") as run):
                mp4 = promo.export_clip(promo.StartMenu(), take, path, path)
            self.assertEqual(mp4, path / "start-menu.mp4")
            self.assertEqual(lossless.call_count, 1)
            self.assertEqual(run.call_count, 3)
            self.assertTrue(all(call.kwargs.get("check") for call in run.call_args_list))
            self.assertIn("libx264", run.call_args_list[0].args[0])
            self.assertIn("libvpx-vp9", run.call_args_list[1].args[0])
            self.assertIn("start-menu.png", str(run.call_args_list[2]))

    def test_reel_inputs_are_allowlisted_scene_names(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            with mock.patch.object(promo.subprocess, "run") as run:
                promo.assemble_reel([output / "desktop.mp4", output / "start-menu.mp4"], output)
                self.assertTrue(run.called)
                self.assertFalse((output / ".promo-concat.txt").exists())

    def test_does_not_overwrite_previous_take_by_default(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            (output / "desktop.mp4").touch()
            with self.assertRaises(SystemExit) as raised:
                promo.main(["desktop", "--out", str(output)])
            self.assertEqual(raised.exception.code, 2)

    def test_rejects_unknown_or_duplicate_scenes(self):
        with self.assertRaises(SystemExit):
            promo.main(["not-a-scene"])
        with self.assertRaises(SystemExit):
            promo.main(["desktop", "desktop"])

    def test_promo_reuses_isolated_private_buses(self):
        scene = promo.StartMenu()
        address = "unix:path=/tmp/vela-private-bus"
        env = promo.Desktop.bus_environment(address, scene)
        self.assertEqual(env["DBUS_SYSTEM_BUS_ADDRESS"], address)
        self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], address)


if __name__ == "__main__":
    unittest.main()
