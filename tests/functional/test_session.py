# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The session: lock and unlock, outputs turned off and on, and the
supervisor restarting the compositor after a crash (locked too)."""

import os
import signal
import tempfile
import time
import unittest

from harness import Session


def child_compositor(supervisor_pid):
    """Il compositor avviato dal supervisore (suo figlio)."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/stat") as stat:
                fields = stat.read().rsplit(")", 1)[1].split()
            with open(f"/proc/{pid}/comm") as comm:
                name = comm.read().strip()
        except OSError:
            continue
        if int(fields[1]) == supervisor_pid and name == "vela-compositor":
            return int(pid)
    return None


class Lock(unittest.TestCase):
    def test_lock_hides_windows_and_unlock_restores_focus(self):
        with Session(lock_hold_ms=1500) as vela:
            window = vela.open_window()
            vela.wait_for(lambda s: s["focused"] == window)
            vela.command("lock")
            state = vela.wait_for(lambda s: s["locked"], what="locked")
            self.assertIsNone(state["focused"]) # no window has the keyboard
            state = vela.wait_for(lambda s: not s["locked"], timeout=6, what="unlocked")
            vela.wait_for(lambda s: s["focused"] == window, what="focus goes back to the window")

    def test_screen_dump_during_lock_does_not_reveal_windows(self):
        """Output capture must display only the lock surface."""
        with Session(lock_hold_ms=5000) as vela:
            vela.open_window()
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"], what="session locked")
            # The test locker paints #102040; wait until it is fully locked.
            vela.wait_for_log("vela-testlock: locked")
            image = vela.pixels()
            self.assertEqual(image.at(image.width // 2, image.height // 2), (16, 32, 64))

    def test_volume_keys_work_while_locked(self):
        with Session(lock_hold_ms=3000) as vela:
            shell = vela.fake_shell()
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            vela.keys("XF86AudioRaiseVolume", "XF86AudioPlay")
            shell.wait_for("media play-pause")
            self.assertEqual(shell.received("volume"), ["volume up"])

    def test_screens_off_and_on_while_locked(self):
        with Session(lock_hold_ms=4000) as vela:
            window = vela.open_window()
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            vela.command("test-power off")
            vela.wait_for(lambda s: not s.output["powered"], what="screen off")
            vela.command("test-power on")
            vela.wait_for(lambda s: s.output["powered"], what="screen back on")
            vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked")
            self.assertTrue(vela.state().has(window))


class Shell(unittest.TestCase):
    """The startup command (the shell): relaunched when it fails, but not
    when it exits cleanly nor when it keeps failing right after starting
    (shell.c)."""

    def runs(self, script):
        with tempfile.NamedTemporaryFile(prefix="vela-shell-runs-", delete=False) as counter:
            count = counter.name
        with Session(startup=f"sh -c 'echo run >> {count}; {script}'") as vela:
            time.sleep(2.5)
            log = vela.log_text()
        runs = len(open(count).read().split()) if os.path.exists(count) else 0
        if os.path.exists(count):
            os.unlink(count)
        return runs, log

    def test_shell_that_keeps_crashing_is_given_up(self):
        runs, log = self.runs("exit 3")
        self.assertEqual(runs, 3)
        self.assertIn("exited (code 3): restarting it", log)
        self.assertIn("keeps exiting (code 3): not restarting it", log)

    def test_shell_that_exits_cleanly_is_not_restarted(self):
        runs, log = self.runs("exit 0")
        self.assertEqual(runs, 1)
        self.assertNotIn("restarting it", log)


class Supervisor(unittest.TestCase):
    def test_redraw_signal_keeps_supervised_session_alive(self):
        # pkill -x matches /proc/<pid>/comm, not argv. Never run pkill in
        # tests: it could signal a real desktop running outside this session.
        with Session(supervise=True) as vela:
            supervisor_pid = vela.process.pid
            vela.wait_for(lambda _: child_compositor(supervisor_pid) is not None,
                          read_state=False, what="supervised compositor")
            compositor_pid = child_compositor(supervisor_pid)
            with open(f"/proc/{supervisor_pid}/comm") as name:
                self.assertEqual(name.read().strip(), "vela-supervise")
            with open(f"/proc/{compositor_pid}/comm") as name:
                self.assertEqual(name.read().strip(), "vela-compositor")

            # The main thread AND GPU worker threads must block SIGUSR1:
            # Wayland's signalfd can only receive a process-directed signal
            # reliably when another thread cannot handle it instead.
            redraw_bit = 1 << (signal.SIGUSR1 - 1)
            for tid in os.listdir(f"/proc/{compositor_pid}/task"):
                try:
                    with open(f"/proc/{compositor_pid}/task/{tid}/status") as status:
                        line = next(line for line in status if line.startswith("SigBlk:"))
                except (FileNotFoundError, ProcessLookupError):
                    continue  # A worker thread can exit while inspecting.
                self.assertNotEqual(int(line.split()[1], 16) & redraw_bit, 0,
                                    f"SIGUSR1 unblocked by compositor thread {tid}")

            # An accidental direct signal to the supervisor must not log out.
            os.kill(supervisor_pid, signal.SIGUSR1)
            time.sleep(0.2)
            self.assertIsNone(vela.process.poll())
            self.assertEqual(child_compositor(supervisor_pid), compositor_pid)

            # Targeting the real compositor must only request a redraw.
            os.kill(compositor_pid, signal.SIGUSR1)
            time.sleep(0.2)
            vela.state()
            self.assertIsNone(vela.process.poll())
            self.assertEqual(child_compositor(supervisor_pid), compositor_pid)
            self.assertNotIn("the compositor crashed", vela.log_text())

    def test_restart_after_crash(self):
        with Session(supervise=True) as vela:
            first = child_compositor(vela.process.pid)
            self.assertIsNotNone(first)
            os.kill(first, signal.SIGSEGV)
            vela.wait_for_log("restarted after a crash")
            second = vela.wait_for(lambda s: child_compositor(vela.process.pid) not in (None, first),
                                   read_state=False, what="a new compositor")
            vela.wait_for(lambda s: s["outputs"], what="the new compositor responds")

    def test_crash_while_locked_restarts_locked(self):
        # The test lock program stays 3 s, then unlocks by itself.
        with Session(supervise=True, lock_hold_ms=3000) as vela:
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            os.kill(child_compositor(vela.process.pid), signal.SIGSEGV)
            vela.wait_for_log("restarting locked")
            # Restarted locked (the desktop never revealed), then the unlock.
            time.sleep(0.5)
            vela.wait_for(lambda s: s["locked"], what="restarted locked")
            vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked by the new locker")

    def test_gives_up_after_three_quick_crashes(self):
        with Session(supervise=True, expected_exit=1) as vela:
            for _ in range(3):
                vela.wait_for(lambda s: child_compositor(vela.process.pid), read_state=False, what="the compositor")
                child = child_compositor(vela.process.pid)
                os.kill(child, signal.SIGSEGV)
                deadline = time.monotonic() + 5
                while child_compositor(vela.process.pid) == child and time.monotonic() < deadline:
                    time.sleep(0.05)
            vela.process.wait(10)
            self.assertEqual(vela.process.returncode, 1)
            self.assertIn("giving up", vela.log_text())
            runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
            self.assertFalse(os.path.exists(os.path.join(runtime, vela.display))) # Wayland socket removed


if __name__ == "__main__":
    unittest.main()
