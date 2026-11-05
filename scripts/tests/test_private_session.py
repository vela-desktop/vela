# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""GPU-free regression tests for the manual Vela session launchers."""

import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]
COMPOSITOR = '''#!/usr/bin/env python3
import json, os, pathlib, signal, sys, time
path = pathlib.Path(os.environ["VELA_TEST_OUTPUT"])
keys = ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
        "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
        "WAYLAND_DISPLAY", "DISPLAY", "QT_WAYLAND_RECONNECT",
        "WLR_BACKENDS", "WLR_LIBINPUT_NO_DEVICES", "VELA_DEBUG_INPUT")
if os.environ.get("VELA_TEST_HOLD") == "1":
    def stop(*args):
        pathlib.Path(os.environ["VELA_TEST_SIGNAL_OUTPUT"]).write_text("terminated")
        sys.exit(0)
    signal.signal(signal.SIGTERM, stop)
result = {"env": {key: os.environ.get(key) for key in keys}, "args": sys.argv[1:]}
result["imported"] = {}
for name in ("vela", "Vela"):
    pref = pathlib.Path(os.environ["XDG_CONFIG_HOME"]) / name / "setting.txt"
    result["imported"][name] = pref.read_text() if pref.exists() else None
for name in ("XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "HOME"):
    (pathlib.Path(os.environ[name]) / "test-write").write_text("private")
path.write_text(json.dumps(result))
if os.environ.get("VELA_TEST_HOLD") == "1":
    while True:
        time.sleep(0.1)
sys.exit(int(os.environ.get("VELA_TEST_EXIT", "0")))
'''


class PrivateSessionTests(unittest.TestCase):
    def setUp(self):
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

    def run_script(self, name, **variables):
        return subprocess.run(["sh", str(SCRIPTS / name), str(self.build)],
                              env=dict(self.env, **variables), text=True,
                              capture_output=True, timeout=10)

    def assert_clean(self):
        self.assertEqual(list(self.tmp.iterdir()), [])
        self.assertEqual(self.sentinel.read_text(), "real session must not change")

    def assert_isolated(self, result):
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
        run = self.run_script("run-nested.sh")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.output.read_text())
        self.assert_isolated(result)
        self.assertEqual(result["env"]["WAYLAND_DISPLAY"], "wayland-host")
        self.assertEqual(result["env"]["DISPLAY"], ":18")
        self.assert_clean()

    def test_headless_drops_host_displays_and_enables_virtual_input(self):
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
        run = self.run_script("run-nested.sh", VELA_TEST_EXIT="37")
        self.assertEqual(run.returncode, 37, run.stderr)
        self.assert_clean()

    def test_opt_in_config_import_is_a_copy(self):
        for name in ("vela", "Vela"):
            folder = self.host / ".config" / name
            folder.mkdir()
            (folder / "setting.txt").symlink_to(self.sentinel)
        run = self.run_script("run-nested.sh", VELA_TEST_COPY_CONFIG="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(self.output.read_text())
        self.assertEqual(result["imported"], {"vela": "real session must not change",
                                              "Vela": "real session must not change"})
        self.assert_clean()

    def test_invalid_config_import_rejected(self):
        run = self.run_script("run-nested.sh", VELA_TEST_COPY_CONFIG="yes")
        self.assertEqual(run.returncode, 2)
        self.assertIn("VELA_TEST_COPY_CONFIG", run.stderr)
        self.assertFalse(self.output.exists())
        self.assert_clean()

    def test_shell_path_is_passed_to_compositor(self):
        shell = self.build / "shell" / "vela-shell"
        shell.parent.mkdir()
        shell.write_text("#!/bin/sh\nexit 0\n")
        shell.chmod(0o755)
        run = self.run_script("run-nested.sh")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(self.output.read_text())["args"], ["-s", str(shell)])
        self.assert_clean()

    def test_sigterm_is_forwarded_and_tempfiles_are_removed(self):
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

    def test_concurrent_launchers_have_distinct_private_roots(self):
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
