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


def fake_ffmpeg(command, check):
    """Writes the output file, the last argument, as FFmpeg would."""
    Path(command[-1]).write_bytes(b"video")


class PromoTests(unittest.TestCase):
    def test_scenes_are_unique_and_use_full_output(self):
        self.assertEqual(list(promo.SCENES), ["desktop", "start-menu", "quick-settings",
                                              "window-motion", "task-view", "settings"])
        for scene in promo.SCENES.values():
            self.assertEqual(scene.size, "1920x1080@60")
            self.assertEqual(scene.scale, 1.0)
            self.assertEqual(scene.speed, 1.0)

    def test_catalog_does_not_require_a_gpu_or_build(self):
        with mock.patch.object(promo.capture.harness, "gpu_available", side_effect=AssertionError("GPU required")):
            promo.main(["--list"])

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
            staging = output / ".staging"
            staging.mkdir()
            with mock.patch.object(promo.subprocess, "run", side_effect=fake_ffmpeg) as run:
                promo.assemble_reel([output / "desktop.mp4", output / "start-menu.mp4"], output, staging)
                self.assertTrue(run.called)
                self.assertFalse(list(output.glob(".vela-promo-*.ffconcat")))
            self.assertEqual((output / "vela-promo.mp4").read_bytes(), b"video")

    def test_existing_playlist_symlink_is_not_touched(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            victim = output / "important.txt"
            victim.write_text("private data")
            (output / ".promo-concat.txt").symlink_to(victim)
            (output / "vela-promo.mp4").symlink_to(victim)
            staging = output / ".staging"
            staging.mkdir()
            with mock.patch.object(promo.subprocess, "run", side_effect=fake_ffmpeg):
                promo.assemble_reel([output / "desktop.mp4"], output, staging)
            self.assertFalse((output / "vela-promo.mp4").is_symlink())
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

    def test_bus_daemon_runs_in_the_private_environment(self):
        bus = mock.Mock()
        bus.stdout.readline.return_value = ""  # stop right after the daemon starts
        host = {"HOME": "/home/real-user", "XDG_RUNTIME_DIR": "/run/user/1000",
                "XDG_DATA_HOME": "/home/real-user/.local/share",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
                "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=/run/dbus/system_bus_socket"}
        desktop = promo.Desktop(Path("/nonexistent-build"), promo.StartMenu())
        with (mock.patch.dict(promo.capture.os.environ, host),
              mock.patch.object(promo.capture.subprocess, "Popen", return_value=bus) as popen):
            with self.assertRaisesRegex(RuntimeError, "D-Bus"):
                desktop.__enter__()
        command, env = popen.call_args.args[0], popen.call_args.kwargs["env"]
        private = desktop.private_runtime.parent
        address = f"unix:path={private / 'runtime' / 'bus'}"
        self.assertIn(f"--address={address}", command)
        self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], address)
        self.assertEqual(env["DBUS_SYSTEM_BUS_ADDRESS"], address)
        self.assertEqual(env["HOME"], str(private / "home"))
        self.assertEqual(env["XDG_RUNTIME_DIR"], str(private / "runtime"))
        self.assertNotIn("XDG_DATA_HOME", env)
        self.assertFalse(private.exists())

    def test_audio_waits_for_outputs_default_and_music(self):
        def node(**props):
            return {"type": "PipeWire:Interface:Node", "info": {"props": props}}
        outputs = [node(**{"node.name": "speakers"}), node(**{"node.name": "headphones"})]
        default = {"type": "PipeWire:Interface:Metadata", "props": {"metadata.name": "default"},
                   "metadata": [{"subject": 0, "key": "default.audio.sink", "value": {"name": "speakers"}}]}
        music = node(**{"application.name": "Music", "media.class": "Stream/Output/Audio"})
        ready = promo.Desktop.audio_ready
        self.assertTrue(ready([*outputs, default, music]))
        self.assertFalse(ready([*outputs, music]))
        self.assertFalse(ready([outputs[0], default, music]))
        self.assertFalse(ready([*outputs, default]))
        self.assertFalse(ready([{"type": "PipeWire:Interface:Core", "info": None}]))

    def test_settings_scenes_need_user_namespaces(self):
        refused = types.SimpleNamespace(returncode=1, stderr="unshare: write failed /proc/self/uid_map")
        with mock.patch.object(promo.capture.subprocess, "run", return_value=refused):
            with self.assertRaisesRegex(RuntimeError, "user and mount namespaces.*uid_map"):
                promo.capture.check_user_namespaces()

    def test_output_planted_as_symlink_after_checks_is_replaced(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "out"
            output.mkdir()
            staging = output / ".staging"
            staging.mkdir()
            victim = Path(folder) / "important.txt"
            victim.write_text("do not overwrite")
            take = types.SimpleNamespace(width=1920, height=1080, times=[1.0], presented=1,
                                         format="bgr0", output_info={})

            def export(scene, take, where, scratch):
                for suffix in (".mp4", ".webm", ".png"):
                    (where / f"{scene.name}{suffix}").write_bytes(b"video")
                    # Planted between the preflight and the install.
                    (output / f"{scene.name}{suffix}").symlink_to(victim)
                (output / f"{scene.name}-manifest.json").symlink_to(victim)
                return where / f"{scene.name}.mp4"

            args = types.SimpleNamespace(keep=False, no_reel=True)
            with (mock.patch.object(promo, "Desktop"), mock.patch.object(promo, "record", return_value=take),
                  mock.patch.object(promo, "export_clip", side_effect=export)):
                promo.record_scenes(["desktop"], Path("/build"), output, staging, args)
            self.assertEqual(victim.read_text(), "do not overwrite")
            for suffix in (".mp4", ".webm", ".png", "-manifest.json"):
                self.assertFalse((output / f"desktop{suffix}").is_symlink())
            self.assertIn('"scene": "desktop"', (output / "desktop-manifest.json").read_text())

    def test_staging_is_removed_when_a_scene_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            with (mock.patch.object(promo.capture.harness, "gpu_available", return_value=True),
                  mock.patch.object(promo.Path, "is_file", return_value=True),
                  mock.patch.object(promo.shutil, "which", return_value="/usr/bin/true"),
                  mock.patch.object(promo, "record_scenes", side_effect=RuntimeError("no GPU after all"))):
                with self.assertRaisesRegex(RuntimeError, "no GPU"):
                    promo.main(["desktop", "--out", str(output)])
            self.assertEqual(list(output.iterdir()), [])


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
                    promo.assemble_reel([output.parent / "elsewhere.mp4"], output, output)
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
