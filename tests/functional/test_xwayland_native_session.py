# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Integration test: Satellite must report PHYSICAL modes, not Wayland logical ones.

A 2560x1440 headless output at scale 1.25 has logical size 2048x1152.
This test proves the session-wide DISPLAY / XRandR route actually exposes
2560x1440. It catches regressions that an external game-launcher test misses.
"""

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
import time
import unittest

from harness import Session, tool


class NativeXwaylandSession(unittest.TestCase):
    def setUp(self):
        if not shutil.which("xwayland-satellite"):
            self.skipTest("xwayland-satellite (>= 0.7) not available")
        if not shutil.which("xrandr"):
            self.skipTest("xrandr not available")
        self.tmp = tempfile.TemporaryDirectory(prefix="vela-native-display-")
        self.display_file = Path(self.tmp.name) / "session-display"
        self.vela = Session(
            scale=1.25, size="2560x1440@60",
            env={"VELA_XWAYLAND_BACKEND": "native",
                 "VELA_NATIVE_TEST_DISPLAY_FILE": str(self.display_file)},
            startup='printf %s "$DISPLAY" > "$VELA_NATIVE_TEST_DISPLAY_FILE"',
        )
        self.vela.start()

    def tearDown(self):
        if hasattr(self, "vela"):
            self.vela.stop()
        if hasattr(self, "tmp"):
            self.tmp.cleanup()

    def test_ewmh_maximize_bridge(self):
        # Satellite main currently handles _NET_WM_STATE_FULLSCREEN but not
        # the paired MAXIMIZED_HORZ/MAXIMIZED_VERT request used by Steam CSD.
        # Keep the reproduction in-tree, but do not fail all GPU tests until
        # upstream Satellite adds support (issues #269 and #460). Set this
        # opt-in on a patched Satellite build to verify the eventual fix.
        if os.environ.get("VELA_ASSERT_SATELLITE_MAXIMIZE") != "1":
            self.skipTest("Blocked upstream: Satellite does not forward EWMH maximize (#269)")
        if not os.path.exists(tool("vela-x11")):
            self.skipTest("vela-x11 test client requires XCB build dependency")
        display = self.vela.wait_for_log(
            "Native-resolution X11 Satellite listening on DISPLAY=",
            lambda line: line.split("DISPLAY=")[1].split()[0],
        )
        env = dict(self.vela.client_env, DISPLAY=display)
        xid_path = Path(self.tmp.name) / "xid"
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen(
            [tool("vela-x11"), "--id-file", str(xid_path), "640", "480"], env=env))
        self.vela.wait_for(
            lambda state: bool({w["id"] for w in state.windows} - before),
            timeout=15, what="Satellite toplevel mapped")
        window = next(iter({w["id"] for w in self.vela.state().windows} - before))
        self.vela.wait_still(window)
        deadline = time.monotonic() + 8
        while not xid_path.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertTrue(xid_path.exists(), "native X11 client did not publish its XID")
        subprocess.run(
            [tool("vela-x11"), "--state", xid_path.read_text().strip(), "maximize", "on"],
            env=env, check=True, timeout=8,
        )
        self.vela.wait_for(
            lambda state: state.window(window)["maximized"],
            timeout=8, what="paired EWMH maximize delivered through Satellite to XDG")

    def test_icccm_minimize_bridge(self):
        # Steam's minimize button can use XIconifyWindow: ICCCM
        # WM_CHANGE_STATE/IconicState. The host compositor receives XDG
        # minimize requests, not raw X11 messages; Satellite must bridge them.
        if os.environ.get("VELA_ASSERT_SATELLITE_MINIMIZE") != "1":
            self.skipTest("Blocked upstream: Satellite lacks ICCCM WM_CHANGE_STATE forwarding")
        if not os.path.exists(tool("vela-x11")):
            self.skipTest("vela-x11 test client requires XCB build dependency")
        display = self.vela.wait_for_log(
            "Native-resolution X11 Satellite listening on DISPLAY=",
            lambda line: line.split("DISPLAY=")[1].split()[0],
        )
        env = dict(self.vela.client_env, DISPLAY=display)
        xid_path = Path(self.tmp.name) / "minimize-xid"
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen(
            [tool("vela-x11"), "--id-file", str(xid_path), "640", "480"], env=env))
        self.vela.wait_for(
            lambda state: bool({w["id"] for w in state.windows} - before),
            timeout=15, what="minimize target mapped via Satellite")
        window = next(iter({w["id"] for w in self.vela.state().windows} - before))
        self.vela.wait_still(window)
        deadline = time.monotonic() + 8
        while not xid_path.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertTrue(xid_path.exists())
        subprocess.run(
            [tool("vela-x11"), "--iconify", xid_path.read_text().strip()],
            env=env, check=True, timeout=8,
        )
        self.vela.wait_for(
            lambda state: state.window(window)["minimized"],
            timeout=8, what="ICCCM IconicState delivered through Satellite to XDG")

    def test_satellite_xrandr_reports_native_mode(self):
        display = self.vela.wait_for_log(
            "Native-resolution X11 Satellite listening on DISPLAY=",
            lambda line: line.split("DISPLAY=")[1].split()[0],
        )
        self.assertRegex(display, r"^:\d+$")
        # This verifies the display apps inherit from Vela, not an explicitly
        # overridden per-game DISPLAY.
        deadline = time.monotonic() + 8
        while not self.display_file.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        self.assertTrue(self.display_file.exists(), "session startup did not run")
        self.assertEqual(self.display_file.read_text(), display)
        env = dict(self.vela.client_env, DISPLAY=display)
        deadline = time.monotonic() + 15
        output = ""
        while time.monotonic() < deadline:
            query = subprocess.run(["xrandr", "--query"], env=env,
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, timeout=5)
            if query.returncode == 0 and "connected" in query.stdout:
                output = query.stdout
                break
            time.sleep(0.15)
        self.assertTrue(output, "Satellite XRandR server did not become ready")
        self.assertRegex(output, r"(?m)^\s+2560x1440\s")
        # The logical size must not be advertised as the active physical mode.
        self.assertNotRegex(output, r"(?m)^\S+ connected 2048x1152")
