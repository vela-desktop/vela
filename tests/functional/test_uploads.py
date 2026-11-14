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
import subprocess
import tempfile
import time
import unittest

from harness import Image, Session, tool

LIMITS = re.compile(r"client buffers up to (\d+)x(\d+) in shared memory, (\d+)x(\d+) as linear dmabufs")
VALIDATION = {"VELA_VULKAN_VALIDATION": "1"}
WINDOW = (512, 256)


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
            (shm_w, _), _ = self.limits(vela)
            width, height = min(shm_w, 16384), 8192  # 512 MiB at 16384: four budgets
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
            (shm_w, _), _ = self.limits(vela)
            width = min(shm_w, 8192)
            logs, windows = [], []
            for i in range(3):
                window, log = self.open(vela, "--app-id", f"big-{i}", width, 4096 + i * 64, *WINDOW)
                windows.append(window)
                logs.append(log)
            for log in logs:
                self.wait_client(log, "released 0")
            upload = vela.state()["upload"]
            print(f"three {width}x4096 buffers: {upload}", flush=True)
            self.assertLessEqual(upload["chunks_peak"], upload["chunks_max"])
            time.sleep(0.2)
            image = vela.pixels()
            for window in windows:
                self.assertEqual(self.versions_shown(image, self.window_box(vela, window)), 0)

    def test_deferred_update_is_never_shown_half_done(self):
        with Session(size="1920x1080@60", env=VALIDATION) as vela:
            (shm_w, _), _ = self.limits(vela)
            width, height = min(shm_w, 16384), 8192
            window, log = self.open(vela, "--next-after", 1500, width, height, *WINDOW)
            self.wait_client(log, "released 0")
            box = self.window_box(vela, window)
            with tempfile.TemporaryDirectory(dir=vela.directory) as directory:
                recorder = subprocess.Popen([tool("vela-screen-record"), directory, *map(str, box)],
                                            env=vela.client_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True)
                vela.clients.append(recorder)
                self.assertIn("recording", recorder.stdout.readline())
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
            self.assertEqual(shown[-1], 1, "the new buffer must end up shown whole")
            self.assertEqual(shown, sorted(shown), "an old buffer came back after the new one")
            self.assertGreaterEqual(vela.state()["upload"]["deferred"], 2)

    def test_competing_uploads_do_not_starve_another(self):
        with Session(size="1920x1080@60") as vela:
            (shm_w, _), _ = self.limits(vela)
            width = min(shm_w, 8192)
            # 144 MiB every frame: above the 128 MiB budget each time.
            greedy, greedy_log = self.open(vela, "--every-frame", "--app-id", "greedy", width, 4608, *WINDOW)
            self.wait_client(greedy_log, "committed 3")
            # 136 MiB once: two frames of pieces, if it gets them.
            other_log = os.path.join(vela.directory, "other.log")
            with open(other_log, "w") as output:
                other = subprocess.Popen([tool("vela-bigbuffer"), "--app-id", "other", str(width), "4352",
                                          *map(str, WINDOW)], env=vela.client_env, stdout=output,
                                         stderr=subprocess.STDOUT)
            vela.clients.append(other)
            self.wait_client(other_log, "committed 0")
            frames_before = vela.state().output["frames"]
            self.wait_client(other_log, "released 0")
            frames = vela.state().output["frames"] - frames_before
            upload = vela.state()["upload"]
            print(f"the other upload completed within {frames} frames under a client committing above the "
                  f"budget every frame: {upload}", flush=True)
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
