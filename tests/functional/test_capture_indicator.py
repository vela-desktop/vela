# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The screen sharing indicator: a sustained capture sends the shell "capture
on" and its end "capture off"; a single screenshot sends nothing, nor does
capture-indicator=no."""

import os
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

from harness import Session, tool


@unittest.skipUnless(shutil.which("wf-recorder"), "wf-recorder is not installed")
class CaptureIndicator(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def stop(self, recorder):
        recorder.send_signal(signal.SIGINT) # it finishes the file on SIGINT
        try:
            recorder.wait(timeout=10)
        except subprocess.TimeoutExpired:
            recorder.kill()
            recorder.wait()

    def record(self, vela):
        return subprocess.Popen(
            ["wf-recorder", "-f", os.path.join(self.dir, "out.mkv")],
            env=vela.client_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def test_on_while_recording_off_after(self):
        with Session() as vela:
            shell = vela.fake_shell()
            vela.open_window(300, 200)
            recorder = self.record(vela)
            try:
                shell.wait_for("capture on", timeout=8)
            finally:
                self.stop(recorder)
            shell.wait_for("capture off", timeout=8)

    def test_dnd_flag_follows_config(self):
        with Session() as vela:
            shell = vela.fake_shell()
            os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
            vela.open_window(300, 200)
            recorder = self.record(vela)
            try:
                shell.wait_for("capture on dnd", timeout=8)
                with open(vela.config_path(), "a") as config:
                    config.write("capture-dnd=no\n")
                vela.command("reload-config")
                deadline = time.time() + 8
                while time.time() < deadline and not [m for m in shell.received("capture") if m.strip() == "capture on"]:
                    time.sleep(0.2)
                self.assertTrue([m for m in shell.received("capture") if m.strip() == "capture on"])
            finally:
                self.stop(recorder)

    def test_one_screenshot_is_silent(self):
        with Session() as vela:
            shell = vela.fake_shell()
            subprocess.run([tool("vela-screen-dump"), os.path.join(self.dir, "a.png")], env=vela.client_env, check=True)
            time.sleep(2.5)
            self.assertFalse([m for m in shell.received("capture")])

    def test_turned_off_in_config(self):
        with Session() as vela:
            shell = vela.fake_shell()
            os.makedirs(os.path.dirname(vela.config_path()), exist_ok=True)
            with open(vela.config_path(), "a") as config:
                config.write("capture-indicator=no\n")
            vela.command("reload-config")
            recorder = self.record(vela)
            try:
                time.sleep(3)
            finally:
                self.stop(recorder)
            self.assertFalse([m for m in shell.received("capture")])


if __name__ == "__main__":
    unittest.main()
