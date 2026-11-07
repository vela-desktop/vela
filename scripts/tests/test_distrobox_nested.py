# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mock-based CLI tests; do not require Distrobox, a GPU or host package installs."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "vela-distrobox.sh"


class DistroboxNestedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        binary = self.root / "bin"
        binary.mkdir()
        mock = binary / "distrobox"
        mock.write_text(
            """#!/bin/sh
printf '%s\\n' "$*" >>"$MOCK_LOG"
case "$1" in
  list)
    printf 'ID | NAME | STATUS | IMAGE\\n'
    if [ "$MOCK_HAS_BOX" = 1 ]; then
      printf 'test-id | %s | Up | quay.io/toolbx/arch-toolbox:latest\\n' "$MOCK_BOX_NAME"
    fi
    ;;
  enter)
    if [ "$MOCK_NOT_ARCH" = 1 ]; then
      case "$*" in *'/bin/sh -c'*) exit 1 ;; esac
    fi
    ;;
esac
exit 0
"""
        )
        mock.chmod(0o755)
        self.log = self.root / "log"
        self.env = dict(os.environ)
        self.env.update(
            HOME=str(self.root),
            XDG_DATA_HOME=str(self.root / "data"),
            XDG_CONFIG_HOME=str(self.root / "config"),
            PATH=str(binary) + os.pathsep + os.environ["PATH"],
            MOCK_LOG=str(self.log),
            MOCK_HAS_BOX="0",
            MOCK_BOX_NAME="vela-nested",
            MOCK_NOT_ARCH="0",
            WAYLAND_DISPLAY="wayland-0",
            XDG_RUNTIME_DIR=str(self.root / "run"),
        )
        (self.root / "run").mkdir()
        for variable in ("VELA_DISTROBOX_NAME", "VELA_DISTROBOX_IMAGE"):
            self.env.pop(variable, None)

    def call(self, *args, **extra):
        env = self.env.copy()
        env.update(extra)
        return subprocess.run(
            ["sh", str(SCRIPT), *args],
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def calls(self):
        return self.log.read_text() if self.log.exists() else ""

    def test_help_does_not_need_distrobox_or_wayland(self):
        result = self.call("help", WAYLAND_DISPLAY="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("setup [--nvidia]", result.stdout)
        self.assertEqual(self.calls(), "")

    def test_setup_creates_container_and_host_launcher(self):
        result = self.call("setup")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertIn("create --yes --name vela-nested", calls)
        self.assertIn("--home " + str(self.root / "data/vela-distrobox/home"), calls)
        self.assertIn("enter --name vela-nested -- /bin/bash -lc", calls)
        self.assertIn("vela-update", calls)
        self.assertIn("--channel beta", calls)
        wrapper = self.root / ".local/bin/vela-distrobox"
        desktop = self.root / "data/applications/org.vela.Nested.Distrobox.desktop"
        self.assertTrue(os.access(wrapper, os.X_OK))
        self.assertIn(f'Exec="{wrapper}" run', desktop.read_text())
        self.assertEqual(
            (self.root / "config/vela-distrobox/container-name").read_text(),
            "vela-nested\n",
        )

    def test_nvidia_option_is_used_only_for_new_container(self):
        result = self.call("setup", "--nvidia")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--nvidia", self.calls())

    def test_existing_arch_container_is_reused(self):
        result = self.call("setup", MOCK_HAS_BOX="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("create --yes", self.calls())
        self.assertIn("enter --name vela-nested -- /bin/sh -c", self.calls())

    def test_nonarch_container_is_rejected(self):
        result = self.call("setup", MOCK_HAS_BOX="1", MOCK_NOT_ARCH="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not Arch", result.stderr)
        self.assertFalse((self.root / ".local/bin/vela-distrobox").exists())

    def test_run_requires_existing_wayland_session(self):
        result = self.call("run", WAYLAND_DISPLAY="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Wayland host session", result.stderr)
        self.assertEqual(self.calls(), "")

    def test_run_forces_nested_backend_and_private_dbus(self):
        result = self.call("run", MOCK_HAS_BOX="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertIn("enter --no-tty --name vela-nested", calls)
        self.assertIn("WLR_BACKENDS=wayland", calls)
        self.assertIn("VELA_POLKIT_AGENT=0", calls)
        self.assertIn("VELA_BLUETOOTH=0", calls)
        self.assertIn("VELA_UPDATE_DAEMON=0", calls)
        self.assertIn("/usr/bin/dbus-run-session -- /usr/bin/vela-compositor -s /usr/bin/vela-shell", calls)

    def test_custom_name_persists_for_launcher(self):
        result = self.call(
            "setup",
            MOCK_HAS_BOX="1",
            MOCK_BOX_NAME="vela-alt",
            VELA_DISTROBOX_NAME="vela-alt",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.log.unlink()
        result = self.call("run", MOCK_HAS_BOX="1", MOCK_BOX_NAME="vela-alt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("enter --no-tty --name vela-alt", self.calls())

    def test_update_runs_only_in_container(self):
        result = self.call("update", MOCK_HAS_BOX="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("enter --name vela-nested -- vela-update --channel beta", self.calls())

    def test_remove_launcher_does_not_remove_container(self):
        self.assertEqual(self.call("setup").returncode, 0)
        self.log.unlink()
        result = self.call("remove-launcher")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), "")
        self.assertFalse((self.root / ".local/bin/vela-distrobox").exists())


if __name__ == "__main__":
    unittest.main()
