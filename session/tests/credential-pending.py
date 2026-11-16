#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check real Linux process/listener metadata without any wallet or user bus."""

import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest


ENSURE = str(Path(sys.argv.pop(1)).resolve())
FIXTURE = Path(sys.argv.pop(1)).resolve()


class PendingMetadata(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="vela-credential-pending-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.proc = self.root / "proc"
        (self.proc / "self").mkdir(parents=True)
        (self.proc / "self/status").touch()
        (self.proc / "net").mkdir()
        (self.proc / "net/unix").symlink_to("/proc/net/unix")

    def pending(self, marker=None):
        env = dict(os.environ)
        env.pop("PAM_KWALLET5_LOGIN", None)
        if marker is not None:
            env["PAM_KWALLET5_LOGIN"] = str(marker)
        result = subprocess.run(["/bin/sh", "-c",
            '. "$1"; vela_credential_kwallet_pending "$2"',
            "probe", ENSURE, str(self.proc)], env=env, check=True,
            capture_output=True, text=True, timeout=3)
        return result.stdout.strip()

    def test_unregistered_process_and_deleted_executable(self):
        # A harmless signal-waiting fixture has the same kernel identity as
        # a provider before D-Bus registration, without coreutils wrappers.
        binary = self.root / "ksecretd"
        shutil.copyfile(FIXTURE, binary)
        binary.chmod(0o700)
        child = subprocess.Popen([str(binary)])
        def stop():
            if child.poll() is None:
                child.kill()
            child.wait(timeout=3)
        self.addCleanup(stop)
        entry = Path(f"/proc/{child.pid}")
        (self.proc / str(child.pid)).symlink_to(entry)
        deadline = time.monotonic() + 3
        while (entry / "comm").read_text().strip() != "ksecretd":
            self.assertIsNone(child.poll())
            self.assertLess(time.monotonic(), deadline, "fixture did not exec")
            time.sleep(0.005)
        self.assertEqual(self.pending(), "pending")
        binary.unlink()
        self.assertEqual(self.pending(), "pending")
        child.terminate()
        child.wait(timeout=3)
        self.assertEqual(self.pending(), "none")

    def test_pam_listener_before_exec_and_stale_marker_after_exit(self):
        marker = self.root / "pam.socket"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind(str(marker))
        listener.listen(1)
        # No daemon process exists yet, but PAM's handoff is already listening.
        self.assertEqual(self.pending(marker), "pending")
        listener.close()
        self.assertTrue(marker.exists())
        self.assertEqual(self.pending(marker), "none")


if __name__ == "__main__":
    unittest.main()
