"""Offline regression tests for the single-build promo capture workflow."""
import importlib.util
from pathlib import Path
import tempfile
import types
import unittest
import sys
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "record-promo.py"
sys.path.insert(0, str(SCRIPT.parent))  # import the sibling recording_core.py
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
                self.assertFalse(list(output.glob(".vela-promo-*.ffconcat")))

    def test_existing_playlist_symlink_is_not_touched(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            victim = output / "important.txt"
            victim.write_text("private data")
            (output / ".promo-concat.txt").symlink_to(victim)
            with mock.patch.object(promo.subprocess, "run"):
                promo.assemble_reel([output / "desktop.mp4"], output)
            self.assertEqual(victim.read_text(), "private data")
            self.assertFalse(list(output.glob(".vela-promo-*.ffconcat")))

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


    def test_promo_capture_preserves_action_start(self):
        import io
        recorder = mock.Mock()
        recorder.stdout = io.StringIO("ready\\n")
        recorder.poll.return_value = 0
        recorder.wait.return_value = 0
        vela = mock.Mock()
        vela.state.return_value = types.SimpleNamespace(output={"name": "HEADLESS-1", "frames": 10})
        vela.client_env = {}
        scene = mock.Mock()
        scene.prepare.return_value = (None, None)
        scene.lead = 0
        scene.tail = 0
        take = mock.Mock()
        with (tempfile.TemporaryDirectory() as folder,
              mock.patch.object(promo.capture.subprocess, "Popen", return_value=recorder),
              mock.patch.object(promo.capture, "Take", return_value=take),
              mock.patch.object(promo.capture.time, "sleep")):
            result = promo.capture.record(types.SimpleNamespace(vela=vela), scene, Path(folder),
                                          align=False)
        self.assertIs(result, take)
        take.align.assert_not_called()

    def test_lossless_encoder_is_reaped_after_pipe_failure(self):
        encoder = mock.Mock()
        encoder.stdin.write.side_effect = BrokenPipeError("encoder quit")
        encoder.stdin.closed = False
        encoder.poll.return_value = None
        take = types.SimpleNamespace(width=1, height=1, format="bgr0")
        with mock.patch.object(promo.capture.subprocess, "Popen", return_value=encoder):
            with self.assertRaises(BrokenPipeError):
                promo.capture.lossless(take, [b"abcd"], Path("/tmp/ignored.mkv"))
        encoder.kill.assert_called_once()
        encoder.wait.assert_called_once()
        encoder.stdin.close.assert_called_once()

    def test_isolated_environment_overrides_host_paths_and_identifiers(self):
        scene = types.SimpleNamespace(env={"HOME": "/home/real-user",
                                           "XDG_RUNTIME_DIR": "/run/user/1000",
                                           "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/host-bus"})
        private = Path("/tmp/vela-test-isolated")
        env = promo.Desktop.isolated_environment("unix:path=/private-bus", scene,
                                                  private / "home", private / "runtime")
        self.assertEqual(env["HOME"], str(private / "home"))
        self.assertEqual(env["XDG_RUNTIME_DIR"], str(private / "runtime"))
        self.assertEqual(env["XDG_STATE_HOME"], str(private / "home" / ".local" / "state"))
        self.assertEqual(env["USER"], "alex")
        self.assertEqual(env["LOGNAME"], "alex")
        self.assertEqual(env["DBUS_SYSTEM_BUS_ADDRESS"], "unix:path=/private-bus")

    def test_real_settings_required_for_window_clips(self):
        desktop = mock.Mock()
        desktop.open_app.return_value = None
        for scene in (promo.WindowMotion(), promo.TaskView()):
            with self.subTest(scene=scene.name):
                with self.assertRaisesRegex(RuntimeError, "build vela-settings"):
                    scene.prepare(desktop)
        desktop.vela.open_window.assert_not_called()

    def test_reel_rejects_untrusted_input_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            with mock.patch.object(promo.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "known scenes"):
                    promo.assemble_reel([output.parent / "elsewhere.mp4"], output)
            run.assert_not_called()

    def test_symlink_is_never_followed_even_with_force(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            victim = output / "important.txt"
            victim.write_text("do not overwrite")
            (output / "desktop.mp4").symlink_to(victim)
            with self.assertRaises(SystemExit) as raised:
                promo.main(["desktop", "--out", str(output), "--force"])
            self.assertEqual(raised.exception.code, 2)
            self.assertEqual(victim.read_text(), "do not overwrite")


if __name__ == "__main__":
    unittest.main()
