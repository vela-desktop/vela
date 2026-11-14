#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Session-owned Mesa selection must reach activation and be cleared on exit."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "session" / "vela-session-env"


class SessionEnvironment(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "calls.jsonl"
        fake = '''#!/usr/bin/python3
import json, os, pathlib, sys
with open(os.environ["VELA_ENV_TEST_LOG"], "a") as log:
    log.write(json.dumps({"tool": pathlib.Path(sys.argv[0]).name, "args": sys.argv[1:],
                          "mesa": os.environ.get("MESA_LOADER_DRIVER_OVERRIDE")}) + "\\n")
if "is-active" in sys.argv:
    sys.exit(0 if os.environ.get("VELA_ENV_TEST_ACTIVE") == "1" else 1)
if "cat" in sys.argv:
    sys.exit(1)
'''
        for name in ("systemctl", "dbus-update-activation-environment"):
            path = self.bin / name
            path.write_text(fake)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.defpath,
                        XDG_RUNTIME_DIR=str(self.root), WAYLAND_DISPLAY="wayland-test",
                        VELA_ENV_TEST_LOG=str(self.log), VELA_ENV_TEST_ACTIVE="0")
        self.env.pop("MESA_LOADER_DRIVER_OVERRIDE", None)

    def run_hook(self, action):
        subprocess.run(["/bin/sh", str(HOOK), action], env=self.env, check=True,
                       capture_output=True, text=True)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_venus_driver_reaches_activation_and_is_cleared(self):
        self.env["MESA_LOADER_DRIVER_OVERRIDE"] = "zink"
        self.run_hook("start")
        imports = [call for call in self.calls() if "import-environment" in call["args"]
                   or call["tool"] == "dbus-update-activation-environment"]
        self.assertEqual(len(imports), 2)
        for call in imports:
            self.assertIn("MESA_LOADER_DRIVER_OVERRIDE", call["args"])
            self.assertEqual(call["mesa"], "zink")
        self.log.write_text("")
        self.run_hook("stop")
        calls = self.calls()
        self.assertTrue(any("unset-environment" in call["args"]
                            and "MESA_LOADER_DRIVER_OVERRIDE" in call["args"] for call in calls))
        self.assertTrue(any(call["tool"] == "dbus-update-activation-environment"
                            and call["args"] == ["MESA_LOADER_DRIVER_OVERRIDE="] for call in calls))
        self.assertFalse((self.root / "vela-session-wayland-test").exists())

    def test_unspecified_driver_is_not_imported(self):
        self.run_hook("start")
        imports = [call for call in self.calls() if "import-environment" in call["args"]
                   or call["tool"] == "dbus-update-activation-environment"]
        self.assertEqual(len(imports), 2)
        for call in imports:
            self.assertNotIn("MESA_LOADER_DRIVER_OVERRIDE", call["args"])

    def test_another_desktop_is_left_alone(self):
        self.env.update(MESA_LOADER_DRIVER_OVERRIDE="zink", VELA_ENV_TEST_ACTIVE="1")
        self.run_hook("start")
        self.run_hook("stop")
        calls = self.calls()
        self.assertEqual(len(calls), 1)
        self.assertIn("is-active", calls[0]["args"])
        self.assertFalse((self.root / "vela-session-wayland-test").exists())


if __name__ == "__main__":
    unittest.main()
