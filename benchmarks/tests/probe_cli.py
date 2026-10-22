#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""CPU-only native probe failures, including a server stalled during startup."""

from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

PROBE = str(Path(sys.argv.pop(1)).resolve())


class ProbeFailures(unittest.TestCase):
    def invoke(self, args, env=None, pass_fds=()):
        return subprocess.run([PROBE, *args], env=env, text=True, capture_output=True, timeout=4, pass_fds=pass_fds)

    def test_help_works_without_wayland(self):
        result = self.invoke(["--help"], env={})
        self.assertEqual(result.returncode, 0)
        self.assertIn("diagnostic", result.stdout)

    def test_invalid_options_fail_before_connecting(self):
        for args in (["--frames", "0"], ["--frames", "-1"], ["--frames", "10001"],
                     ["--frames", "2oops"], ["--delay-ms", "1001"], ["--timeout", "0"],
                     ["--unknown", "1"], ["--frames"], ["--inputs", "0"], ["--inputs", "1001"], ["--hold"]):
            with self.subTest(args=args):
                result = self.invoke(args, env={})
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("cannot connect", result.stderr)

    def test_missing_display_is_failure_not_zero_latency(self):
        with tempfile.TemporaryDirectory() as temp:
            result = self.invoke([], env={"XDG_RUNTIME_DIR": temp, "WAYLAND_DISPLAY": "missing"})
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('"complete":true', result.stdout)

    def test_deadline_bounds_unresponsive_startup(self):
        # WAYLAND_SOCKET is a connected, inherited fd. No listener, network
        # access or working compositor is needed to model a stalled server.
        client, server = socket.socketpair()
        with client, server:
            result = self.invoke(["--timeout", "1"], env={"WAYLAND_SOCKET": str(client.fileno())}, pass_fds=(client.fileno(),))
        self.assertEqual(result.returncode, 124)
        self.assertIn("deadline expired", result.stderr)
        self.assertNotIn('"complete":true', result.stdout)


unittest.main()
