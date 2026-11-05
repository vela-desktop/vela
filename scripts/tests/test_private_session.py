# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""GPU-free regression tests for the manual Vela session launchers."""

import ctypes
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
COMPOSITOR = '''#!/usr/bin/env python3
import json, os, pathlib, signal, subprocess, sys, time
path = pathlib.Path(os.environ["VELA_TEST_OUTPUT"])
keys = ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
        "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
        "WAYLAND_DISPLAY", "DISPLAY", "QT_WAYLAND_RECONNECT",
        "WLR_BACKENDS", "WLR_LIBINPUT_NO_DEVICES", "VELA_DEBUG_INPUT")
if os.environ.get("VELA_TEST_IGNORE_TERM") == "1":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
elif os.environ.get("VELA_TEST_HOLD") == "1":
    def stop(*args):
        pathlib.Path(os.environ["VELA_TEST_SIGNAL_OUTPUT"]).write_text("terminated")
        sys.exit(0)
    signal.signal(signal.SIGTERM, stop)
result = {"env": {key: os.environ.get(key) for key in keys}, "args": sys.argv[1:]}
result["imported"] = {}
result["outside"] = {}
result["linked"] = {}
for name in ("vela", "Vela"):
    pref = pathlib.Path(os.environ["XDG_CONFIG_HOME"]) / name / "setting.txt"
    result["imported"][name] = pref.read_text() if pref.exists() else None
    result["linked"][name] = pref.is_symlink()
    result["outside"][name] = (pathlib.Path(os.environ["XDG_CONFIG_HOME"]) / name / "external").exists()
for name in ("XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "HOME"):
    (pathlib.Path(os.environ[name]) / "test-write").write_text("private")
path.write_text(json.dumps(result))
if os.environ.get("VELA_TEST_ORPHAN") == "1":
    orphan = subprocess.Popen(
        [sys.executable, "-c",
         "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(120)"],
        start_new_session=True)
    pathlib.Path(os.environ["VELA_TEST_ORPHAN_PID"]).write_text(str(orphan.pid))
if os.environ.get("VELA_TEST_HOLD") == "1":
    while True:
        time.sleep(0.1)
sys.exit(int(os.environ.get("VELA_TEST_EXIT", "0")))
'''


class PrivateSessionTests(unittest.TestCase):
    def setUp(self):
        """Create a fake Vela build and a sentinel-protected host environment."""
        work = tempfile.TemporaryDirectory(prefix="vela-script-test-")
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.build = self.root / "build"
        self.tmp = self.root / "tmp"
        self.tmp.mkdir()
        (self.build / "compositor").mkdir(parents=True)
        executable = self.build / "compositor" / "vela-compositor"
        executable.write_text(COMPOSITOR)
        executable.chmod(0o755)
        self.output = self.root / "output.json"
        self.host = self.root / "host"
        for directory in (self.host / ".cache", self.host / ".config",
                          self.host / ".local" / "share", self.host / ".local" / "state",
                          self.host / "runtime"):
            directory.mkdir(parents=True, exist_ok=True)
        self.sentinel = self.host / ".cache" / "vela" / "wallpaper-tint"
        self.sentinel.parent.mkdir()
        self.sentinel.write_text("real session must not change")
        self.env = dict(os.environ,
                        HOME=str(self.host), TMPDIR=str(self.tmp),
                        XDG_CONFIG_HOME=str(self.host / ".config"),
                        XDG_DATA_HOME=str(self.host / ".local" / "share"),
                        XDG_CACHE_HOME=str(self.host / ".cache"),
                        XDG_STATE_HOME=str(self.host / ".local" / "state"),
                        XDG_RUNTIME_DIR=str(self.host / "runtime"),
                        DBUS_SESSION_BUS_ADDRESS="unix:path=/host/bus",
                        WAYLAND_DISPLAY="wayland-host", DISPLAY=":18",
                        QT_WAYLAND_RECONNECT="1", VELA_TEST_OUTPUT=str(self.output))
        self.env.pop("VELA_TEST_COPY_CONFIG", None)

    @staticmethod
    def load_helper():
        """Import the hyphenated launcher module for focused lifecycle tests."""
        spec = importlib.util.spec_from_file_location("vela_private_session_tests",
                                                     SCRIPTS / "private-session.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def run_script(self, name, **variables):
        """Run the selected launcher with per-test environment overrides."""
        return subprocess.run(["sh", str(SCRIPTS / name), str(self.build)],
                              env=dict(self.env, **variables), text=True,
                              capture_output=True, timeout=10)

    def assert_clean(self):
        """Require the private directory to disappear and host cache to stay intact."""
        self.assertEqual(list(self.tmp.iterdir()), [])
        self.assertEqual(self.sentinel.read_text(), "real session must not change")

    def assert_isolated(self, result):
        """Check every persistent XDG path belongs to one private session."""
        values = result["env"]
        roots = [Path(values[key]) for key in
                 ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME")]
        self.assertEqual(len(set(roots)), 5)
        self.assertEqual(len({p.parent for p in roots}), 1)
        self.assertNotEqual(roots[0], self.host)
        self.assertEqual(values["XDG_RUNTIME_DIR"], str(self.host / "runtime"))
        self.assertEqual(values["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/host/bus")
        self.assertEqual(result["imported"], {"vela": None, "Vela": None})

    def test_nested_preserves_host_wayland_and_isolates_state(self):
        """Nested sessions must retain the host display but not its persistent state."""
        run = self.run_script("run-nested.sh")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.output.read_text())
        self.assert_isolated(result)
        self.assertEqual(result["env"]["WAYLAND_DISPLAY"], "wayland-host")
        self.assertEqual(result["env"]["DISPLAY"], ":18")
        self.assert_clean()

    def test_headless_drops_host_displays_and_enables_virtual_input(self):
        """Headless sessions must drop host displays and enable virtual input."""
        run = self.run_script("run-headless.sh")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.output.read_text())
        self.assert_isolated(result)
        for key in ("WAYLAND_DISPLAY", "DISPLAY", "QT_WAYLAND_RECONNECT"):
            self.assertIsNone(result["env"][key], key)
        self.assertEqual(result["env"]["WLR_BACKENDS"], "headless")
        self.assertEqual(result["env"]["WLR_LIBINPUT_NO_DEVICES"], "1")
        self.assertEqual(result["env"]["VELA_DEBUG_INPUT"], "1")
        self.assert_clean()

    def test_nonzero_compositor_status_survives_cleanup(self):
        """Preserve the compositor exit code after cleaning up its descendants."""
        run = self.run_script("run-nested.sh", VELA_TEST_EXIT="37")
        self.assertEqual(run.returncode, 37, run.stderr)
        self.assert_clean()

    def test_opt_in_config_import_is_a_copy(self):
        """Import only safe, dereferenced Vela configuration files."""
        for name in ("vela", "Vela"):
            folder = self.host / ".config" / name
            folder.mkdir()
            (folder / "original.txt").write_text("copied locally")
            (folder / "setting.txt").symlink_to("original.txt")
            (folder / "external").symlink_to(self.sentinel)
            (folder / "cycle").symlink_to(".")
        run = self.run_script("run-nested.sh", VELA_TEST_COPY_CONFIG="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.output.read_text())
        self.assertEqual(result["imported"], {"vela": "copied locally", "Vela": "copied locally"})
        self.assertEqual(result["linked"], {"vela": False, "Vela": False})
        self.assertEqual(result["outside"], {"vela": False, "Vela": False})
        self.assertIn("Skipping config symlink outside Vela", run.stderr)
        self.assertIn("Skipping recursive config symlink", run.stderr)
        self.assert_clean()

    def test_invalid_config_import_rejected(self):
        """Reject invalid configuration-import values before allocating a session."""
        run = self.run_script("run-nested.sh", VELA_TEST_COPY_CONFIG="yes")
        self.assertEqual(run.returncode, 2)
        self.assertIn("VELA_TEST_COPY_CONFIG", run.stderr)
        self.assertFalse(self.output.exists())
        self.assert_clean()

    def test_helper_rejects_arbitrary_executables_and_flags(self):
        """Prevent the helper from executing arbitrary commands or shell paths."""
        for command in (["/bin/echo"], [str(self.build / "compositor" / "vela-compositor"),
                                        "-s", "/bin/sh"]):
            with self.subTest(command=command):
                result = subprocess.run(
                    ["python3", str(SCRIPTS / "private-session.py"), *command],
                    env=self.env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse(self.output.exists())
                self.assert_clean()

    def test_relative_tmpdir_uses_safe_fallback(self):
        """Ignore an invalid relative TMPDIR and still clean up safely."""
        run = self.run_script("run-headless.sh", TMPDIR="relative")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assert_isolated(json.loads(self.output.read_text()))
        self.assert_clean()

    def test_detached_stubborn_child_is_reaped_before_directory_cleanup(self):
        """Terminate an orphaned setsid client before deleting its home."""
        orphan_pid = self.root / "orphan.pid"
        try:
            run = self.run_script("run-nested.sh", VELA_TEST_ORPHAN="1",
                                  VELA_TEST_ORPHAN_PID=str(orphan_pid))
            self.assertEqual(run.returncode, 0, run.stderr)
            pid = int(orphan_pid.read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            self.assert_clean()
        finally:
            if orphan_pid.exists():
                try:
                    os.kill(int(orphan_pid.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def test_uncooperative_compositor_is_killed_after_grace_period(self):
        """Escalate from SIGTERM to SIGKILL for an uncooperative compositor."""
        process = subprocess.Popen(
            ["sh", str(SCRIPTS / "run-headless.sh"), str(self.build)],
            env=dict(self.env, VELA_TEST_HOLD="1", VELA_TEST_IGNORE_TERM="1"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(200):
                if self.output.exists():
                    break
                self.assertIsNone(process.poll())
                time.sleep(0.02)
            else:
                self.fail("fake compositor never started")
            process.terminate()
            _, error = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 143, error.decode())
            self.assert_clean()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()

    def test_shell_path_is_passed_to_compositor(self):
        """Pass a validated sibling shell executable to the compositor."""
        shell = self.build / "shell" / "vela-shell"
        shell.parent.mkdir()
        shell.write_text("#!/bin/sh\nexit 0\n")
        shell.chmod(0o755)
        run = self.run_script("run-nested.sh")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(self.output.read_text())["args"], ["-s", str(shell)])
        self.assert_clean()

    def test_sigterm_is_forwarded_and_tempfiles_are_removed(self):
        """Forward SIGTERM and remove temporary files after shutdown."""
        marker = self.root / "terminated.txt"
        process = subprocess.Popen(["sh", str(SCRIPTS / "run-headless.sh"), str(self.build)],
                                   env=dict(self.env, VELA_TEST_HOLD="1", VELA_TEST_SIGNAL_OUTPUT=str(marker)),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(200):
                if self.output.exists():
                    break
                self.assertIsNone(process.poll())
                time.sleep(0.02)
            else:
                self.fail("fake compositor never started")
            process.send_signal(signal.SIGTERM)
            _, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 143, error.decode())
            self.assertEqual(marker.read_text(), "terminated")
            self.assert_clean()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()

    def test_pidfd_rejects_stale_process_identity(self):
        """Even if a PID is reused, the old start time cannot target it."""
        helper = self.load_helper()
        child = subprocess.Popen(
            [sys.executable, "-u", "-c", "import time; print('ready', flush=True); time.sleep(60)"],
            stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), "ready")
            fields = Path(f"/proc/{child.pid}/stat").read_text().rsplit(")", 1)[1].split()
            started = int(fields[19])
            helper._signal_descendant(child.pid, started + 1, signal.SIGKILL)
            self.assertIsNone(child.poll(), "stale identity signalled a living process")
            helper._signal_descendant(child.pid, started, signal.SIGKILL)
            self.assertEqual(child.wait(timeout=3), -signal.SIGKILL)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            child.stdout.close()

    def test_transient_empty_proc_snapshot_cannot_leave_a_live_child(self):
        """An empty procfs scan is not enough while waitpid sees our child."""
        helper = self.load_helper()
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        calls = 0
        real_descendants = helper._descendants

        def transient_gap():
            nonlocal calls
            calls += 1
            return {} if calls == 1 else real_descendants()

        try:
            with patch.object(helper, "_descendants", side_effect=transient_gap):
                self.assertTrue(helper._stop_all(child))
            self.assertGreaterEqual(calls, 2)
            self.assertIsNotNone(child.returncode)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()

    def test_stop_error_preserves_the_private_home(self):
        """A failed descendant shutdown must never delete its writable home."""
        helper = self.load_helper()
        mkdtemp = tempfile.mkdtemp
        handlers = {sig: signal.getsignal(sig) for sig in
                    (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)}
        try:
            with patch.object(helper.tempfile, "mkdtemp",
                              side_effect=lambda **kw: mkdtemp(dir=self.tmp, **kw)), \
                    patch.dict(os.environ, self.env, clear=True), \
                    patch.object(helper, "_stop_all", side_effect=OSError("injected stop failure")):
                with self.assertRaisesRegex(OSError, "injected stop failure"):
                    helper.main([str(self.build / "compositor" / "vela-compositor")])
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            # Direct helper.main() calls alter this unittest process's prctl
            # subreaper setting; leave no inherited supervision behind.
            ctypes.CDLL(None).prctl(helper.PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0)
        preserved = list(self.tmp.iterdir())
        self.assertEqual(len(preserved), 1)
        self.assertTrue((preserved[0] / "home").is_dir())
        self.assertEqual(self.sentinel.read_text(), "real session must not change")

    def test_signal_received_during_cleanup_updates_exit_code(self):
        """SIGTERM during descendant cleanup must return 143, not compositor 0."""
        helper = self.load_helper()
        mkdtemp = tempfile.mkdtemp
        handlers = {sig: signal.getsignal(sig) for sig in
                    (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)}

        def interrupt_cleanup(process):
            helper._requested_signal = signal.SIGTERM
            return not helper._reap(process)

        try:
            with patch.object(helper.tempfile, "mkdtemp",
                              side_effect=lambda **kw: mkdtemp(dir=self.tmp, **kw)), \
                    patch.dict(os.environ, self.env, clear=True), \
                    patch.object(helper, "_stop_all", side_effect=interrupt_cleanup):
                self.assertEqual(helper.main(
                    [str(self.build / "compositor" / "vela-compositor")]), 143)
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            # Direct helper.main() calls alter this unittest process's prctl
            # subreaper setting; leave no inherited supervision behind.
            ctypes.CDLL(None).prctl(helper.PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0)
        self.assert_clean()

    def test_concurrent_launchers_have_distinct_private_roots(self):
        """Parallel launchers must never share persistent private paths."""
        paths = [self.root / "a.json", self.root / "b.json"]
        markers = [self.root / "a.term", self.root / "b.term"]
        processes = []
        try:
            for path, marker in zip(paths, markers):
                processes.append(subprocess.Popen(
                    ["sh", str(SCRIPTS / "run-nested.sh"), str(self.build)],
                    env=dict(self.env, VELA_TEST_OUTPUT=str(path), VELA_TEST_HOLD="1",
                             VELA_TEST_SIGNAL_OUTPUT=str(marker)),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE))
            for _ in range(200):
                if all(p.exists() for p in paths):
                    break
                self.assertTrue(all(p.poll() is None for p in processes))
                time.sleep(0.02)
            else:
                self.fail("concurrent launchers never started")
            data = [json.loads(p.read_text()) for p in paths]
            self.assertNotEqual(data[0]["env"]["XDG_CACHE_HOME"], data[1]["env"]["XDG_CACHE_HOME"])
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                process.stdout.close()
                process.stderr.close()
        self.assertEqual([p.returncode for p in processes], [143, 143])
        self.assertTrue(all(marker.exists() for marker in markers))
        self.assert_clean()


if __name__ == "__main__":
    unittest.main()
