# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Brightness in the compositor (docs/brightness.md §6, §7, §12): one
vela-brightness per output, on fake monitors that reproduce the faults
measured on real ones (brightness/src/fake.h). No hardware is touched: a
headless session starts helpers only with VELA_BRIGHTNESS_FAKE."""

import json
import os
import socket
import tempfile
import time
import unittest

from harness import Session, TimeoutError

# The LG of §3 on the first output, the Mi on the second (test-output add).
MONITORS = """
[HEADLESS-1]
value = 70
ignore-write-gap = 5-190
bad-replies = 2

[HEADLESS-2]
value = 50
drop-writes = 30
bad-replies = 50
"""


def fake_file(text):
    handle = tempfile.NamedTemporaryFile("w", prefix="vela-monitors-", suffix=".conf", delete=False)
    handle.write(text)
    handle.close()
    return handle.name


def outputs(vela):
    return {entry["name"]: entry for entry in vela.query("brightness")["outputs"]}


def wait_brightness(vela, condition, what, timeout=10):
    """Waits until `condition` holds for the brightness answer, by output."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = outputs(vela)
        if condition(last):
            return last
        time.sleep(0.05)
    raise TimeoutError(f"brightness: {what} (last: {last})")


def helpers(vela):
    """The vela-brightness processes of this compositor: {connector: pid}."""
    found = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/stat") as stat:
                parent = int(stat.read().rsplit(")", 1)[1].split()[1])
            with open(f"/proc/{entry}/cmdline", "rb") as cmdline:
                arguments = cmdline.read().split(b"\0")
        except (OSError, IndexError, ValueError):
            continue
        if parent == vela.process.pid and len(arguments) >= 3 and arguments[0].endswith(b"vela-brightness"):
            found[arguments[2].decode()] = int(entry)
    return found


def wait_helpers(vela, condition, what, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = helpers(vela)
        if condition(found):
            return found
        time.sleep(0.05)
    raise TimeoutError(f"helpers: {what} (last: {helpers(vela)})")


class Watcher:
    """A `watch brightness` connection, optionally one that never reads."""

    def __init__(self, vela, small_buffer=False):
        self.socket = socket.socket(socket.AF_UNIX)
        if small_buffer:
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        self.socket.connect(vela.socket_path)
        self.data = b""

    def send(self, line):
        self.socket.sendall((line + "\n").encode())

    def lines(self, quiet=0.3):
        """Everything received until nothing comes for `quiet` seconds."""
        self.socket.settimeout(quiet)
        while True:
            try:
                chunk = self.socket.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            self.data += chunk
        *complete, self.data = self.data.split(b"\n")
        return [json.loads(line) for line in complete if line]

    def close(self):
        self.socket.close()


class BrightnessTest(unittest.TestCase):
    monitors = MONITORS
    env = {}

    def setUp(self):
        self.fake = fake_file(self.monitors)
        self.vela = Session(env=dict(self.env, VELA_BRIGHTNESS_FAKE=self.fake))
        self.vela.start()
        self.started = time.monotonic()

    def tearDown(self):
        try:
            self.vela.stop()
        finally:
            os.unlink(self.fake)

    def ready(self, name="HEADLESS-1", value=None):
        return wait_brightness(self.vela, lambda o: name in o and o[name]["state"] == "ready"
                               and (value is None or abs(o[name]["value"] - value) < 1e-6),
                               f"{name} ready" + (f" at {value}" if value is not None else ""))

    def confirmed(self, name, value, timeout=10):
        return wait_brightness(self.vela, lambda o: name in o and not o[name]["pending"]
                               and abs(o[name]["value"] - value) < 1e-6, f"{name} confirmed at {value}", timeout)

    def add_output(self, name="HEADLESS-2"):
        self.vela.command("test-output add 1920x1080")
        self.vela.wait_for(lambda s: any(o["name"] == name for o in s["outputs"]), what=name)


class Basics(BrightnessTest):
    def test_probe_set_and_confirm(self):
        found = self.ready(value=0.7)["HEADLESS-1"]
        self.assertEqual((found["max"], found["pending"], found["refused"], found["seq"]), (100, False, False, 0))
        self.assertEqual(list(helpers(self.vela)), ["HEADLESS-1"])
        self.vela.command("brightness HEADLESS-1 0.3")
        # Shown at once as asked, then confirmed by the read back.
        now = outputs(self.vela)["HEADLESS-1"]
        self.assertEqual((now["value"], now["seq"]), (0.3, 1))
        self.confirmed("HEADLESS-1", 0.3)

    def test_watchers_get_every_change_and_the_same_state(self):
        self.ready()
        watcher = Watcher(self.vela)
        watcher.send("watch brightness")
        first = watcher.lines()
        self.assertEqual(first, [self.vela.query("brightness")])
        self.vela.command("brightness HEADLESS-1 0.4")
        self.confirmed("HEADLESS-1", 0.4)
        seen = [(e["outputs"][0]["value"], e["outputs"][0]["pending"]) for e in watcher.lines()]
        self.assertEqual(seen, [(0.4, True), (0.4, False)])
        # Nothing changed, nothing sent.
        self.vela.command("brightness HEADLESS-1 0.4")
        self.assertEqual(watcher.lines(), [])
        watcher.close()

    # The LG ignores a write up to 190 ms after another: a drag must still end
    # on the last value (§5.4).
    def test_a_drag_ends_on_the_last_value(self):
        self.ready()
        connection = Watcher(self.vela)
        for step in range(31):
            connection.send(f"brightness HEADLESS-1 {0.2 + step * 0.02:.2f}")
            time.sleep(0.02)
        self.assertEqual(outputs(self.vela)["HEADLESS-1"]["value"], 0.8)
        self.confirmed("HEADLESS-1", 0.8)
        connection.close()

    def test_bad_requests_change_nothing(self):
        self.ready()
        for line in ("brightness HEADLESS-1 1.5", "brightness HEADLESS-1 -0.1", "brightness HEADLESS-1 nan",
                     "brightness HEADLESS-1 0.5x", "brightness HEADLESS-1", "brightness NOPE-1 0.5",
                     "brightness-probe NOPE-1", "watch nothing"):
            self.vela.command(line)
        time.sleep(0.2)
        found = outputs(self.vela)["HEADLESS-1"]
        self.assertEqual((found["value"], found["seq"]), (0.7, 0))

    def test_power_off_and_on(self):
        self.ready()
        self.vela.command("test-power off")
        wait_brightness(self.vela, lambda o: o["HEADLESS-1"]["state"] == "off", "off")
        self.vela.command("brightness HEADLESS-1 0.2")
        time.sleep(0.2)
        self.assertEqual(outputs(self.vela)["HEADLESS-1"]["seq"], 0)
        pid = helpers(self.vela)["HEADLESS-1"]
        self.vela.command("test-power on")
        self.ready(value=0.7)
        # Off for idle, the helper stays.
        self.assertEqual(helpers(self.vela)["HEADLESS-1"], pid)


class Outputs(BrightnessTest):
    # COSMIC #150: one output's slider never touches another output.
    def test_each_output_has_its_own_helper_and_value(self):
        self.add_output()
        self.ready("HEADLESS-2", 0.5)
        self.ready("HEADLESS-1", 0.7)
        wait_helpers(self.vela, lambda h: set(h) == {"HEADLESS-1", "HEADLESS-2"}, "one per output")
        # The Mi loses writes at random: the read back writes again.
        for value in (0.9, 0.1, 0.65):
            self.vela.command(f"brightness HEADLESS-2 {value}")
            self.confirmed("HEADLESS-2", value, timeout=15)
        first = outputs(self.vela)["HEADLESS-1"]
        self.assertEqual((first["value"], first["seq"]), (0.7, 0))

    def test_unplugging_ends_the_helper(self):
        self.add_output()
        self.ready("HEADLESS-2")
        self.vela.command("test-output remove HEADLESS-2")
        wait_brightness(self.vela, lambda o: "HEADLESS-2" not in o, "HEADLESS-2 gone")
        wait_helpers(self.vela, lambda h: set(h) == {"HEADLESS-1"}, "HEADLESS-2's helper gone", timeout=3)
        # Plugged in again: a new output, a new helper.
        self.add_output("HEADLESS-3")
        wait_helpers(self.vela, lambda h: set(h) == {"HEADLESS-1", "HEADLESS-3"}, "HEADLESS-3's helper")

    def test_a_disabled_output_has_no_helper(self):
        self.add_output()
        self.ready("HEADLESS-2")
        self.vela.randr("--output", "HEADLESS-2", "--off")
        wait_brightness(self.vela, lambda o: o["HEADLESS-2"]["state"] == "off", "HEADLESS-2 off")
        wait_helpers(self.vela, lambda h: set(h) == {"HEADLESS-1"}, "HEADLESS-2's helper gone", timeout=3)
        self.vela.randr("--output", "HEADLESS-2", "--on")
        self.ready("HEADLESS-2", 0.5)


class SlowWatcher(BrightnessTest):
    # A watcher that stops reading can't block the compositor, and misses
    # states in between, never the last one (§7.2).
    def test_a_watcher_that_stops_reading(self):
        self.ready()
        stuck = Watcher(self.vela, small_buffer=True)
        stuck.send("watch brightness")
        commands = Watcher(self.vela)
        for step in range(3000):
            commands.send(f"brightness HEADLESS-1 {(step % 100) / 100:.2f}")
        commands.send("brightness HEADLESS-1 0.42")
        started = time.monotonic()
        self.vela.query("state")
        self.assertLess(time.monotonic() - started, 1.0)
        self.confirmed("HEADLESS-1", 0.42, timeout=15)
        received = stuck.lines(quiet=1.0)
        self.assertLess(len(received), 3000)
        self.assertEqual(received[-1], self.vela.query("brightness"))
        stuck.close()
        commands.close()


class Live(BrightnessTest):
    # The monitor's buttons, ms after its helper starts.
    monitors = """
[HEADLESS-1]
value = 70
buttons = 3000:40, 7000:20, 11000:60
"""

    def since_start(self, seconds):
        time.sleep(max(0.0, self.started + seconds - time.monotonic()))

    # Live sync is on while any connection has it on; a connection that
    # closes counts as off (§7.2).
    def test_live_sync_is_counted_per_connection(self):
        self.ready(value=0.7)
        self.since_start(4.0)
        self.assertEqual(outputs(self.vela)["HEADLESS-1"]["value"], 0.7) # nobody shows a slider
        a = Watcher(self.vela)
        b = Watcher(self.vela)
        a.send("brightness-live on")
        b.send("brightness-live on")
        self.confirmed("HEADLESS-1", 0.4, timeout=3)
        b.send("brightness-live off")
        self.confirmed("HEADLESS-1", 0.2, timeout=6) # a still shows one
        a.close() # without saying off
        self.since_start(12.5)
        self.assertEqual(outputs(self.vela)["HEADLESS-1"]["value"], 0.2)
        b.close()


class Watchdog(BrightnessTest):
    # The first output hangs on the transaction after its probe.
    monitors = """
[HEADLESS-1]
value = 70
hang-after = 1

[HEADLESS-2]
value = 50
"""

    def test_a_hung_helper_is_killed_alone(self):
        self.add_output()
        self.ready("HEADLESS-1")
        self.ready("HEADLESS-2")
        hung = helpers(self.vela)["HEADLESS-1"]
        self.vela.command("brightness HEADLESS-1 0.3")
        time.sleep(0.5)
        # The other monitor doesn't notice.
        self.vela.command("brightness HEADLESS-2 0.8")
        self.confirmed("HEADLESS-2", 0.8, timeout=3)
        found = wait_brightness(self.vela, lambda o: o["HEADLESS-1"]["state"] == "not-responding",
                                "HEADLESS-1 not responding", timeout=8)
        self.assertEqual((found["HEADLESS-1"]["value"], found["HEADLESS-1"]["pending"]), (0.7, False))
        self.assertIn("a transaction took more than 5 s", self.vela.log_text())
        # Not relaunched...
        wait_helpers(self.vela, lambda h: hung not in h.values(), "the hung helper gone", timeout=3)
        time.sleep(1.0)
        self.assertNotIn("HEADLESS-1", helpers(self.vela))
        # ... until it wakes up.
        self.vela.command("test-power off")
        self.vela.command("test-power on")
        self.ready("HEADLESS-1", 0.7)


class CrashLoop(BrightnessTest):
    env = {"VELA_BRIGHTNESS": "false"}
    monitors = "[HEADLESS-1]\n"

    def test_a_helper_that_keeps_exiting_is_not_relaunched(self):
        wait_brightness(self.vela, lambda o: o["HEADLESS-1"]["state"] == "not-responding", "not responding")
        time.sleep(1.0)
        log = self.vela.log_text()
        self.assertEqual(log.count("exited (code 1): restarting it"), 2)
        self.assertEqual(log.count("keeps exiting (code 1): not restarting it"), 1)


class Disabled(unittest.TestCase):
    def test_no_helper_on_a_headless_session_without_fake_monitors(self):
        with Session() as vela:
            time.sleep(0.5)
            self.assertEqual(vela.query("brightness"), {"outputs": []})
            self.assertEqual(helpers(vela), {})

    # A helper of one's own still drives DRM outputs only: a headless or
    # nested session has no monitor of ours behind its outputs.
    def test_no_helper_for_outputs_that_are_not_drm(self):
        helper = os.path.join(os.environ.get("VELA_BUILD", os.path.join(os.path.dirname(__file__), "..", "..",
                                                                         "build")), "brightness", "vela-brightness")
        with Session(env={"VELA_BRIGHTNESS": helper}) as vela:
            self.assertIn("Brightness: " + helper, vela.wait_for_log("Brightness: "))
            time.sleep(0.5)
            self.assertEqual(vela.query("brightness"), {"outputs": []})
            self.assertEqual(helpers(vela), {})

    def test_brightness_no_in_vela_conf(self):
        fake = fake_file(MONITORS)
        try:
            with Session(env={"VELA_BRIGHTNESS_FAKE": fake}) as vela:
                wait_brightness(vela, lambda o: "HEADLESS-1" in o, "a helper")
                config = os.path.join(vela.env["XDG_CONFIG_HOME"], "vela")
                os.makedirs(config, exist_ok=True)
                with open(os.path.join(config, "vela.conf"), "w") as conf:
                    conf.write("brightness=no\n")
                vela.command("reload-config")
                wait_brightness(vela, lambda o: o == {}, "no outputs")
                wait_helpers(vela, lambda h: h == {}, "no helpers", timeout=3)
        finally:
            os.unlink(fake)


if __name__ == "__main__":
    unittest.main()
