# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The session: lock and unlock, outputs turned off and on, and the
supervisor restarting the compositor after a crash (locked too)."""

import os
import signal
import socket
import struct
import tempfile
import time
import unittest

from harness import Session, tool


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

    def test_missing_locker_never_exposes_desktop(self):
        # A failed launcher must hide windows and revoke keyboard focus even
        # when no ext-session-lock client has connected.
        with Session(env={"VELA_LOCK": "/nonexistent/vela-test-lock-missing"}) as vela:
            window = vela.open_window()
            vela.wait_for(lambda s: s["focused"] == window)
            vela.command("lock")
            state = vela.wait_for(lambda s: s["locked"], what="locked without locker")
            self.assertIsNone(state["focused"])
            image = vela.pixels()
            self.assertEqual(image.at(image.width // 2, image.height // 2), (0, 0, 0))
            self.assertIn("Locking the screen", vela.log_text())

    def test_locker_exits_without_registering_stays_locked(self):
        with Session(env={"VELA_LOCK": "exit 1"}) as vela:
            vela.open_window()
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            time.sleep(0.3)
            self.assertTrue(vela.state()["locked"])
            self.assertIsNone(vela.state()["focused"])

    def test_repeated_requests_do_not_launch_duplicate_lockers(self):
        with tempfile.TemporaryDirectory(prefix="vela-lock-retries-") as directory:
            attempts = os.path.join(directory, "attempts")
            with Session(env={"VELA_LOCK": f"echo launch >> '{attempts}'; exit 1"}) as vela:
                vela.command("lock")
                vela.wait_for(lambda _: os.path.exists(attempts), read_state=False, what="first launcher")
                for _ in range(4):
                    vela.command("lock")
                time.sleep(0.4)
                with open(attempts) as record:
                    self.assertEqual(len(record.readlines()), 1, "duplicate lockers were launched")
                def attempted_twice(_):
                    if not os.path.exists(attempts):
                        return False
                    with open(attempts) as record:
                        return len(record.readlines()) >= 2
                vela.wait_for(attempted_twice, read_state=False, timeout=9, what="automatic retry")
                self.assertTrue(vela.state()["locked"])

    def test_nonregistering_locker_recovers_when_available(self):
        with tempfile.TemporaryDirectory(prefix="vela-lock-recovery-") as directory:
            ready = os.path.join(directory, "ready")
            attempts = os.path.join(directory, "attempts")
            # Wait for the first failed attempt BEFORE making the test locker
            # available, so the test cannot pass without a genuine retry.
            launcher = (f"echo launch >> '{attempts}'; "
                        f"if [ -e '{ready}' ]; then exec '{tool('vela-testlock')}' 2000; else exit 1; fi")
            with Session(env={"VELA_LOCK": launcher}) as vela:
                vela.command("lock")
                vela.wait_for(lambda _: os.path.exists(attempts), read_state=False,
                              what="first failed locker launch")
                with open(ready, "w"):
                    pass
                vela.wait_for_log("vela-testlock: locked", timeout=14)
                vela.wait_for(lambda s: not s["locked"], timeout=9, what="locker unlocks")
                # The original 5-second watchdog must have been disarmed,
                # otherwise it could re-lock the already unlocked desktop.
                time.sleep(5.5)
                self.assertFalse(vela.state()["locked"])
                self.assertEqual(vela.log_text().count("Locking the screen:"), 2,
                                 "the retry timer should stop on registration")

    def test_failed_locker_uses_five_fast_attempts_then_cools_down(self):
        with tempfile.TemporaryDirectory(prefix="vela-lock-throttle-") as directory:
            attempts = os.path.join(directory, "attempts")
            with Session(env={"VELA_LOCK": f"echo launch >> '{attempts}'; exit 1"}) as vela:
                vela.command("lock")
                def launch_count():
                    if not os.path.exists(attempts):
                        return 0
                    with open(attempts) as record:
                        return len(record.readlines())
                vela.wait_for(lambda _: launch_count() == 5, read_state=False,
                              timeout=30, what="five unsuccessful locker launches")
                time.sleep(5.5)
                self.assertEqual(launch_count(), 5, "cooldown must suppress the sixth fast launch")
                self.assertTrue(vela.state()["locked"])
                self.assertIsNone(vela.state()["focused"])

    def test_registered_locker_crash_is_retried(self):
        with tempfile.TemporaryDirectory(prefix="vela-lock-crash-") as directory:
            pid_path = os.path.join(directory, "locker.pid")
            launcher = f"echo $$ > '{pid_path}'; exec '{tool('vela-testlock')}' 20000"
            with Session(env={"VELA_LOCK": launcher}) as vela:
                vela.command("lock")
                vela.wait_for_log("vela-testlock: locked")
                with open(pid_path) as record:
                    first_pid = int(record.read().strip())
                os.kill(first_pid, signal.SIGKILL)
                def relaunched(_):
                    try:
                        with open(pid_path) as record:
                            return int(record.read().strip()) != first_pid
                    except (OSError, ValueError):
                        return False
                vela.wait_for(relaunched, read_state=False, timeout=9,
                              what="new locker after previous registered locker crashed")
                self.assertTrue(vela.state()["locked"])
                self.assertGreaterEqual(vela.log_text().count("Locking the screen:"), 2)

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


class Caffeine(unittest.TestCase):
    """Caffeine (quick settings): inactivity turns nothing off until it's
    turned off again, or its minutes run out (lock.c)."""

    def test_caffeine_holds_off_inactivity(self):
        # 3 seconds of inactivity; headless it needs VELA_SCREEN_OFF.
        with Session(env={"VELA_SCREEN_OFF": "0.05", "VELA_LOCK_ON_IDLE": "no"}) as vela:
            self.assertFalse(vela.query("accessibility")["caffeine"])
            vela.command("caffeine on")
            vela.wait_for(lambda s: vela.query("accessibility")["caffeine"], read_state=False, what="caffeine on")
            self.assertEqual(vela.query("accessibility")["caffeineSecs"], -1)
            vela.wait_for(lambda s: s.output["powered"], what="screen on")
            time.sleep(4.5)
            self.assertTrue(vela.state().output["powered"])
            vela.command("caffeine toggle")
            vela.wait_for(lambda s: not s.output["powered"], timeout=6, what="screen off without caffeine")
            self.assertFalse(vela.query("accessibility")["caffeine"])

    def test_caffeine_turns_itself_off(self):
        # 0.04 minutes are about 2.4 seconds; the screen (3 seconds of
        # inactivity) must still be on just after, and off once time is up.
        with Session(env={"VELA_SCREEN_OFF": "0.05", "VELA_LOCK_ON_IDLE": "no"}) as vela:
            vela.command("caffeine on 0.04")
            vela.wait_for(lambda s: vela.query("accessibility")["caffeine"], read_state=False, what="timed caffeine on")
            self.assertGreater(vela.query("accessibility")["caffeineSecs"], 0)
            vela.wait_for(lambda s: not vela.query("accessibility")["caffeine"], read_state=False, timeout=10,
                          what="caffeine off by itself")
            vela.wait_for(lambda s: not s.output["powered"], timeout=6, what="screen off after time is up")

    def test_caffeine_prevents_automatic_lock(self):
        # Test lock-on-idle itself, not just monitor power (the other tests
        # explicitly disable idle locking).
        with Session(env={"VELA_SCREEN_OFF": "0.05"}, lock_hold_ms=4000) as vela:
            vela.command("caffeine on")
            vela.wait_for(lambda _: vela.query("accessibility")["caffeine"],
                          read_state=False, what="caffeine enabled")
            time.sleep(3.5)
            self.assertFalse(vela.state()["locked"])
            vela.command("caffeine off")
            vela.wait_for(lambda s: s["locked"], timeout=7, what="automatic lock after caffeine")

    def test_caffeine_presets_and_invalid_durations(self):
        with Session() as vela:
            vela.command("caffeine on 30")
            vela.wait_for(lambda _: vela.query("accessibility")["caffeineDurationSecs"] == 1800,
                          read_state=False, what="30 minute duration")
            for value in ("bogus", "-1", "0", "1e309", "1441", "30junk"):
                vela.command("caffeine on " + value)
                # The command socket is asynchronous; checking after a small
                # delay ensures the bad command was handled.
                time.sleep(0.05)
                status = vela.query("accessibility")
                self.assertTrue(status["caffeine"])
                self.assertEqual(status["caffeineDurationSecs"], 1800)
            vela.command("caffeine on 60")
            vela.wait_for(lambda _: vela.query("accessibility")["caffeineDurationSecs"] == 3600,
                          read_state=False, what="renewed 60 minute duration")
            vela.command("caffeine on")
            vela.wait_for(lambda _: vela.query("accessibility")["caffeineSecs"] == -1,
                          read_state=False, what="indefinite caffeine")
            self.assertEqual(vela.query("accessibility")["caffeineDurationSecs"], 0)
            vela.command("caffeine off")
            vela.wait_for(lambda _: not vela.query("accessibility")["caffeine"],
                          read_state=False, what="caffeine disabled")

    def test_caffeine_confirmed_commands_report_authoritative_state(self):
        with Session() as vela:
            enabled = vela.query("caffeine-set on 30")
            self.assertTrue(enabled["ok"])
            self.assertTrue(enabled["state"]["caffeine"])
            self.assertEqual(enabled["state"]["caffeineDurationSecs"], 1800)

            # A rejected request must report failure and the existing state.
            invalid = vela.query("caffeine-set on bogus")
            self.assertFalse(invalid["ok"])
            self.assertTrue(invalid["state"]["caffeine"])

            disabled = vela.query("caffeine-set off")
            self.assertTrue(disabled["ok"])
            self.assertFalse(disabled["state"]["caffeine"])
            self.assertEqual(disabled["state"]["caffeineSecs"], 0)

    def test_caffeine_submillisecond_timer_is_not_stuck(self):
        # Fractional durations must expire even if the millisecond timer
        # fires before the CLOCK_MONOTONIC deadline.
        with Session() as vela:
            vela.command("caffeine on 0.000025")  # 1.5 ms
            vela.wait_for_log("Caffeine time is up", timeout=5)
            self.assertFalse(vela.query("accessibility")["caffeine"])


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


class CommandSocket(unittest.TestCase):
    def test_client_that_hangs_up_before_the_reply_does_not_kill_the_session(self):
        # A large reply ("state" with windows) to clients that reset the
        # connection at once: writing to them must not raise SIGPIPE.
        with Session() as vela:
            for _ in range(20):
                vela.open_window(200, 150)
            for _ in range(50):
                with socket.socket(socket.AF_UNIX) as client:
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                    client.connect(vela.socket_path)
                    client.sendall(b"state\n")
            self.assertEqual(len(vela.state().windows), 20)


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

            # The main thread AND GPU worker threads must block SIGUSR1 and
            # SIGINT/SIGTERM: Wayland's signalfd can only receive a
            # process-directed signal reliably when another thread cannot
            # handle it instead.
            redraw_bit = 1 << (signal.SIGUSR1 - 1)
            exit_bits = (1 << (signal.SIGINT - 1)) | (1 << (signal.SIGTERM - 1))
            for tid in os.listdir(f"/proc/{compositor_pid}/task"):
                try:
                    with open(f"/proc/{compositor_pid}/task/{tid}/status") as status:
                        line = next(line for line in status if line.startswith("SigBlk:"))
                except (FileNotFoundError, ProcessLookupError):
                    continue  # A worker thread can exit while inspecting.
                self.assertNotEqual(int(line.split()[1], 16) & redraw_bit, 0,
                                    f"SIGUSR1 unblocked by compositor thread {tid}")
                self.assertEqual(int(line.split()[1], 16) & exit_bits, exit_bits,
                                 f"SIGINT/SIGTERM unblocked by compositor thread {tid}")

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

    def test_termination_signals_exit_cleanly(self):
        # Both signals must reach Wayland's signalfd and shut down the
        # compositor without being mistaken for a crash by the supervisor.
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signal.Signals(signum).name):
                with Session(supervise=True) as vela:
                    compositor_pid = child_compositor(vela.process.pid)
                    self.assertIsNotNone(compositor_pid)
                    os.kill(compositor_pid, signum)
                    self.assertEqual(vela.process.wait(timeout=15), 0)
                    log = vela.log_text()
                    self.assertNotIn("the compositor crashed", log)
                    self.assertNotIn("restarting it", log)

    def test_restart_after_crash(self):
        with Session(supervise=True) as vela:
            first = child_compositor(vela.process.pid)
            self.assertIsNotNone(first)
            os.kill(first, signal.SIGSEGV)
            vela.wait_for_log("restarted after a crash")
            second = vela.wait_for(lambda s: child_compositor(vela.process.pid) not in (None, first),
                                   read_state=False, what="a new compositor")
            vela.wait_for(lambda s: s["outputs"], what="the new compositor responds")

    def test_unwritable_lock_marker_ends_session_without_restart(self):
        # Replacing a non-empty directory with the marker must fail.
        # This exercises the real marker code without fault-injection hooks.
        with Session(supervise=True, expected_exit=77) as vela:
            runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
            marker = os.path.join(runtime, f"vela-locked-{vela.display}")
            os.mkdir(marker, 0o700)
            try:
                vela.command("lock")
                self.assertEqual(vela.process.wait(timeout=12), 77)
                log = vela.log_text()
                self.assertIn("secure lock state could not be persisted", log)
                self.assertIn("blackout committed on all active outputs", log)
                self.assertNotIn("restarting it", log)
            finally:
                os.rmdir(marker)

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
