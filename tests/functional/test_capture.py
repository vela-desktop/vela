# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Deferred SHM capture pixels, cancellation and presentation at 180 Hz.

Output and window sources retain buffers until GPU completion and the final
CPU row copy. The original animation workload must still satisfy the same
missed-vblank budget, with no skipped or silently dropped captures.
"""

import os
import json
import select
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
            state = vela.state()
            missed = state.output["missed"] - before
            readback = state["readback"]
            print(f"capture: 2560x1440@180 captures={captures} missed_vblanks={missed} "
                  f"completed={readback['completed']} cancelled={readback['cancelled']} "
                  f"failed={readback['failed']} rejected={readback['rejected']} "
                  f"pending={readback['pending']}", flush=True)
            self.assertEqual(readback["completed"], captures, "every capture must use deferred completion")
            self.assertEqual(readback["pending"], 0)
            self.assertEqual(readback["failed"], 0)
            self.assertEqual(readback["cancelled"], 0)
            self.assertEqual(readback["rejected"], 0)
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
                              timeout=5, interval=0.001, what="an actual deferred capture")
        self.assertLessEqual(state["readback"]["pending"], 4)
        self.assertLessEqual(state["readback"]["bytes"], 128 * 1024 * 1024)
        return state["readback"]

    def drained(self, vela):
        return vela.wait_for(lambda s: s["readback"]["pending"] == 0,
                             what="capture resources released")["readback"]

    def cancel(self, process):
        _, errors = process.communicate("cancel\n", timeout=5)
        self.assertEqual(process.returncode, 0, errors)

    def add_wallpaper(self, vela):
        process = subprocess.Popen([tool("vela-panel"), "wallpaper", "--color", "123456"], env=vela.client_env)
        vela.clients.append(process)

    def wallpaper(self, vela):
        self.add_wallpaper(vela)
        vela.wait_for(lambda _: vela.pixels().at(100, 100) == (0x12, 0x34, 0x56),
                      read_state=False, what="the solid wallpaper's first captured frame")
        return vela.state()["readback"]["completed"]

    def test_concurrent_full_pixels_padding_and_opaque_alpha(self):
        with Session(size="1367x769@180") as vela:
            vela.input("move", 0, 0)
            before = self.wallpaper(vela)
            # Independent Wayland connections exercise per-client source
            # lifetime as well as four simultaneous readback jobs.
            processes = [self.probe(vela, "pixels", 2) for _ in range(2)]
            for process in processes:
                _, errors = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(self.drained(vela)["completed"] - before, 4)

    def test_repeated_session_does_not_lose_damage(self):
        with Session(size="1367x769@180") as vela:
            vela.input("move", 0, 0)
            before = self.wallpaper(vela)
            process = self.probe(vela, "repeat", 4)
            for round_number in (1, 2):
                self.assertTrue(select.select([process.stdout], [], [], 5)[0], "capture round timed out")
                self.assertEqual(process.stdout.readline().strip(), f"round {round_number}")
                # Headless hardware-cursor motion is outside the captured
                # output image. Commit another real wallpaper buffer instead;
                # the opaque color oracle remains valid throughout the update.
                self.add_wallpaper(vela)
            _, errors = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(self.drained(vela)["completed"] - before, 12)

    def test_stream_latency_and_idle_metrics(self):
        # No sleeps between capture requests, no PNG encoding and no fresh
        # sessions per frame. Compare untouched new SHM pages with buffer reuse.
        for size, clients in (("2560x1440@180", 1), ("3840x2160@180", 1), ("2560x1440@180", 4)):
            with self.subTest(size=size, clients=clients), Session(size=size) as vela:
                window = vela.open_window(400, 300, decorated=True)
                for mode in ("bench-fresh", "bench-reuse", "idle"):
                    if mode == "idle":
                        vela.wait_still(window)
                    before = vela.state()
                    process = self.probe(vela, mode, clients)
                    if mode != "idle":
                        end = time.monotonic() + 3.2
                        maximize = True
                        while time.monotonic() < end:
                            vela.command(f"window {window} {'maximize' if maximize else 'restore'}")
                            maximize = not maximize
                            time.sleep(0.2)
                    output, errors = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 0, errors)
                    metrics = json.loads(output)
                    after = vela.state()
                    metrics.update(size=size, missed=after.output["missed"] - before.output["missed"],
                                   output_frames=after.output["frames"] - before.output["frames"])
                    print("capture-stream: " + json.dumps(metrics, sort_keys=True), flush=True)
                    self.assertGreater(metrics["frames"], 0)
                    if mode == "idle":
                        self.assertEqual(metrics["frames"], clients,
                                         "persistent capture must wait for source damage on an idle desktop")
                        self.assertLessEqual(metrics["output_frames"], 2)
                    elif "Vulkan readback staging: uncached fallback" not in vela.log_text():
                        # Explicit floors beyond the old 512 KiB/ms scheduler's
                        # theoretical ceiling (~35.7 / ~16.1 frames/s). Buffer
                        # reuse must sustain a typical 60/30 fps screen stream.
                        is_4k = size.startswith("3840")
                        minimum_fps = (30 if is_4k else 60) if mode == "bench-reuse" else (20 if is_4k else 40)
                        self.assertGreaterEqual(metrics["fps"], minimum_fps)
                        self.assertGreaterEqual(min(metrics["client_frames"]) / metrics["seconds"],
                                                0.8 * minimum_fps / clients, "one capture client starved")
                        self.assertLess(metrics["p95_ms"], 150 if clients > 1 else 100)
                        self.assertLess(metrics["missed"], metrics["frames"],
                                        "sustained capture exceeded the presentation budget")
                    self.assertEqual(after["readback"]["failed"], before["readback"]["failed"])
                    self.assertEqual(after["readback"]["rejected"], before["readback"]["rejected"])

    def test_queue_limit_reports_failure_and_recovers(self):
        with Session(size="3840x2160@180") as vela:
            before = vela.state()["readback"]
            process = self.probe(vela, "overflow", 8)
            _, errors = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, errors)
            after = self.drained(vela)
            self.assertEqual(after["completed"] - before["completed"], 4)
            # This counter increments only at capacity rejection, so unrelated
            # allocation, submission or timeout failures cannot satisfy it.
            self.assertEqual(after["rejected"] - before["rejected"], 4)
            self.assertEqual(after["failed"], before["failed"])
            self.assertEqual(after["cancelled"], before["cancelled"])
            vela.screenshot(os.path.join(vela.directory, "after-limit.png"), region=(0, 0, 8, 8))

    def test_client_disconnect_releases_reserved_staging(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela, count=4)
            before = self.pending(vela)
            process.kill()
            process.communicate(timeout=5)
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.assertEqual(after["bytes"], 0)
            vela.screenshot(os.path.join(vela.directory, "after-cancel.png"), region=(0, 0, 8, 8))

    def test_destroy_frame_cancels_before_connection_closes(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela, count=4)
            before = self.pending(vela)
            self.cancel(process)
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.assertEqual(after["bytes"], 0)

    def test_resize_cancels_old_buffer_constraints(self):
        with Session(size="3840x2160@180") as vela:
            process = self.probe(vela, count=4)
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
            process = self.probe(vela, count=4, output=1)
            before = self.pending(vela)
            vela.command("test-output remove HEADLESS-2")
            after = self.drained(vela)
            self.assertGreater(after["cancelled"], before["cancelled"])
            self.cancel(process)
            self.assertEqual(len(vela.state()["outputs"]), 1)

    def test_lock_rejects_readback_of_previous_desktop(self):
        with Session(size="3840x2160@180", lock_hold_ms=2000) as vela:
            process = self.probe(vela, count=4)
            before = self.pending(vela)
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"], what="session lock")
            after = self.drained(vela)
            self.assertGreater(after["failed"], before["failed"])
            self.cancel(process)

    def test_shutdown_with_pending_readback(self):
        with Session(size="3840x2160@180", env={"VELA_VULKAN_VALIDATION": "1"}) as vela:
            process = self.probe(vela, count=4)
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
