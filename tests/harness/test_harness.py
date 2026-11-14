# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""CPU checks for test infrastructure: no compositor, Qt or GPU is required."""

import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import zlib

from harness import Image, PrivateBus, Session, State


class SessionCleanup(unittest.TestCase):
    def child(self, source):
        process = subprocess.Popen([sys.executable, "-c", source], start_new_session=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.reap, process)
        return process

    @staticmethod
    def reap(process):
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
        process.stderr.close()

    def test_already_crashed_compositor_fails_the_scenario(self):
        session = Session()
        session.process = self.child("raise SystemExit(7)")
        session.process.wait(timeout=3)
        with self.assertRaisesRegex(AssertionError, "stopped with 7"):
            session.stop()

    def test_already_clean_compositor_exit_is_accepted(self):
        session = Session()
        session.process = self.child("raise SystemExit(0)")
        session.process.wait(timeout=3)
        session.stop()

    def test_deliberate_failure_must_match_the_explicit_expected_exit(self):
        for actual in (1, 7):
            with self.subTest(actual=actual):
                session = Session(expected_exit=1)
                session.process = self.child(f"raise SystemExit({actual})")
                session.process.wait(timeout=3)
                if actual == 1:
                    session.stop()
                else:
                    with self.assertRaisesRegex(AssertionError, "expected 1"):
                        session.stop()

    def test_running_compositor_is_stopped_and_reaped(self):
        session = Session()
        session.process = self.child("import signal,time\n"
                                     "signal.signal(signal.SIGTERM, lambda *_: exit(0))\n"
                                     "print('ready', flush=True)\nwhile True: time.sleep(1)")
        self.assertEqual(session.process.stdout.readline().strip(), "ready")
        session.stop()
        self.assertEqual(session.process.returncode, 0)

    def test_uncooperative_client_is_killed_and_reaped(self):
        session = Session()
        client = self.child("import signal,time\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                            "print('ready', flush=True)\nwhile True: time.sleep(1)")
        self.assertEqual(client.stdout.readline().strip(), "ready")
        session.clients.append(client)
        session.stop()
        self.assertIsNotNone(client.returncode)

    def test_startup_uses_private_home_and_state_paths(self):
        session = Session()
        host_home = os.environ.get("HOME")
        with tempfile.TemporaryDirectory() as missing_build, patch("harness.BUILD", missing_build):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(FileNotFoundError):
                session.start()
        root = session.directory
        self.assertEqual(session.env["HOME"], os.path.join(root, "home"))
        self.assertEqual(session.env["XDG_STATE_HOME"], os.path.join(root, "state"))
        for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            self.assertEqual(os.path.dirname(session.env[key]), root)
        self.assertNotEqual(session.env["HOME"], host_home)
        self.assertEqual(os.environ.get("HOME"), host_home)
        self.assertFalse(os.path.exists(root))

    def test_compositor_launch_receives_private_environment(self):
        """Verify the compositor receives private XDG paths, not just Session.env."""
        session = Session()
        launched = {}

        def record_popen(*args, **kwargs):
            launched.update(kwargs)
            raise FileNotFoundError("compositor launch intercepted")

        with patch("harness.subprocess.Popen", side_effect=record_popen), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(FileNotFoundError):
            session.start()
        env = launched["env"]
        root = session.directory
        self.assertEqual(env["HOME"], os.path.join(root, "home"))
        self.assertEqual(env["XDG_STATE_HOME"], os.path.join(root, "state"))
        for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"):
            self.assertEqual(os.path.dirname(env[key]), root)
        self.assertFalse(os.path.exists(root))

    def test_home_creation_failure_does_not_mask_error_or_leak_directory(self):
        """An early mkdir error must not try to dump a non-existent log."""
        session = Session()
        real_mkdir = os.mkdir

        def deny_private_home(path, *args, **kwargs):
            if os.fspath(path).endswith("/home"):
                raise PermissionError("injected home failure")
            return real_mkdir(path, *args, **kwargs)

        with patch("harness.os.mkdir", side_effect=deny_private_home), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertRaisesRegex(PermissionError, "injected home failure"):
            session.start()
        self.assertIsNone(session.log_path)
        self.assertFalse(os.path.exists(session.directory))

    def test_failed_startup_removes_temporary_directory_and_closes_log(self):
        session = Session()
        with tempfile.TemporaryDirectory() as missing_build, patch("harness.BUILD", missing_build):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(FileNotFoundError):
                session.start()
        self.assertFalse(os.path.exists(session.directory))
        self.assertTrue(session.log.closed)


@unittest.skipUnless(shutil.which("dbus-daemon") and shutil.which("dbus-send"), "needs dbus-daemon and dbus-send")
class PrivateBusCleanup(unittest.TestCase):
    """What a private bus activates must neither reach the host session nor
    outlive the bus (it once kept the desktop portal of every test run
    connected to the developer's compositor)."""

    HOST = {"WAYLAND_DISPLAY": "wayland-0", "DISPLAY": ":0", "WAYLAND_SOCKET": "3",
            "XDG_RUNTIME_DIR": "/run/user/1000"}

    def activated_service(self, directory, ignore_term=False):
        """A .service file whose program reports its pid and environment."""
        report = os.path.join(directory, "report.json")
        program = os.path.join(directory, "service.py")
        with open(program, "w") as file:
            file.write("import json, os, signal, sys, time\n"
                       + ("signal.signal(signal.SIGTERM, signal.SIG_IGN)\n" if ignore_term else "")
                       + "with open(sys.argv[1] + '.part', 'w') as f:\n"
                       "    json.dump({'pid': os.getpid(), 'env': dict(os.environ)}, f)\n"
                       "os.rename(sys.argv[1] + '.part', sys.argv[1])\n"
                       "while True: time.sleep(1)\n")
        services = os.path.join(directory, "services")
        os.mkdir(services)
        with open(os.path.join(services, "org.vela.TestService.service"), "w") as file:
            file.write(f"[D-BUS Service]\nName=org.vela.TestService\nExec={sys.executable} {program} {report}\n")
        return services, report

    @staticmethod
    def start_service(bus, name):
        return subprocess.run(["dbus-send", f"--bus={bus.address}", "--print-reply", "--reply-timeout=500",
                               "--dest=org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus.StartServiceByName", f"string:{name}", "uint32:0"],
                              capture_output=True, text=True, timeout=10)

    @staticmethod
    def identity(pid):
        """Return a live process identity; a zombie has already released its fds."""
        try:
            with open(f"/proc/{pid}/stat") as stat:
                fields = stat.read().rsplit(")", 1)[1].split()
            if fields[0] in ("Z", "X", "x"):
                return None
            return pid, fields[19]  # starttime; distinguish a reused PID
        except FileNotFoundError:
            return None

    def check_activated_child_is_stopped(self, ignore_term):
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, self.HOST):
            services, report = self.activated_service(directory, ignore_term)
            bus = PrivateBus(services=services)
            bus.start()
            self.addCleanup(bus.stop)
            self.assertEqual(os.getsid(bus.process.pid), bus.process.pid)
            self.start_service(bus, "org.vela.TestService") # it never takes the name: a timeout
            deadline = time.monotonic() + 10
            while not os.path.exists(report) and time.monotonic() < deadline:
                time.sleep(0.02)
            with open(report) as file:
                child = json.load(file)
            identity = self.identity(child["pid"])
            self.assertIsNotNone(identity)
            for key in ("WAYLAND_DISPLAY", "DISPLAY", "WAYLAND_SOCKET"):
                self.assertNotIn(key, child["env"])
            runtime = child["env"]["XDG_RUNTIME_DIR"]
            self.assertEqual(os.path.dirname(runtime), bus.directory)
            private = bus.directory
            bus.stop()
            self.assertNotEqual(self.identity(child["pid"]), identity, "the activated service outlived its bus")
            self.assertFalse(os.path.exists(private))

    def test_activated_service_stops_with_the_bus(self):
        self.check_activated_child_is_stopped(ignore_term=False)

    def test_activated_service_ignoring_sigterm_is_killed(self):
        self.check_activated_child_is_stopped(ignore_term=True)

    def test_zombie_only_group_is_not_live(self):
        # A child that exits but hasn't been waited for is a zombie on Linux.
        # PID 1 can leave activated services in this state indefinitely in CI.
        from harness import _group_has_live_members, stop_process_group

        process = subprocess.Popen([sys.executable, "-c", "pass"], start_new_session=True)
        self.addCleanup(process.wait)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            with open(f"/proc/{process.pid}/stat") as stat:
                state = stat.read().rsplit(")", 1)[1].split()[0]
            if state == "Z":
                break
            time.sleep(0.02)
        else:
            self.fail("test child did not become a zombie")
        self.assertIsNone(self.identity(process.pid))
        self.assertFalse(_group_has_live_members(process.pid))
        started = time.monotonic()
        stop_process_group(process, timeout=0.25)
        self.assertLess(time.monotonic() - started, 1.5)

    def test_default_bus_activates_nothing(self):
        with PrivateBus() as bus:
            result = self.start_service(bus, "org.freedesktop.portal.Desktop")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("org.freedesktop.DBus.Error.ServiceUnknown", result.stderr)

    def test_failed_start_leaves_no_directory(self):
        bus = PrivateBus()
        with patch("harness.subprocess.Popen", side_effect=FileNotFoundError("dbus-daemon")), \
                self.assertRaises(FileNotFoundError):
            bus.start()
        self.assertIsNone(bus.directory)


class ClientStartupDiagnostics(unittest.TestCase):
    def session(self, directory):
        # No compositor needed: the harness should explain failures from a
        # subprocess that never creates a Wayland window.
        session = Session()
        session.directory = directory
        session.client_env = os.environ.copy()
        session.state = lambda: State({"windows": []})
        session.log_text = lambda: "test compositor log"
        return session

    def test_early_client_exit_reports_code_and_stderr(self):
        with tempfile.TemporaryDirectory() as directory:
            session = self.session(directory)
            try:
                with self.assertRaises(AssertionError) as raised:
                    session.open_window(
                        command=[sys.executable, "-u", "-c",
                                 "import sys; print('gbm_bo_create failed', file=sys.stderr); sys.exit(7)"],
                        timeout=2)
                self.assertIn("exited with status 7", str(raised.exception))
                self.assertIn("gbm_bo_create failed", str(raised.exception))
            finally:
                session.stop()

    def test_unmapped_live_client_timeout_includes_progress_output(self):
        with tempfile.TemporaryDirectory() as directory:
            session = self.session(directory)
            try:
                with self.assertRaises(AssertionError) as raised:
                    session.open_window(
                        command=[sys.executable, "-u", "-c",
                                 "import sys,time; print('calibrating compute workload', "
                                 "file=sys.stderr); time.sleep(5)"],
                        timeout=0.5)
                self.assertIn("after 0.5 s", str(raised.exception))
                self.assertIn("calibrating compute workload", str(raised.exception))
            finally:
                session.stop()


class ImageChecks(unittest.TestCase):
    @staticmethod
    def png(raw, width=2, height=6):
        def chunk(kind, data):
            return (struct.pack(">I", len(data)) + kind + data
                    + struct.pack(">I", zlib.crc32(kind + data)))
        header = struct.pack(">II5B", width, height, 8, 2, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

    def read(self, data):
        with tempfile.NamedTemporaryFile() as file:
            file.write(data)
            file.flush()
            return Image.read(file.name)

    def test_all_png_filters_against_known_pixels(self):
        # Fixed encoded scanlines, independently specified. The Paeth rows
        # exercise above, left, upper-left, tie breaking and byte wraparound.
        raw = bytes((0, 10, 20, 30, 40, 50, 60,
                     1, 15, 25, 35, 30, 30, 30,
                     2, 5, 5, 5, 5, 5, 5,
                     3, 15, 20, 25, 18, 18, 18,
                     4, 5, 5, 5, 221, 5, 216,
                     4, 20, 226, 40, 30, 20, 196))
        image = self.read(self.png(raw))
        self.assertEqual((image.width, image.height), (2, 6))
        self.assertEqual(image.rows, [bytes(row) for row in
                         ((10, 20, 30, 40, 50, 60), (15, 25, 35, 45, 55, 65),
                          (20, 30, 40, 50, 60, 70), (25, 35, 45, 55, 65, 75),
                          (30, 40, 50, 20, 70, 35), (50, 10, 90, 80, 60, 30))])

    def test_invalid_png_filter_and_scanline_length_are_rejected(self):
        for raw in (bytes((5, 1, 2, 3)), bytes((0, 1, 2))):
            with self.subTest(raw=raw), self.assertRaises(AssertionError):
                self.read(self.png(raw, width=1, height=1))

    def test_changed_bounds_include_all_rgb_channels_and_exclude_bottom_rows(self):
        original = Image(3, 3, [bytes(9)] * 3)
        changed = Image(3, 3, [bytes((0, 0, 1, 0, 0, 0, 0, 0, 0)),
                               bytes((0, 0, 0, 0, 1, 0, 0, 0, 0)),
                               bytes((0, 0, 0, 0, 0, 0, 1, 0, 0))])
        self.assertIsNone(original.changed_bounds(original))
        self.assertEqual(changed.changed_bounds(original), (0, 0, 3, 3))
        self.assertEqual(changed.changed_bounds(original, height=2), (0, 0, 2, 2))
        self.assertIsNone(changed.changed_bounds(original, height=0))
        with self.assertRaises(AssertionError):
            changed.changed_bounds(Image(1, 1, [bytes(3)]))


if __name__ == "__main__":
    unittest.main()
