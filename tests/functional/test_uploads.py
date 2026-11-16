# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Shared-memory uploads under the staging budget and the image-extent cap,
on a real GPU (docs/memory-allocation-policy.md, point 6 and "Tests").

compositor.upload-staging checks the same rules on the CPU, deterministically
and with every draw compared with the committed buffers; this test checks
them through a real session: a real driver's limits, real copies, the
frames a client sees and what reaches the screen.

The client is vela-bigbuffer: buffers of 16 horizontal bands, whose red says
the version and whose green says the band, scaled into a small window.
"""

import os
import re
import select
import signal
import subprocess
import tempfile
import time
import unittest

from harness import Image, Session, TIME_SCALE, tool

LIMITS = re.compile(r"client buffers up to (\d+)x(\d+) in shared memory, (\d+)x(\d+) as linear dmabufs")
VALIDATION = {"VELA_VULKAN_VALIDATION": "1"}
WINDOW = (512, 256)


def upload_extent(limit, desired_bytes, budget_bytes, require_deferred=False):
    """A valid 32-bit buffer, optionally larger than one actual upload budget."""
    max_width, max_height = limit
    if max_width <= 0 or max_height <= 0 or budget_bytes <= 0:
        raise AssertionError("invalid renderer extent or upload budget")
    capacity = max_width * max_height * 4
    if require_deferred and capacity <= budget_bytes:
        raise unittest.SkipTest("device cannot fit a buffer above its upload budget")
    requested = max(desired_bytes, budget_bytes + 4) if require_deferred else desired_bytes
    size = min(requested, capacity)
    width = min(max_width, 16384)
    height = (size + width * 4 - 1) // (width * 4)
    if height > max_height:
        height = max_height
        width = (size + height * 4 - 1) // (height * 4)
    if not (0 < width <= max_width and 0 < height <= max_height):
        raise AssertionError("selected buffer exceeds the device extent")
    if require_deferred and width * height * 4 <= budget_bytes:
        raise AssertionError("selected buffer does not exceed the upload budget")
    return width, height


def band_color(version, band):
    return (0x20 + 0x40 * (version % 4), 0x08 + 0x10 * band, 0x40)


def close(a, b, tolerance=3):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


class Uploads(unittest.TestCase):
    def limits(self, vela):
        """The device's limits for XRGB8888, as the compositor logs them."""
        match = LIMITS.search(vela.log_text())
        self.assertIsNotNone(match, "the compositor doesn't log its image limits")
        shm_w, shm_h, dmabuf_w, dmabuf_h = (int(value) for value in match.groups())
        return (shm_w, shm_h), (dmabuf_w, dmabuf_h)

    def size_for(self, vela, budgets, extra=0, require_deferred=False):
        limit, _ = self.limits(vela)
        budget = vela.state()["upload"]["budget_bytes"]
        return upload_extent(limit, int(budgets * budget) + extra, budget, require_deferred)

    def open(self, vela, *args, timeout=30):
        """vela-bigbuffer with these arguments: its window and the path of its log."""
        window = vela.open_window(command=[tool("vela-bigbuffer"), *map(str, args)], timeout=timeout)
        return window, os.path.join(vela.directory, f"client-{len(vela.clients)}.log")

    @staticmethod
    def client_log(path):
        with open(path, errors="replace") as log:
            return log.read()

    def wait_client(self, path, line, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if line in self.client_log(path):
                return
            time.sleep(0.02)
        raise AssertionError(f"the client never printed {line!r}:\n{self.client_log(path)[-2000:]}")

    def wait_recorder(self, recorder, timeout=10):
        """Bound readiness even if the recorder writes only a partial line."""
        deadline = time.monotonic() + timeout * TIME_SCALE
        output = bytearray()
        while b"recording\n" not in output:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([recorder.stdout], [], [], remaining)[0]:
                break
            chunk = os.read(recorder.stdout.fileno(), 4096)
            if not chunk:
                break
            output.extend(chunk)
            if len(output) > 65536:
                break
        else:
            return
        recorder.terminate()
        try:
            tail, _ = recorder.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            recorder.kill()
            tail, _ = recorder.communicate(timeout=5)
        output.extend(tail or b"")
        self.fail(f"recorder did not become ready within {timeout * TIME_SCALE:g} s "
                  f"(exit={recorder.returncode}):\n{output[-4000:].decode(errors='replace')}")

    def versions_shown(self, image, box):
        """The version every band of the window shows, or None for a mix."""
        x, y, width, height = box
        versions = set()
        for band in range(16):
            sample = image.at(x + width // 2, y + int((band + 0.5) * height / 16))
            version = next((v for v in range(4) if close(sample, band_color(v, band))), None)
            if version is None:
                return None
            versions.add(version)
        return versions.pop() if len(versions) == 1 else None

    def window_box(self, vela, window):
        w = vela.state().window(window)
        return (w["x"], w["y"], w["w"], w["h"])

    # ------------------------------------------------------- the budget --

    def test_oversized_buffer_is_copied_within_the_budget(self):
        with Session(size="1920x1080@60") as vela:
            width, height = self.size_for(vela, 4, require_deferred=True)
            window, log = self.open(vela, width, height, *WINDOW)
            self.wait_client(log, "released 0")
            state = vela.state()
            upload = state["upload"]
            print(f"{width}x{height} buffer: {upload}", flush=True)
            self.assertGreaterEqual(upload["deferred"], 1, "a buffer above the budget must be deferred")
            self.assertGreaterEqual(upload["completed"], 1)
            self.assertLessEqual(upload["chunks_peak"], upload["chunks_max"])
            time.sleep(0.2)
            self.assertEqual(self.versions_shown(vela.pixels(), self.window_box(vela, window)), 0,
                             "the whole buffer must be on screen")

    def test_several_surfaces_upload_at_once(self):
        with Session(size="1920x1080@60") as vela:
            logs, windows = [], []
            for i in range(3):
                width, height = self.size_for(vela, 1, extra=i * 2 * 1024 * 1024)
                window, log = self.open(vela, "--app-id", f"big-{i}", width, height, *WINDOW)
                windows.append(window)
                logs.append(log)
            for log in logs:
                self.wait_client(log, "released 0")
            upload = vela.state()["upload"]
            print(f"three buffers within the reported extent: {upload}", flush=True)
            self.assertLessEqual(upload["chunks_peak"], upload["chunks_max"])
            time.sleep(0.2)
            for window in windows:
                vela.command(f"window {window} activate")
                vela.wait_for(lambda s: s["focused"] == window, what="each upload window exposed")
                self.assertEqual(self.versions_shown(vela.pixels(), self.window_box(vela, window)), 0)

    def test_deferred_update_is_never_shown_half_done(self):
        with Session(size="1920x1080@60", env=VALIDATION) as vela:
            width, height = self.size_for(vela, 4, require_deferred=True)
            window, log = self.open(vela, "--next-on-signal", width, height, *WINDOW)
            client = vela.clients[-1]
            self.wait_client(log, "released 0")
            box = self.window_box(vela, window)
            with tempfile.TemporaryDirectory(dir=vela.directory) as directory:
                recorder = subprocess.Popen([tool("vela-screen-record"), directory, *map(str, box)],
                                            env=vela.client_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                vela.clients.append(recorder)
                self.wait_recorder(recorder)
                client.send_signal(signal.SIGUSR1)
                self.wait_client(log, "committed 1")
                self.wait_client(log, "released 1")
                time.sleep(0.3)
                recorder.terminate()
                recorder.communicate(timeout=10)
                with open(os.path.join(directory, "frames.txt")) as info:
                    lines = info.read().split("\n")
                record_w, record_h, pixel_format = lines[0].split()
                record_w, record_h = int(record_w), int(record_h)
                with open(os.path.join(directory, "frames.raw"), "rb") as raw:
                    data = raw.read()
            frame_bytes = record_w * record_h * 4
            frames = len(data) // frame_bytes
            shown = []
            for f in range(frames):
                rows = []
                for y in range(record_h):
                    start = f * frame_bytes + y * record_w * 4
                    row = data[start:start + record_w * 4]
                    if pixel_format == "bgr0":
                        rows.append(bytes(b for x in range(record_w) for b in (row[4 * x + 2], row[4 * x + 1],
                                                                                   row[4 * x])))
                    else:
                        rows.append(bytes(b for x in range(record_w) for b in row[4 * x:4 * x + 3]))
                version = self.versions_shown(Image(record_w, record_h, rows), (0, 0, record_w, record_h))
                self.assertIsNotNone(version, f"frame {f} of {frames} shows two buffers at once")
                shown.append(version)
            print(f"{frames} frames recorded, versions {shown}", flush=True)
            self.assertGreater(frames, 0)
            self.assertEqual(shown[0], 0, "the recording must include the old buffer before the update")
            self.assertEqual(shown[-1], 1, "the new buffer must end up shown whole")
            self.assertEqual(shown, sorted(shown), "an old buffer came back after the new one")
            self.assertGreaterEqual(vela.state()["upload"]["deferred"], 2)

    def test_competing_uploads_do_not_starve_another(self):
        with Session(size="1920x1080@60") as vela:
            width, height = self.size_for(vela, 9 / 8, require_deferred=True)
            # Two buffers, each larger than the reported upload budget,
            # committed together. Establish real contention even when a
            # slow CPU takes longer to paint than one upload takes.
            greedy, greedy_log = self.open(vela, "--burst", "--app-id", "greedy", width, height, *WINDOW)
            vela.wait_for(lambda s: s["upload"]["replaced"] > 0, timeout=20,
                          what="the greedy client's pending upload replaced")
            self.assertTrue(vela.query("test-upload-hold on")["ok"])
            try:
                vela.wait_for(lambda s: s.window(greedy)["upload"]["pending"], timeout=20,
                              what="the greedy upload held pending")
                width, height = self.size_for(vela, 17 / 16, require_deferred=True)
                other, other_log = self.open(vela, "--app-id", "other", width, height, *WINDOW)
                vela.wait_for(lambda s: s.window(other)["upload"]["pending"]
                              and s.window(other)["upload"]["overlapped"],
                              what="the other upload queued behind real pending work")
            finally:
                self.assertTrue(vela.query("test-upload-hold off")["ok"])
            self.wait_client(other_log, "released 0")
            state = vela.state()
            target = state.window(other)["upload"]
            frames = target["frames"]
            self.assertTrue(target["deferred"] and target["overlapped"])
            self.assertFalse(target["pending"])
            self.assertGreater(frames, 0)
            upload = state["upload"]
            self.assertEqual(upload["abandoned"], 0)
            print(f"the other upload completed within {frames} service frames under real overlapping work: "
                  f"{upload}", flush=True)
            self.assertGreater(upload["replaced"], 0, "the greedy client must have replaced its uploads")
            self.assertLessEqual(frames, 8)
            self.assertLessEqual(upload["chunks_peak"], upload["chunks_max"])

    # --------------------------------------------------- the extent cap --

    def check_extent(self, dmabuf):
        with Session(size="1280x720@60", env=VALIDATION) as vela:
            shm_limit, dmabuf_limit = self.limits(vela)
            limit = dmabuf_limit[0] if dmabuf else shm_limit[0]
            self.assertGreater(limit, 0)
            mode = ["--dmabuf"] if dmabuf else ["--next-after", "50"]
            # One pixel above the limit: refused before vkCreateImage, the
            # window shows nothing (it had nothing before), logged once.
            above, above_log = self.open(vela, *mode, limit + 1, 1, *WINDOW)
            vela.wait_for(lambda s: s["upload"]["rejected"] >= 1, what="the buffer refused")
            if not dmabuf:
                self.wait_client(above_log, "committed 1")  # a second refused commit
                vela.wait_for(lambda s: s["upload"]["rejected"] >= 2, what="the second buffer refused")
            time.sleep(0.2)
            text = vela.log_text()
            self.assertEqual(text.count("refused, the GPU takes at most"), 1, "logged once per client")
            self.assertIn(f"of {limit + 1}x1 refused, the GPU takes at most {limit}x", text)
            rejected = vela.state()["upload"]["rejected"]
            # At the limit: accepted.
            at, at_log = self.open(vela, *(["--dmabuf"] if dmabuf else []), limit, 1, *WINDOW)
            if not dmabuf:
                self.wait_client(at_log, "released 0")
            time.sleep(0.3)
            self.assertEqual(vela.state()["upload"]["rejected"], rejected)
            if not dmabuf:
                box = self.window_box(vela, at)
                image = vela.pixels()
                self.assertTrue(close(image.at(box[0] + box[2] // 2, box[1] + box[3] // 2), band_color(0, 0)))

    def test_shm_above_the_device_limit_is_refused(self):
        self.check_extent(dmabuf=False)

    def test_dmabuf_above_the_format_limit_is_refused(self):
        self.check_extent(dmabuf=True)


if __name__ == "__main__":
    unittest.main()
