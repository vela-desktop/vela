# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Brightness in the compositor (docs/brightness.md §6, §7, §12): one
vela-brightness per output, on fake monitors that reproduce the faults
measured on real ones (brightness/src/fake.h). No hardware is touched: a
headless session starts helpers only with VELA_BRIGHTNESS_FAKE."""

import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

import test_shell
from harness import BUILD, Image, Session, TimeoutError

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
            target = arguments[2].decode()
            if target in found:
                raise AssertionError(f"duplicate vela-brightness helpers for {target}: {found[target]}, {entry}")
            found[target] = int(entry)
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


class Backlight(BrightnessTest):
    """A built-in screen's backlight on a fake sysfs tree (§5.3): the first
    output's connector has a backlight device, written straight into its
    file. logind and the real backlights are never reached."""

    monitors = "[HEADLESS-2]\nvalue = 50\n"

    def setUp(self):
        self.tree = tempfile.mkdtemp(prefix="vela-sysfs-")
        os.makedirs(os.path.join(self.tree, "class/drm/HEADLESS-1"))
        self.device = os.path.join(self.tree, "class/backlight/test_bl")
        os.makedirs(self.device)
        os.symlink("../../drm/HEADLESS-1", os.path.join(self.device, "device"))
        self.write("max_brightness", 96000)
        self.write("brightness", 48000)
        self.env = {"VELA_BRIGHTNESS_SYSFS": self.tree}
        super().setUp()

    def tearDown(self):
        try:
            super().tearDown()
        finally:
            shutil.rmtree(self.tree, ignore_errors=True)

    def write(self, name, value):
        with open(os.path.join(self.device, name), "w") as file:
            file.write(f"{value}\n")

    def read(self, name):
        with open(os.path.join(self.device, name)) as file:
            return int(file.read())

    def test_a_built_in_screen_uses_its_backlight(self):
        found = self.ready("HEADLESS-1", 0.5)["HEADLESS-1"]
        self.assertEqual((found["mechanism"], found["max"]), ("backlight", 96000))
        self.assertIn("test_bl", helpers(self.vela)) # vela-brightness backlight test_bl
        self.vela.command("brightness HEADLESS-1 0.25")
        self.confirmed("HEADLESS-1", 0.25, timeout=3)
        self.assertEqual(self.read("brightness"), 24000)
        # Never 0 with this many levels: the panel could turn off.
        self.vela.command("brightness HEADLESS-1 0")
        self.confirmed("HEADLESS-1", 1 / 96000, timeout=3)
        self.assertEqual(self.read("brightness"), 1)
        # An external monitor next to it is still DDC/CI's.
        self.add_output()
        self.assertEqual(self.ready("HEADLESS-2", 0.5)["HEADLESS-2"]["mechanism"], "ddc")
        self.assertEqual(set(helpers(self.vela)), {"test_bl", "HEADLESS-2"})

    def test_reenable_waits_for_previous_backlight_helper(self):
        """Don't start a second backlight writer while the previous one exits."""
        self.ready("HEADLESS-1", 0.5)
        previous = helpers(self.vela)["test_bl"]
        # Hold the old writer alive after SIGTERM to make the race deterministic.
        os.kill(previous, signal.SIGSTOP)
        try:
            self.vela.randr("--output", "HEADLESS-1", "--off")
            wait_brightness(self.vela, lambda o: o["HEADLESS-1"]["state"] == "off", "backlight off")
            self.vela.randr("--output", "HEADLESS-1", "--on")
            self.vela.query("state") # process the enable before checking helpers
            time.sleep(0.2)
            self.assertEqual(helpers(self.vela), {"test_bl": previous})
        finally:
            # SIGTERM can complete once the old writer is allowed to run.
            try:
                os.kill(previous, signal.SIGCONT)
            except ProcessLookupError:
                pass
        self.ready("HEADLESS-1", 0.5)
        wait_helpers(self.vela, lambda h: h.get("test_bl") not in (None, previous),
                     "replacement backlight helper")

    def test_live_sync_follows_the_firmware(self):
        self.ready("HEADLESS-1", 0.5)
        live = Watcher(self.vela)
        live.send("brightness-live on")
        self.write("brightness", 72000) # a key the firmware handles
        self.confirmed("HEADLESS-1", 0.75, timeout=3)
        live.close()


def knobs(image, box):
    """The sliders' knobs in `box` (left, top, right, bottom), as centers:
    accent blobs about 12 pixels across, with no accent 9 pixels above,
    below, or beside their upper half (a filled track is thin, a tile big),
    and the filled track coming from their left (not an accent icon)."""
    def accent(x, y):
        r, g, b = image.at(x, y)[:3]
        return b > 200 and b > r + 40
    left, top, right, bottom = box
    points = [(x, y) for y in range(top + 9, bottom - 9) for x in range(left + 9, right - 9)
              if accent(x, y - 5) and accent(x, y + 5) and not accent(x, y - 9) and not accent(x, y + 9)
              and not accent(x - 9, y - 5) and not accent(x + 9, y - 5)]
    groups = []
    for x, y in points:
        for group in groups:
            if abs(group[0][0] - x) <= 12 and abs(group[0][1] - y) <= 12:
                group.append((x, y))
                break
        else:
            groups.append([(x, y)])
    centers = [(round(sum(p[0] for p in g) / len(g)), round(sum(p[1] for p in g) / len(g))) for g in groups]
    return [(x, y) for x, y in centers if accent(x - 14, y) and not accent(x - 14, y - 5)]


class SettingsProbe(BrightnessTest):
    # Asleep past the helper's own probes (at 0, 2 and 10 s): only Settings'
    # probe brings it back before the next one, at 30 s (§5.5, §8).
    monitors = """
[HEADLESS-1]
value = 60
asleep = 13000
"""

    def test_the_display_page_probes_again(self):
        wait_brightness(self.vela, lambda o: o.get("HEADLESS-1", {}).get("state") == "no-answer", "no-answer")
        time.sleep(max(0.0, self.started + 13.5 - time.monotonic()))
        self.assertEqual(outputs(self.vela)["HEADLESS-1"]["state"], "no-answer")
        env = dict(self.vela.client_env)
        env.pop("QT_WAYLAND_RECONNECT", None)
        settings = subprocess.Popen([os.path.join(BUILD, "settings", "vela-settings"), "--page", "display"], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.vela.clients.append(settings)
        self.ready(value=0.6)
        self.assertLess(time.monotonic() - self.started, 29)


class QuickSettings(test_shell.RealShell):
    """The quick settings slider and page (docs/brightness.md §8), in the real
    shell on fake monitors."""

    QS = "vela-quick-settings"
    monitors = MONITORS
    settled_layer = test_shell.QtShell.settled_layer
    layer = staticmethod(test_shell.QtShell.layer)

    def setUp(self):
        self.fake = fake_file(self.monitors)
        self.addCleanup(os.unlink, self.fake)
        self.compositor_env = {"VELA_BRIGHTNESS_FAKE": self.fake}
        self.started = time.monotonic()
        super().setUp()

    def open_panel(self, *actions):
        """Opens the panel and does `actions` in it, in one vela-input: a new
        one's keyboard takes the focus, and the panel closes."""
        if self.mapped(self.vela.state(), self.QS):
            self.vela.input("key", "Escape")
            self.vela.wait_for(lambda s: not self.mapped(s, self.QS), what="Quick Settings closed")
        self.vela.input("move", 400, 400, "key", "super+a", "sleep", 1200, *actions)
        layer = self.settled_layer(self.QS)
        # The panel, in output pixels (scale 1): its window has 46 units of
        # room for the shadow, and 12 at the right and bottom.
        return (layer["x"] + 46, layer["y"] + 46, layer["x"] + layer["w"] - 12, layer["y"] + layer["h"] - 12)

    def knobs(self, box):
        path = os.path.join(self.vela.directory, "panel.png")
        self.vela.screenshot(path)
        return sorted(knobs(Image.read(path), box), key=lambda k: k[1])

    def settled(self, name, condition, what):
        return wait_brightness(self.vela, lambda o: not o[name]["pending"] and condition(o[name]["value"]), what,
                               timeout=15)[name]["value"]

    def test_the_slider_moves_the_display(self):
        wait_brightness(self.vela, lambda o: o.get("HEADLESS-1", {}).get("state") == "ready", "ready")
        box = self.open_panel()
        x0, y0 = self.knobs(box)[0] # the brightness, above the volume
        # A click on the track: the display goes there.
        self.open_panel("move", x0 - 100, y0, "sleep", 300, "click", "sleep", 300)
        v1 = self.settled("HEADLESS-1", lambda v: v < 0.6, "lower after the click")
        x1, y1 = self.knobs(box)[0]
        self.assertEqual(y1, y0)
        self.assertAlmostEqual(x1, x0 - 100, delta=3)
        per_unit = 100 / (0.7 - v1)
        # A drag: the display ends where the knob was let go.
        self.open_panel("move", x1, y1, "sleep", 300, "down", "move", x1 - 20, y1, "sleep", 50,
                        "move", x1 - 40, y1, "sleep", 50, "move", x1 - 60, y1, "sleep", 100, "up", "sleep", 300)
        v2 = self.settled("HEADLESS-1", lambda v: v < v1 - 0.1, "lower after the drag")
        self.assertAlmostEqual(v2, v1 - 60 / per_unit, delta=0.02)
        x2, _ = self.knobs(box)[0]
        self.assertAlmostEqual(x2, x1 - 60, delta=3)

    def test_the_page_has_a_slider_per_display(self):
        self.vela.command("test-output add 1920x1080")
        wait_brightness(self.vela, lambda o: all(o.get(n, {}).get("state") == "ready"
                                                 for n in ("HEADLESS-1", "HEADLESS-2")), "both ready")
        # The shell has its taskbar on the new display too.
        self.vela.wait_for(lambda s: sum(l["namespace"] == "vela-taskbar" and l["mapped"] for l in s["layers"]) == 2,
                           what="a taskbar on each display")
        time.sleep(0.5)
        box = self.open_panel()
        x0, y0 = self.knobs(box)[0]
        # The arrow at the right of this display's slider opens the page.
        arrow = ("move", box[2] - 34, y0, "sleep", 300, "click", "sleep", 1000)
        page_box = self.open_panel(*arrow)
        sliders = self.knobs(page_box)
        self.assertEqual(len(sliders), 2, sliders)
        (a, ay), (b, by) = sliders
        self.assertGreater(a, b + 20) # 70% above 50%
        # COSMIC #150: the second slider moves the second display only.
        self.open_panel(*arrow, "move", b - 60, by, "sleep", 300, "click", "sleep", 300)
        self.settled("HEADLESS-2", lambda v: v < 0.45, "HEADLESS-2 lower")
        self.assertEqual(outputs(self.vela)["HEADLESS-1"]["seq"], 0)


class QuickSettingsLive(QuickSettings):
    # The monitor's buttons, ms after its helper starts.
    monitors = """
[HEADLESS-1]
value = 70
buttons = 7000:30, 15000:90
"""

    # While the panel is open the slider follows the monitor's buttons; once
    # it closes, nobody reads the monitor.
    def test_the_slider_follows_the_buttons_while_open(self):
        box = self.open_panel()
        self.assertLess(time.monotonic() - self.started, 6.5, "the shell took too long to start")
        before = self.knobs(box)[0]
        wait_brightness(self.vela, lambda o: abs(o["HEADLESS-1"]["value"] - 0.3) < 1e-6, "0.3 from the buttons",
                        timeout=10)
        time.sleep(0.3)
        after = self.knobs(box)[0]
        self.assertLess(after[0], before[0] - 50)
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, self.QS), what="closed")
        time.sleep(max(0.0, self.started + 17.5 - time.monotonic()))
        self.assertAlmostEqual(outputs(self.vela)["HEADLESS-1"]["value"], 0.3)

    test_the_slider_moves_the_display = None
    test_the_page_has_a_slider_per_display = None


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
