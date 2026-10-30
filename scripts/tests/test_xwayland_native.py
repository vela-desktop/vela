# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native X11 game launcher: argument boundaries, DISPLAY and RandR primary."""
import os
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "packaging/vela-xwayland-native"


class NativeX11Launcher(unittest.TestCase):
    def test_no_command(self):
        result = subprocess.run(["sh", str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)

    def test_missing_satellite(self):
        with tempfile.TemporaryDirectory() as folder:
            env = dict(os.environ, PATH=folder, WAYLAND_DISPLAY="wayland-test", XDG_RUNTIME_DIR=folder)
            result = subprocess.run(["/bin/sh", str(SCRIPT), "true"], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 127)
            self.assertIn("install xwayland-satellite", result.stderr)

    def test_native_display_and_primary(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            binpath = root / "bin"
            binpath.mkdir()
            runtime = root / "runtime"
            runtime.mkdir()
            config = root / "config" / "Vela"
            config.mkdir(parents=True)
            (config / "vela-shell.conf").write_text("[display]\nprimaryOutput=DP-3\n")
            log = root / "xrandr.log"
            fake = {
                "xwayland-satellite": "#!/bin/sh\ntrap 'exit 0' TERM\nwhile :; do /bin/sleep 1; done\n",
                "xrandr": "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$VELA_TEST_LOG\"\nexit 0\n",
                "target": "#!/bin/sh\nprintf '%s\\n' \"$DISPLAY\" > \"$VELA_TEST_DISPLAY\"\nprintf '%s\\n' \"$1\" > \"$VELA_TEST_ARG\"\n",
            }
            for name, content in fake.items():
                path = binpath / name
                path.write_text(content)
                path.chmod(0o755)
            env = dict(os.environ, PATH=f"{binpath}:/usr/bin:/bin",
                       WAYLAND_DISPLAY="wayland-test", XDG_RUNTIME_DIR=str(runtime),
                       XDG_CONFIG_HOME=str(root / "config"),
                       VELA_TEST_LOG=str(log), VELA_TEST_DISPLAY=str(root / "display"),
                       VELA_TEST_ARG=str(root / "arg"))
            result = subprocess.run(["/bin/sh", str(SCRIPT), "target", "two words"], env=env,
                                    text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            display = (root / "display").read_text().strip()
            self.assertRegex(display, r"^:\d+$")
            self.assertEqual((root / "arg").read_text().strip(), "two words")
            self.assertIn("--output DP-3 --primary", log.read_text())
            self.assertFalse(list(runtime.glob("vela-native-x11-*.lock")))


if __name__ == "__main__":
    unittest.main()
