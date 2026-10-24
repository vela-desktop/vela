# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Subprocess diagnostics use synthetic results, never real host operations."""

import subprocess
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from vela_bench.host import Host, HostOperationError


class CommandDiagnostics(unittest.TestCase):
    def test_session_dispatch_really_executes_and_keeps_the_payload_exit_code(self):
        if os.getuid() == 0:
            self.skipTest("the actual exec fixture must run unprivileged")
        # NSS and the selected desktop entry are fixture boundaries. execve,
        # UID checks, environment construction, cwd, pipes and exit are real.
        # The replacement payload only prints metadata and exits; it never
        # contacts the actual runtime/user bus or starts a desktop.
        child = '''import os, sys
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, sys.argv[1])
from vela_bench.pilot import launcher_main
user = SimpleNamespace(pw_uid=os.getuid(), pw_name="vela-benchmark", pw_dir=sys.argv[2], pw_shell="/bin/sh")
payload = "import json, os, sys; print(json.dumps({'uid':os.getuid(),'cwd':os.getcwd(),'home':os.environ['HOME'],'variables':sorted(os.environ)})); sys.exit(37)"
entry = {"executables":[{"path":sys.executable}], "argv":[sys.executable,"-I","-c",payload]}
with patch("vela_bench.pilot.pwd.getpwnam", return_value=user), patch("vela_bench.pilot.resolve_targets", return_value={"vela":entry}):
    raise SystemExit(launcher_main(["_session","vela"]))
'''
        with tempfile.TemporaryDirectory(prefix="vela-session-dispatch-") as home:
            result = subprocess.run([sys.executable, "-I", "-c", child, str(Path(__file__).resolve().parents[1]), home],
                env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "XDG_SESSION_ID": "fixture",
                     "XDG_VTNR": "8", "XDG_SEAT": "seat0", "WAYLAND_DISPLAY": "foreign-fixture"},
                capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 37, result.stderr)
            observed = json.loads(result.stdout)
            self.assertEqual(observed["home"], home)
            self.assertEqual(observed["cwd"], home)
            self.assertEqual(observed["uid"], os.getuid())
            self.assertNotIn("WAYLAND_DISPLAY", observed["variables"])
            for stage in ('"stage": "entry"', "environment validated", "exec "):
                self.assertIn(stage, result.stderr)

    def test_stdout_only_failure_keeps_exit_code_and_message(self):
        command = ["/usr/bin/ddcutil", "--bus", "6", "getvcp", "10"]
        result = subprocess.CompletedProcess(command, 1, "Display not found on bus 6\n", "")
        with patch("vela_bench.host.subprocess.run", return_value=result), self.assertRaises(HostOperationError) as caught:
            Host().run(command)
        self.assertIn("exit 1", str(caught.exception))
        self.assertIn("Display not found on bus 6", str(caught.exception))
        self.assertEqual(caught.exception.operation["argv"], command)
        self.assertEqual(caught.exception.operation["stdout"], result.stdout)

    def test_timeout_keeps_partial_byte_output_and_deadline(self):
        error = subprocess.TimeoutExpired(["test-fixture"], 3, output=b"partial output\n", stderr=b"partial error\n")
        with patch("vela_bench.host.subprocess.run", side_effect=error), self.assertRaises(HostOperationError) as caught:
            Host().run(["test-fixture"], timeout=3)
        self.assertIn("timeout after 3s", str(caught.exception))
        self.assertEqual(caught.exception.operation["stdout"], "partial output\n")
        self.assertEqual(caught.exception.operation["stderr"], "partial error\n")

    def test_unchecked_failure_remains_available_to_the_callers_parser(self):
        result = subprocess.CompletedProcess(["test-fixture"], 1, "nonzero output", "")
        with patch("vela_bench.host.subprocess.run", return_value=result):
            self.assertIs(Host().run(["test-fixture"], check=False), result)


if __name__ == "__main__":
    unittest.main()
