# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The frame path allocates nothing in steady state, on a real GPU.

docs/memory-allocation-policy.md, point 7 and "Frame path". The compositor
under test is vela-compositor-alloc-count: the same objects as
vela-compositor, linked with compositor/tests/alloc_count.c, which logs every
allocation made through Vela's helpers (file and line) to
VELA_TEST_ALLOC_LOG. The test reads that log over a window of frames.

The scene redraws every frame: a decorated window and vela-slowgpu, a dmabuf
client that commits a new buffer every frame with explicit sync. Then the
same with a capture client copying the output into shared memory on every
frame (a second pass and a readback each time).

Some allocations in that window belong to a client's request, not to the
frame: an object per commit or per captured frame, made and freed with the
request's own lifetime, as wlroots allocates its own objects for the same
requests. They are named below, found by their source line, and printed with
their counts; everything else must be zero. compositor.frame-path checks the
drawing itself without a GPU, deterministically and with two passes open at
once; this test checks that nothing else in a real frame allocates.
"""

import collections
import os
import subprocess
import time
import unittest

from harness import ROOT, Session, tool

# A client's requests, each with the source line that allocates for it.
PER_REQUEST = {
    "dmabuf import (one per committed buffer, and per readback of an output buffer)":
        ("compositor/src/render/texture.c", "struct vela_texture *texture = VELA_NEW(struct vela_texture);"),
    "commit held until its fences signal (one per commit)":
        ("compositor/src/scene/ready.c", "struct held *held = VELA_NEW(struct held);"),
}
PER_CAPTURE = {
    "captured output frame":
        ("compositor/src/scene/output_capture.c",
         "struct output_capture_frame *pending = VELA_NEW(struct output_capture_frame);"),
    "copy into a shared-memory frame":
        ("compositor/src/scene/capture.c", "struct pending_capture *pending = VELA_NEW(struct pending_capture);"),
    "readback":
        ("compositor/src/render/texture.c", "struct vela_readback *readback = VELA_NEW(struct vela_readback);"),
}
# The capture watch (who captures the screen, docs/screen-sharing.md)
# publishes its state as JSON, built with vela_buffer, when it changes.
CAPTURE_WATCH_FILES = ("compositor/src/capture_watch.c", "compositor/src/buffer.c")

WARM_UP_S = 1.0
WINDOW_S = 1.5


def site(path, text):
    """The site path:line of `text`, which must appear once in `path`."""
    with open(os.path.join(ROOT, path)) as source:
        lines = [number for number, line in enumerate(source, 1) if text in line]
    if len(lines) != 1:
        raise AssertionError(f"{text!r} found {len(lines)} times in {path}: update this test")
    return f"{path}:{lines[0]}"


class FramePath(unittest.TestCase):
    def setUp(self):
        self.log = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), f"vela-alloc-{os.getpid()}.log")
        if os.path.exists(self.log):
            os.unlink(self.log)

    def tearDown(self):
        if os.path.exists(self.log):
            os.unlink(self.log)

    def window(self, vela):
        """Allocations logged over WINDOW_S of frames, by site, with frame counts."""
        before = vela.state()
        start = os.path.getsize(self.log)
        time.sleep(WINDOW_S)
        end = os.path.getsize(self.log)
        after = vela.state()  # its own allocations come after `end`
        with open(self.log) as log:
            log.seek(start)
            text = log.read(end - start)
        sites = collections.Counter()
        for line in text.splitlines():
            kind, where, _ = line.split(" ")
            sites[os.path.relpath(where, ROOT) if os.path.isabs(where) else where] += 1
        frames = after.output["frames"] - before.output["frames"]
        readbacks = after["readback"]["completed"] - before["readback"]["completed"]
        return sites, frames, readbacks

    def check(self, sites, allowed, label, allowed_files=()):
        print(f"{label}: " + (", ".join(f"{count} at {where}" for where, count in sites.most_common())
                              or "no allocation"), flush=True)
        unexpected = {where: count for where, count in sites.items()
                      if where not in allowed and not where.startswith(allowed_files)}
        self.assertEqual(unexpected, {}, f"{label}: the frame path allocated")

    def test_steady_frames_allocate_nothing(self):
        request_sites = {site(*where) for where in PER_REQUEST.values()}
        capture_sites = {site(*where) for where in PER_CAPTURE.values()}
        with Session(size="1280x720@60", binary="vela-compositor-alloc-count",
                     env={"VELA_TEST_ALLOC_LOG": self.log}) as vela:
            vela.open_window(400, 300, decorated=True)
            vela.open_window(300, 200, command=[tool("vela-slowgpu"), "1"])
            time.sleep(WARM_UP_S)
            sites, frames, _ = self.window(vela)
            self.assertGreater(frames, 30, "the scene must redraw every frame")
            self.check(sites, request_sites, f"{frames} frames of a window and a dmabuf client")

            probe = subprocess.Popen([tool("vela-capture-probe"), "bench-reuse", "1", "0"], env=vela.client_env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            vela.clients.append(probe)
            time.sleep(WARM_UP_S / 2)  # the probe captures for 3 s
            sites, frames, readbacks = self.window(vela)
            probe.communicate(timeout=10)
            self.assertEqual(probe.returncode, 0)
            self.assertGreater(frames, 30, "the scene must redraw every frame")
            self.assertGreater(readbacks, 30, "the capture must copy every frame")
            self.check(sites, request_sites | capture_sites, f"{frames} frames and {readbacks} captures",
                       CAPTURE_WATCH_FILES)


if __name__ == "__main__":
    unittest.main()
