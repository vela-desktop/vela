# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Capture correctness during animations, and speed when cached readback is available.

A capture into shared memory is copied by the GPU into a staging buffer, which
the compositor then reads with the CPU, inside the output's frame. From
uncached memory, the kind uploads use, reading a 1440p output
took longer than a frame at 180 Hz: about three missed vblanks per capture,
for as long as a screen recording or a screen share lasted. Readback staging
is preferably cached (render/renderer.c); supported GPUs read it in a fraction of a frame."""

import os
import signal
import subprocess
import threading
import time
import unittest

from harness import Session, tool


class Captures(unittest.TestCase):
    def test_captures_during_animations(self):
        with Session(size="2560x1440@180") as vela:
            window = vela.open_window(400, 300, decorated=True)
            vela.wait_still(window)
            path = os.path.join(vela.directory, "capture.png")
            stop = threading.Event()
            captures = 0
            capture_error = None

            def capture():
                # The compositor copies the whole output whatever the region:
                # a small one only keeps PNG encoding out of the measure.
                nonlocal captures, capture_error
                try:
                    while not stop.is_set():
                        vela.screenshot(path, region=(0, 0, 8, 8))
                        captures += 1
                        time.sleep(0.05)
                except Exception as error:
                    capture_error = error

            before = vela.state().output["missed"]
            thread = threading.Thread(target=capture)
            thread.start()
            try:
                for _ in range(4):
                    vela.command(f"window {window} maximize")
                    time.sleep(0.35)
                    vela.command(f"window {window} restore")
                    time.sleep(0.35)
            finally:
                stop.set()
                thread.join()
            if capture_error is not None:
                raise capture_error
            missed = vela.state().output["missed"] - before
            if os.environ.get("VELA_READBACK_DIAG") == "1":
                for line in vela.log_text().splitlines():
                    if "VELA_READBACK_DIAG" in line:
                        print(line, flush=True)
            # Uncached readback can be slow, so verify repeated successful
            # captures there without imposing the cached path's rate/latency bound.
            uncached = "Vulkan readback staging: uncached fallback" in vela.log_text()
            self.assertGreaterEqual(captures, 3 if uncached else 20, "too few captures to tell")
            if not uncached:
                self.assertLess(missed, captures, f"{missed} vblanks missed during {captures} captures")




class DeferredCapture(unittest.TestCase):
    """Pixels and resource ownership of actual deferred protocol requests."""

    def probe(self, vela, mode="hold", count=1, output=0):
        process = subprocess.Popen([tool("vela-capture-probe"), mode, str(count), str(output)],
                                   env=vela.client_env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        vela.clients.append(process)
        return process

    def pending(self, vela):
        state = vela.wait_for(lambda s: s["readback"]["pending"] > 0,
                              timeout=5, what="an actual deferred capture")
        self.assertLessEqual(state["readback"]["pending"], 4)
        self.assertLessEqual(state["readback"]["bytes"], 128 * 1024 * 1024)
        return state["readback"]

    def drained(self, vela):
        return vela.wait_for(lambda s: s["readback"]["pending"] == 0,
                             what="capture resources released")["readback"]

    def cancel(self, process):
        _, errors = process.communicate("cancel\n", timeout=5)
        self.assertEqual(process.returncode, 0, errors)

    def test_concurrent_full_pixels_padding_and_opaque_alpha(self):
        with Session(size="1367x769@180") as vela:
            vela.input("move", 0, 0)
            wallpaper = subprocess.Popen([tool("vela-panel"), "wallpaper", "--color", "ff00ff"],
                                         env=vela.client_env)
            vela.clients.append(wallpaper)
            time.sleep(0.6)
            process = self.probe(vela, "pixels", 4)
            _, errors = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(self.drained(vela)["completed"], 4)

    def test_repeated_session_does_not_lose_damage(self):
        with Session(size="1367x769@180") as vela:
            vela.input("move", 0, 0)
            wallpaper = subprocess.Popen([tool("vela-panel"), "wallpaper", "--color", "ff00ff"],
                                         env=vela.client_env)
            vela.clients.append(wallpaper)
            time.sleep(0.6)
            process = self.probe(vela, "repeat", 4)
            _, errors = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(self.drained(vela)["completed"], 12)

    def test_queue_limit_reports_failure_and_recovers(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela, "overflow", 8)
            _, errors = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(self.drained(vela)["completed"], 4)
            vela.screenshot(os.path.join(vela.directory, "after-limit.png"), region=(0, 0, 8, 8))

    def test_client_disconnect_releases_reserved_staging(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela, count=2)
            before = self.pending(vela)
            process.kill()
            process.communicate(timeout=5)
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.assertEqual(after["bytes"], 0)
            vela.screenshot(os.path.join(vela.directory, "after-cancel.png"), region=(0, 0, 8, 8))

    def test_destroy_frame_cancels_before_connection_closes(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela)
            before = self.pending(vela)
            self.cancel(process)
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.assertEqual(after["bytes"], 0)

    def test_resize_cancels_old_buffer_constraints(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela)
            before = self.pending(vela)
            vela.randr("--output", "HEADLESS-1", "--mode", "1367x769")
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.cancel(process)
            image = vela.pixels()
            self.assertEqual((image.width, image.height), (1367, 769))

    def test_unplug_cancels_source_with_pending_pixels(self):
        with Session() as vela:
            vela.command("test-output add 3840x2160@180")
            vela.wait_for(lambda s: len(s["outputs"]) == 2, what="capture output")
            process = self.probe(vela, output=1)
            before = self.pending(vela)
            vela.command("test-output remove HEADLESS-2")
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.cancel(process)
            self.assertEqual(len(vela.state()["outputs"]), 1)

    def test_lock_rejects_readback_of_previous_desktop(self):
        with Session(size="3840x2160@180", lock_hold_ms=2000) as vela:
            process = self.probe(vela)
            before = self.pending(vela)
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"], what="session lock")
            after = self.drained(vela)
            self.assertGreater(after["failed"], before["failed"])
            self.cancel(process)

    def test_shutdown_with_pending_readback(self):
        with Session(size="3840x2160@180", env={"VELA_VULKAN_VALIDATION": "1"}) as vela:
            process = self.probe(vela)
            self.pending(vela)
            os.kill(vela.process.pid, signal.SIGTERM)
            self.assertEqual(vela.process.wait(timeout=15), 0)
            process.communicate(timeout=5)
            log = vela.log_text()
            self.assertIn("Vulkan validation layers enabled", log)
            self.assertIn("Shutdown: Vulkan resource cleanup complete", log)
            self.assertNotIn("Validation Error", log)


if __name__ == "__main__":
    unittest.main()
