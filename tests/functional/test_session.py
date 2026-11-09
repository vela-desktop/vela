# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The session: lock and unlock, outputs turned off and on, and the
supervisor restarting the compositor after a crash (locked too)."""

import os
import select
import signal
import socket
import subprocess
import struct
import tempfile
import time
import unittest

from harness import BUILD, TIME_SCALE, Session, tool


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

    def test_hidden_window_resize_border_does_not_steal_lock_pointer(self):
        """Mouse motion at an invisible desktop resize border must reach the locker.

        Before the fix, resize_border_at() still walks disabled window children,
        chooses an east/west resize cursor, clears pointer focus and returns.
        This is why the lock screen can display a resize arrow over empty space.
        """
        with Session(lock_hold_ms=8000, env={"VELA_TESTLOCK_LOG_POINTER": "1"}) as vela:
            window = vela.open_window(400, 300, decorated=True)
            frame = vela.state().window(window)
            x = frame["x"] + frame["w"] + 4
            y = frame["y"] + frame["h"] // 2
            # Start outside the invisible resize band so that the second
            # move crosses into it only after the lock surface has mapped.
            vela.input("move", x + 60, y)
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"], what="locked")
            vela.wait_for_log("vela-testlock: locked")
            # Ensure the opaque lock surface has actually committed.
            vela.wait_for(lambda _: vela.pixels().at(10, 10) == (16, 32, 64),
                          what="mapped lock surface", read_state=False)
            vela.input("move", x, y)
            vela.wait_for_log(f"vela-testlock: pointer-motion {x} {y}", timeout=3)
            self.assertTrue(vela.state()["locked"], "pointer movement must not unlock")


    def test_stationary_pointer_click_reaches_new_lock_surface(self):
        """Pointer focus is assigned when the lock surface maps, without motion."""
        with Session(lock_hold_ms=6000, env={"VELA_TESTLOCK_LOG_POINTER": "1"}) as vela:
            vela.input("move", 350, 260)
            vela.command("lock")
            vela.wait_for_log("vela-testlock: locked")
            vela.wait_for(lambda _: vela.pixels().at(10, 10) == (16, 32, 64),
                          what="mapped lock surface", read_state=False)
            # Do not move the mouse after locking: a missing map-time retarget
            # cannot be hidden by the normal pointer-motion event path.
            vela.input("click")
            vela.wait_for_log("vela-testlock: pointer-button 272 1", timeout=3)
            vela.wait_for_log("vela-testlock: pointer-button 272 0", timeout=3)
            self.assertTrue(vela.state()["locked"])

    def test_keyboard_move_size_is_cancelled_before_lock(self):
        """Keyboard Move/Size must not steal the locker's password keystrokes."""
        for mode in ("move", "resize"):
            with self.subTest(mode=mode):
                with Session(lock_hold_ms=10000, env={"VELA_TESTLOCK_LOG_KEYBOARD": "1"}) as vela:
                    window = vela.open_window(400, 300, decorated=True)
                    vela.wait_for(lambda s: s["focused"] == window)
                    original = vela.state().window(window)
                    vela.command(f"window active {mode}")
                    # The headless backend has no physical keyboard. vela-input
                    # normally disconnects after each gesture, so the seat has
                    # no keyboard when the locker maps. Keep one virtual device
                    # connected throughout the lock, as a real keyboard would.
                    # The checkpoint roundtrip proves that it was registered.
                    keyboard = subprocess.Popen(
                        [tool("vela-input"), "checkpoint"], env=vela.client_env,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True)
                    try:
                        ready, _, _ = select.select([keyboard.stdout], [], [], 5 * TIME_SCALE)
                        self.assertTrue(ready, "virtual keyboard did not register")
                        self.assertEqual(keyboard.stdout.readline().strip(), "READY")
                        vela.command("lock")
                        vela.wait_for_log("vela-testlock: locked")
                        # Check actual wl_keyboard focus before delivering a key.
                        vela.wait_for_log("vela-testlock: keyboard-enter", timeout=5)
                        vela.input("key", "Right")
                        # Linux input-event-codes: KEY_RIGHT = 106.
                        vela.wait_for_log("vela-testlock: keyboard-key 106 1", timeout=3)
                        self.assertTrue(vela.state()["locked"])
                        frame = vela.state().window(window)
                        self.assertEqual(tuple(frame[k] for k in ("x", "y", "w", "h")),
                                         tuple(original[k] for k in ("x", "y", "w", "h")))
                    finally:
                        if keyboard.poll() is None:
                            keyboard.terminate()
                        keyboard.communicate(timeout=5)

    def test_lock_pointer_retargets_after_output_layout_change(self):
        """A cursor relocated to a surviving output must be able to click without moving."""
        with Session(lock_hold_ms=8000, env={"VELA_TESTLOCK_LOG_POINTER": "1"}) as vela:
            vela.command("test-output add 640x480")
            vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two headless outputs")
            vela.randr("--output", "HEADLESS-2", "--pos", "1920,0")
            vela.input("move", 2000, 100)
            vela.command("lock")
            vela.wait_for_log("vela-testlock: locked")
            vela.command("test-output remove HEADLESS-2")
            vela.wait_for(lambda s: len(s["outputs"]) == 1, what="second output removed")
            # A fresh click, not a motion event, must reach the remaining locker.
            vela.input("click")
            vela.wait_for_log("vela-testlock: pointer-button 272 1", timeout=3)
            self.assertTrue(vela.state()["locked"])

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

    def test_nonregistering_live_locker_is_stopped_before_retry(self):
        # A locker that stays alive without ext-session-lock registration must
        # be terminated before launching its replacement.
        with tempfile.TemporaryDirectory(prefix="vela-lock-live-") as directory:
            attempts = os.path.join(directory, "attempts")
            launcher = f"echo $$ >> '{attempts}'; exec sleep 30"
            with Session(env={"VELA_LOCK": launcher}) as vela:
                vela.command("lock")
                def pids():
                    if not os.path.exists(attempts):
                        return []
                    with open(attempts) as record:
                        return [int(line.strip()) for line in record if line.strip()]
                vela.wait_for(lambda _: len(pids()) >= 1, read_state=False, what="first live launcher")
                first_pid = pids()[0]
                vela.wait_for(lambda _: len(pids()) >= 2, read_state=False,
                              timeout=9, what="replacement live launcher")
                self.assertNotEqual(first_pid, pids()[1])
                with self.assertRaises(ProcessLookupError):
                    os.kill(first_pid, 0)
                self.assertTrue(vela.state()["locked"])
                self.assertIsNone(vela.state()["focused"])

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


class DamageFailure(unittest.TestCase):
    """A pixman region operation that runs out of memory must never leave
    stale pixels: the frame redraws whole buffers instead of trusting broken
    damage. The failures are real (an LD_PRELOAD shim makes malloc fail
    inside the chosen operation), not simulated in Vela's code."""

    SHIM = os.path.join(BUILD, "compositor", "libvela-pixman-fail-preload.so")
    # copy also reaches wlroots' own region code (wlr_region_transform).
    OPERATIONS = ("union", "union_rect", "intersect", "subtract", "copy")

    def start(self, **options):
        self.assertTrue(os.path.exists(self.SHIM), "build the vela-pixman-fail-preload target")
        directory = tempfile.mkdtemp(prefix="vela-pixman-fail-")
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", directory], check=False))
        self.trigger = os.path.join(directory, "trigger")
        self.injections = 0
        return Session(env={"LD_PRELOAD": self.SHIM, "VELA_TEST_PIXMAN_FAIL": self.trigger}, **options)

    def arm(self, vela, operation, skip, case):
        with open(self.trigger + ".tmp", "w") as trigger:
            trigger.write(f"{vela.process.pid} {operation} {skip} {case}")
        os.replace(self.trigger + ".tmp", self.trigger)

    def disarm(self, vela):
        """True when the armed failure really happened."""
        injected = not os.path.exists(self.trigger)
        if not injected:
            os.unlink(self.trigger)  # this change needed fewer allocations
        return injected

    def assert_matches_full_redraw(self, vela, what):
        # One capture after each of a few small frames (the pointer moving in
        # a corner): each frame reuses another swapchain buffer, so stale
        # pixels left in any one of them reach a capture.
        shown = []
        for step in range(3):
            vela.input("move", 3 + step % 2, 3)
            time.sleep(0.1 * TIME_SCALE)
            shown.append(vela.pixels())
        # SIGUSR1 redraws every buffer from scratch (server.c): if the image
        # changes, the damage had left stale pixels.
        os.kill(vela.process.pid, signal.SIGUSR1)
        time.sleep(0.3 * TIME_SCALE)
        redrawn = vela.pixels()
        for index, image in enumerate(shown):
            stale = sum(a != b for a, b in zip(image.rows, redrawn.rows))
            self.assertEqual(stale, 0, f"{what}, capture {index}: {stale} rows differ from a full redraw")

    @staticmethod
    def recoveries(vela):
        log = vela.log_text()
        return log.count("damage lost (out of memory)") + log.count("drawing hit a failed region operation")

    def test_failed_region_operations_never_leave_stale_pixels(self):
        # Decorated, most allocating operations are in occlusion culling.
        # Undecorated there are fewer, so increasing `skip` reaches the damage
        # diff, the damage ring and the drawing too.
        for decorated in (True, False):
            with self.subTest(decorated=decorated):
                self.check_failed_region_operations(decorated)

    def check_failed_region_operations(self, decorated):
        with self.start() as vela:
            window = vela.open_window(400, 300, decorated=decorated)
            vela.wait_for(lambda s: s["focused"] == window)
            injected = {operation: 0 for operation in self.OPERATIONS}
            case = 0
            for operation in self.OPERATIONS:
                for skip in range(6):
                    with self.subTest(operation=operation, skip=skip):
                        case += 1
                        # Keyboard Move changes the scene in one step, with no
                        # animation whose later frames would damage the lost
                        # area again. Return ends it, so the pointer moved
                        # below no longer drags the window.
                        vela.command("window active move")
                        time.sleep(0.1 * TIME_SCALE)
                        before = vela.state().window(window)["x"]
                        recovered = self.recoveries(vela)
                        self.arm(vela, operation, skip, case)
                        vela.input("key", "Right" if case % 2 else "Left")
                        vela.wait_for(lambda s: s.window(window)["x"] != before, what="the window moved")
                        time.sleep(0.2 * TIME_SCALE)
                        if self.disarm(vela):
                            injected[operation] += 1
                            # Recovered on its own, before a screen capture
                            # could force a frame of its own.
                            self.assertGreater(self.recoveries(vela), recovered, "no recovery frame")
                        vela.input("key", "Return")
                        self.assert_matches_full_redraw(vela, f"{operation} after {skip}")
            log = vela.log_text()
            # Every operation really failed at least once, along both paths.
            for operation, count in injected.items():
                self.assertGreater(count, 0, f"no {operation} failure was injected")
            self.assertIn("damage lost (out of memory)", log)
            self.assertIn("drawing hit a failed region operation", log)

    def test_failed_region_operation_while_locking_shows_only_the_lock(self):
        for operation in self.OPERATIONS:
            with self.subTest(operation=operation):
                with self.start(lock_hold_ms=10000) as vela:
                    vela.open_window(800, 600)
                    self.arm(vela, operation, 0, 1)
                    vela.command("lock")
                    vela.wait_for_log("vela-testlock: locked")
                    time.sleep(0.2 * TIME_SCALE)
                    self.disarm(vela)
                    image = vela.pixels()
                    lock_color = (16, 32, 64)  # what vela-testlock paints
                    for y in range(0, image.height, 16):
                        for x in range(0, image.width, 16):
                            self.assertEqual(image.at(x, y), lock_color, (x, y))
                    self.assert_matches_full_redraw(vela, f"lock after {operation}")


class ConfigurationRead(unittest.TestCase):
    """desktop.conf that exists but cannot be read whole is never rewritten
    from what the compositor managed to read (docs/memory-allocation-policy.md,
    "Existing defects", 1). vela.conf and outputs.conf: compositor.config-read."""

    SHIM = os.path.join(BUILD, "compositor", "libvela-getline-fail-preload.so")
    CONTENT = "# mine\ndesktop=Work\ndesktop=Games\nsticky-app=org.kde.konsole\n"

    def run_session(self, failure):
        config = tempfile.mkdtemp(prefix="vela-desktop-conf-")
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", config], check=False))
        os.mkdir(os.path.join(config, "vela"), 0o700)
        path = os.path.join(config, "vela", "desktop.conf")
        with open(path, "w") as file:
            file.write(self.CONTENT)
        env = {"XDG_CONFIG_HOME": config}
        if failure == "unreadable":
            os.chmod(path, 0)
        elif failure:
            self.assertTrue(os.path.exists(self.SHIM), "build the vela-getline-fail-preload target")
            env.update({"LD_PRELOAD": self.SHIM, "VELA_TEST_GETLINE_FAIL": f"desktop.conf 3 {failure}"})
        with Session(env=env) as vela:
            if failure:
                vela.wait_for_log("desktop.conf could not be read")
            # Both rewrite desktop.conf.
            desktops = vela.state()["workspaces"]
            vela.command("workspace rename 0 Renamed")
            vela.command("workspace new")
            vela.wait_for(lambda s: s["workspaces"] == desktops + 1, what="the new desktop")
            time.sleep(0.3 * TIME_SCALE)
        os.chmod(path, 0o600)
        with open(path) as file:
            return file.read()

    def test_desktop_conf_read_failure_never_rewrites_it(self):
        for failure in ("EIO", "ENOMEM", "unreadable"):
            with self.subTest(failure=failure):
                if failure == "unreadable" and os.geteuid() == 0:
                    self.skipTest("root ignores file permissions")
                self.assertEqual(self.run_session(failure), self.CONTENT)

    def test_readable_desktop_conf_is_still_saved(self):
        # The control: the same commands do rewrite a file read whole.
        content = self.run_session(None)
        self.assertIn("desktop=Renamed", content)
        self.assertIn("desktop=Games", content)
        self.assertIn("sticky-app=org.kde.konsole", content)


class VulkanShutdown(unittest.TestCase):
    """Headless shutdown smoke tests; physical DRM/KMS regression tests are separate."""

    @staticmethod
    def shutdown_env():
        # Opt-in on the dedicated GPU runners; generic CI may lack the layer.
        return {"VELA_VULKAN_VALIDATION": "1"} if os.environ.get("VELA_SHUTDOWN_VALIDATE") == "1" else {}

    def assert_cleanup_complete(self, vela):
        self.assertEqual(vela.process.wait(timeout=20), 0)
        log = vela.log_text()
        self.assertIn("Shutdown: releasing Vulkan renderer, allocator and device", log)
        self.assertEqual(log.count("Shutdown: Vulkan resource cleanup complete"), 1, log[-3000:])
        self.assertNotIn("Shutdown: skipping Vulkan resource cleanup", log)
        return log

    def test_normal_logout_releases_gpu_resources(self):
        with Session(env=self.shutdown_env()) as vela:
            vela.open_window()
            os.kill(vela.process.pid, signal.SIGTERM)
            self.assert_cleanup_complete(vela)

    def test_supervised_logout_does_not_restart(self):
        # SIGTERM goes to the compositor child, not to its supervisor: this
        # exercises the normal "Sign out" path and its zero-exit detection.
        with Session(supervise=True, env=self.shutdown_env()) as vela:
            vela.open_window()
            pid = child_compositor(vela.process.pid)
            self.assertIsNotNone(pid, "the supervised compositor must be running")
            os.kill(pid, signal.SIGTERM)
            log = self.assert_cleanup_complete(vela)
            self.assertNotIn("vela-supervise: restarting it", log)
            self.assertNotIn("vela-supervise: the compositor crashed", log)
            self.assertEqual(log.count("Vela running on WAYLAND_DISPLAY="), 1, log[-3000:])

    def test_repeated_normal_logout(self):
        # Keep generic CI short; dedicated hardware CI runs thirty iterations
        # with Vulkan validation enabled, with a window alive at shutdown.
        cycles = int(os.environ.get("VELA_SHUTDOWN_STRESS_CYCLES", "3"))
        self.assertGreaterEqual(cycles, 1)
        self.assertLessEqual(cycles, 50)
        for cycle in range(cycles):
            with self.subTest(cycle=cycle + 1):
                with Session(env=self.shutdown_env()) as vela:
                    vela.open_window()
                    os.kill(vela.process.pid, signal.SIGTERM)
                    self.assert_cleanup_complete(vela)

    def test_shutdown_after_locked_output_power_cycle(self):
        # Headless power state changes are not real DRM/KMS power cycling.
        with Session(lock_hold_ms=2000, env=self.shutdown_env()) as vela:
            vela.open_window()
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            vela.command("test-power off")
            vela.wait_for(lambda s: not s.output["powered"], what="screen off")
            vela.command("test-power on")
            vela.wait_for(lambda s: s.output["powered"], what="screen on")
            vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked")
            os.kill(vela.process.pid, signal.SIGTERM)
            self.assert_cleanup_complete(vela)

    def test_shutdown_while_locked_after_output_power_cycle(self):
        # Different lifetime path: shutdown while lock surfaces are still live.
        with Session(lock_hold_ms=15000, env=self.shutdown_env()) as vela:
            vela.open_window()
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            vela.command("test-power off")
            vela.wait_for(lambda s: not s.output["powered"], what="screen off")
            vela.command("test-power on")
            vela.wait_for(lambda s: s.output["powered"], what="screen on")
            self.assertTrue(vela.state()["locked"])
            os.kill(vela.process.pid, signal.SIGTERM)
            self.assert_cleanup_complete(vela)

    def test_diagnostic_cleanup_opt_out(self):
        with Session(env={"VELA_SKIP_VULKAN_CLEANUP": "1"}) as vela:
            os.kill(vela.process.pid, signal.SIGTERM)
            self.assertEqual(vela.process.wait(timeout=15), 0)
            log = vela.log_text()
            self.assertIn("Shutdown: skipping Vulkan resource cleanup", log)
            self.assertNotIn("Shutdown: Vulkan resource cleanup complete", log)


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

    def _marker(self, vela):
        runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        return os.path.join(runtime, f"vela-locked-{vela.display}")

    def test_crash_while_locked_restarts_locked_without_marker(self):
        # The shared lock state alone carries the lock across a restart: the
        # case of a marker that could not be created, or was removed, when a
        # crash happens (such as during the lock's fatal blackout window).
        with Session(supervise=True, lock_hold_ms=3000) as vela:
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            os.unlink(self._marker(vela))
            first = child_compositor(vela.process.pid)
            os.kill(first, signal.SIGSEGV)
            vela.wait_for_log("restarting locked")
            vela.wait_for(lambda s: s["locked"], what="restarted locked")
            vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked by the new locker")

            # After a real unlock the page says "unlocked": the next crash
            # restarts unlocked, as before.
            second = child_compositor(vela.process.pid)
            os.kill(second, signal.SIGSEGV)
            vela.wait_for(lambda s: child_compositor(vela.process.pid) not in (None, second),
                          read_state=False, what="a third compositor")
            vela.wait_for(lambda s: s["outputs"], what="the third compositor responds")
            self.assertFalse(vela.state()["locked"])
            self.assertEqual(vela.log_text().count("restarting locked"), 1)

    CRASHES = (signal.SIGSEGV, signal.SIGABRT)  # SIGABRT: what vela_oom will raise
    LIBC_SHIM = os.path.join(BUILD, "compositor", "libvela-libc-fail-preload.so")

    def _stop_once_logged(self, vela, compositor, text):
        """Run the compositor in 2 ms slices until `text` is logged, and leave
        it stopped there (the windows these tests aim at last a frame or more)."""
        deadline = time.monotonic() + 5 * TIME_SCALE
        os.kill(compositor, signal.SIGSTOP)
        while text not in vela.log_text():
            self.assertLess(time.monotonic(), deadline, f"never logged: {text}")
            os.kill(compositor, signal.SIGCONT)
            time.sleep(0.002)
            os.kill(compositor, signal.SIGSTOP)
        with open(f"/proc/{compositor}/stat") as stat:
            self.assertEqual(stat.read().rsplit(")", 1)[1].split()[0], "T", "stopped, not exited")

    def _crash_stopped(self, compositor, signum):
        os.kill(compositor, signum)
        os.kill(compositor, signal.SIGCONT)

    def _libc_failures(self):
        self.assertTrue(os.path.exists(self.LIBC_SHIM), "build the vela-libc-fail-preload target")
        directory = tempfile.mkdtemp(prefix="vela-libc-fail-")
        self.addCleanup(lambda: subprocess.run(["rm", "-rf", directory], check=False))
        setenv, fstatat = os.path.join(directory, "setenv"), os.path.join(directory, "fstatat")
        env = {"LD_PRELOAD": self.LIBC_SHIM, "VELA_TEST_SETENV_FAIL": setenv, "VELA_TEST_FSTATAT_FAIL": fstatat}
        return env, setenv, fstatat

    def test_crash_during_lock_blackout_restarts_locked(self):
        # The marker cannot be created, so the lock takes its fatal path: a
        # blackout, then the end of the session. A crash anywhere during that
        # window used to restart unlocked (no marker); the shared lock state
        # now restarts it locked.
        for signum in self.CRASHES:
            with self.subTest(signal=signal.Signals(signum).name):
                with Session(supervise=True, lock_hold_ms=1000) as vela:
                    marker = self._marker(vela)
                    compositor = child_compositor(vela.process.pid)
                    os.mkdir(marker, 0o700)  # renaming a file over it fails
                    try:
                        vela.command("lock")
                        self._stop_once_logged(vela, compositor, "ending session after the black backdrop")
                    finally:
                        # Without the directory the restarted compositor can
                        # create the marker; the page alone keeps it locked.
                        os.rmdir(marker)
                    self._crash_stopped(compositor, signum)
                    vela.wait_for_log("restarting locked")
                    log = vela.log_text()
                    self.assertIn("the compositor crashed", log)
                    self.assertNotIn("secure lock state could not be persisted", log)
                    vela.wait_for(lambda s: s["locked"], what="restarted locked")
                    vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked by the new locker")

    def test_crash_while_locking_restarts_locked(self):
        # Between the lock engaging and the locker being told "locked".
        for signum in self.CRASHES:
            with self.subTest(signal=signal.Signals(signum).name):
                with Session(supervise=True, lock_hold_ms=1000) as vela:
                    compositor = child_compositor(vela.process.pid)
                    vela.command("lock")
                    self._stop_once_logged(vela, compositor, "Screen locked")
                    self.assertNotIn("vela-testlock: locked", vela.log_text())
                    self._crash_stopped(compositor, signum)
                    vela.wait_for_log("restarting locked")
                    vela.wait_for(lambda s: s["locked"], what="restarted locked")
                    vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked by the new locker")

    def test_crash_while_locked_restarts_locked_after_abort(self):
        # The same as test_crash_while_locked_restarts_locked, with the signal
        # an allocation failure raises.
        with Session(supervise=True, lock_hold_ms=3000) as vela:
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            os.kill(child_compositor(vela.process.pid), signal.SIGABRT)
            vela.wait_for_log("restarting locked")
            vela.wait_for(lambda s: s["locked"], what="restarted locked")

    def test_setenv_failure_in_restart_child_never_restarts_unlocked(self):
        for signum in self.CRASHES:
            with self.subTest(signal=signal.Signals(signum).name):
                env, setenv, _ = self._libc_failures()
                with Session(supervise=True, lock_hold_ms=3000, env=env) as vela:
                    vela.command("lock")
                    vela.wait_for(lambda s: s["locked"])
                    with open(setenv, "w") as trigger:
                        trigger.write("VELA_RESTARTED once")
                    os.kill(child_compositor(vela.process.pid), signum)
                    # That child gives up before exec; the next one starts
                    # locked from the page.
                    vela.wait_for_log("injected setenv failure")
                    vela.wait_for_log("exited with an error (code 127)")
                    vela.wait_for_log("restarting locked")
                    vela.wait_for(lambda s: s["locked"], what="restarted locked")

    def test_setenv_always_failing_ends_the_session(self):
        env, setenv, _ = self._libc_failures()
        with Session(supervise=True, lock_hold_ms=10000, env=env, expected_exit=1) as vela:
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"])
            with open(setenv, "w") as trigger:
                trigger.write("VELA_RESTARTED")
            os.kill(child_compositor(vela.process.pid), signal.SIGABRT)
            self.assertEqual(vela.process.wait(timeout=15), 1)
            log = vela.log_text()
            self.assertIn("giving up", log)
            self.assertEqual(log.count("Vela running on"), 1, "no compositor ever started again")

    def test_marker_that_cannot_be_checked_restarts_locked(self):
        # Unlocked page, no marker, but fstatat on the marker fails (EIO): its
        # absence is not proven, so the restart is locked.
        env, _, fstatat = self._libc_failures()
        with Session(supervise=True, lock_hold_ms=1000, env=env) as vela:
            open(fstatat, "w").close()
            try:
                os.kill(child_compositor(vela.process.pid), signal.SIGSEGV)
                vela.wait_for_log("Lock marker unreadable")
                vela.wait_for_log("restarting locked")
            finally:
                os.unlink(fstatat)
            vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked by the new locker")

    def test_unreadable_lock_marker_restarts_locked(self):
        # A marker whose state cannot be established counts as present, even
        # though the page says "unlocked". A group-readable file is not a
        # valid marker, but the restarted lock can still replace it (a
        # directory could not be replaced, which ends the session instead).
        with Session(supervise=True, lock_hold_ms=1000) as vela:
            marker = self._marker(vela)
            with open(marker, "w"):
                pass
            os.chmod(marker, 0o640)
            try:
                os.kill(child_compositor(vela.process.pid), signal.SIGSEGV)
                vela.wait_for_log("Lock marker unreadable")
                vela.wait_for_log("restarting locked")
                vela.wait_for(lambda s: not s["locked"], timeout=8, what="unlocked by the new locker")
                self.assertFalse(os.path.exists(marker))
            finally:
                if os.path.exists(marker):
                    os.unlink(marker)

    def test_lock_state_is_never_inherited_by_applications(self):
        def descriptors(pid):
            links = []
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    links.append(os.readlink(f"/proc/{pid}/fd/{fd}"))
                except OSError:
                    pass
            return links

        def descendants(pid):
            found, frontier = [], [pid]
            while frontier:
                parent = frontier.pop()
                try:
                    children = open(f"/proc/{parent}/task/{parent}/children").read().split()
                except OSError:
                    continue
                found += [int(child) for child in children]
                frontier += [int(child) for child in children]
            return found

        page = "memfd:vela-lock-state"
        with Session(supervise=True, lock_hold_ms=5000) as vela:
            vela.command("lock")  # starts the locker, a child of the compositor
            vela.wait_for(lambda s: s["locked"])
            compositor = child_compositor(vela.process.pid)
            self.assertTrue(any(page in link for link in descriptors(vela.process.pid)),
                            "the supervisor keeps the page for the next compositor")
            self.assertFalse(any(page in link for link in descriptors(compositor)),
                             "the compositor keeps only the mapping")
            with open(f"/proc/{compositor}/maps") as maps:
                self.assertIn(page, maps.read())
            applications = descendants(compositor)
            self.assertTrue(applications, "the locker is running")
            for pid in applications:
                self.assertFalse(any(page in link for link in descriptors(pid)), pid)

    def test_unusable_lock_state_ends_session_before_starting(self):
        # Only the supervisor passes --lock-state-fd; a value that is not its
        # sealed page is refused before anything is shown (status 77).
        compositor = os.path.join(BUILD, "compositor", "vela-compositor")
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_LIBINPUT_NO_DEVICES="1")
        env.pop("WAYLAND_DISPLAY", None)
        for value in ("0", "abc", "-1", "999"):
            with self.subTest(value=value):
                result = subprocess.run([compositor, f"--lock-state-fd={value}"], env=env,
                                        stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 77, result.stderr)
                self.assertIn(b"lock state is unusable", result.stderr)

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
