# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Sustained rendering and post-hang evidence, with a real GPU and 1024 FDs.

The NVIDIA report exhausted descriptors after seconds of rendering. Exercise
both synchronization paths beyond that limit and check actual /proc growth.
"""

import os
import re
import resource
import signal
import socket
import time
import unittest

from harness import Session, tool


def descriptor_counts(pid):
    links = []
    for fd in os.scandir(f"/proc/{pid}/fd"):
        try:
            links.append(os.readlink(fd.path))
        except FileNotFoundError:
            pass
    return len(links), sum("sync_file" in link for link in links)


def limit_with_room_for(pid, count):
    """The RLIMIT_NOFILE that leaves the process room for count more
    descriptors: one above its count-th lowest free number, since a new
    descriptor takes the lowest free one and a gap may sit below the last."""
    used = {int(name) for name in os.listdir(f"/proc/{pid}/fd")}
    number = -1
    for _ in range(count):
        number += 1
        while number in used:
            number += 1
    return number + 1


class DescriptorExhaustion(unittest.TestCase):
    """Event sources that create descriptors fail locally
    (docs/memory-allocation-policy.md, point 2b): the compositor's real
    RLIMIT_NOFILE is lowered while it runs, so the next descriptor it creates
    fails with EMFILE."""

    def test_a_connection_and_a_timer_fail_locally(self):
        with Session() as vela:
            pid = vela.process.pid
            vela.state()
            soft, hard = resource.prlimit(pid, resource.RLIMIT_NOFILE)
            try:
                # Room for the accepted connection alone: watching it needs a
                # duplicate, which fails, and the connection is dropped.
                resource.prlimit(pid, resource.RLIMIT_NOFILE, (limit_with_room_for(pid, 1), hard))
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(5)
                    connection.connect(vela.socket_path)
                    self.assertEqual(connection.recv(1), b"")
                vela.wait_for_log("Can't watch a command connection")
                # Room for the command's connection and its watch: the new
                # output's timers (its latch and its virtual vblank, each a
                # timerfd) get none. libwayland's own timers share one
                # timerfd, made at startup, so they cannot fail this way.
                # The command's connection stays open until the output
                # exists. One closed at once is read to its end and dropped
                # before the command runs, and its two descriptors go to
                # whatever the new output creates first: its buffers, or
                # its latch.
                resource.prlimit(pid, resource.RLIMIT_NOFILE, (limit_with_room_for(pid, 2), hard))
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.connect(vela.socket_path)
                    connection.sendall(b"test-output add 640x480\n")
                    vela.wait_for_log("HEADLESS-2: no virtual vblank, frames follow the backend")
                    vela.wait_for_log("test-output: connected HEADLESS-2")
            finally:
                resource.prlimit(pid, resource.RLIMIT_NOFILE, (soft, hard))
            # Its buffers could not be made under the same limit either, so it
            # starts off. Switched on now, it draws without its timers: each
            # redraw (SIGUSR1 redraws everything) presents a frame, following
            # the backend's frame events, and the compositor keeps answering.
            def frames(state):
                return next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")["frames"]
            vela.randr("--output", "HEADLESS-2", "--on")
            before = frames(vela.wait_for(
                lambda s: any(o["name"] == "HEADLESS-2" and o["enabled"] for o in s["outputs"]),
                what="the output without its timers on"))
            os.kill(pid, signal.SIGUSR1)
            vela.wait_for(lambda s: frames(s) > before,
                          what="frames on the output without its timers")


class RetainedResources(unittest.TestCase):
    def test_supervisor_keeps_sampling_a_frozen_compositor(self):
        with Session(supervise=True) as vela:
            line = vela.wait_for_log("vela-supervise: resources ")
            pid = int(re.search(r"pid=(\d+)", line)[1])
            os.kill(pid, signal.SIGSTOP)
            try:
                before = vela.log_text().count("vela-supervise: resources ")
                vela.wait_for(lambda _: vela.log_text().count("vela-supervise: resources ") > before,
                              timeout=5, read_state=False, what="resources while compositor is stopped")
            finally:
                os.kill(pid, signal.SIGCONT)
            vela.state() # the compositor responds after resuming

    def exercise_rendering(self, mode):
        with Session(supervise=True, nofile=1024, size="800x600@120",
                     env={"VELA_SYNC_FILE": mode}) as vela:
            line = vela.wait_for_log("vela-supervise: resources ")
            pid = int(re.search(r"pid=(\d+)", line)[1])
            vela.open_window(command=[tool("vela-slowgpu"), "1", "--implicit"], timeout=15)
            if mode == "0":
                self.assertIn("CPU synchronization", vela.log_text())
                self.assertNotIn("Explicit sync with apps (linux-drm-syncobj-v1) enabled", vela.log_text())
            start = vela.state().output["frames"]
            vela.wait_for(lambda s: s.output["frames"] >= start + 120, timeout=10, what="renderer warm-up")
            baseline = descriptor_counts(pid)
            start = vela.state().output["frames"]
            deadline = time.monotonic() + 35
            samples = []
            while time.monotonic() < deadline:
                state = vela.state()
                samples.append(descriptor_counts(pid))
                self.assertLessEqual(samples[-1][0], baseline[0] + 64,
                                     f"FD growth while rendering: baseline={baseline}, samples={samples}")
                if state.output["frames"] >= start + 1200:
                    break
                time.sleep(0.25)
            else:
                self.fail("compositor did not render 1200 additional frames in 35 seconds")
            self.assertLessEqual(samples[-1][1], baseline[1] + 16, f"sync_file growth: {samples}")
            self.assertNotIn("Too many open files", vela.log_text())

    def test_gpu_sync_has_bounded_descriptors_over_1200_frames(self):
        self.exercise_rendering("1")

    def test_cpu_sync_has_bounded_descriptors_over_1200_frames(self):
        self.exercise_rendering("0")


if __name__ == "__main__":
    unittest.main()
