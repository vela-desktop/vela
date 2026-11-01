# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The actual Qt shell: panels, mixed-output coordinates, selection and snips.

Fake shell sockets and vela-panel cannot exercise Qt's layer-surface lifecycle.
Run the real shell on a private session bus and isolated configuration.
"""

from contextlib import contextmanager
import json
import os
import select
import re
import shutil
import socket
import subprocess
import time
import unittest

from harness import BUILD, Image, Session, tool

SHELL = os.path.join(BUILD, "shell", "vela-shell")
OUTPUT_CASES = (("normal", 1, "above"), ("90", 1, "above"), ("180", 1.25, "right"),
                ("270", 1.5, "left"), ("flipped", 1, "below"), ("flipped-90", 1.25, "right"),
                ("flipped-180", 1.5, "left"), ("flipped-270", 1, "below"))


GRADIENT = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
            '<linearGradient id="g" x2="1" y2="1">'
            '<stop stop-color="#15324a"/><stop offset="1" stop-color="#da805a"/>'
            '</linearGradient><path fill="url(#g)" d="M0 0H100V100H0Z"/></svg>')


@unittest.skipUnless(os.path.exists(SHELL), "vela-shell not built (VELA_BUILD_SHELL=OFF)")
class RealShell(unittest.TestCase):
    """The real Qt desktop on a headless output: its setup, no tests."""

    wallpaper = GRADIENT
    compositor_env = {} # more for the compositor (fake monitors, test_brightness.py)

    def setUp(self):
        self.bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.addCleanup(self.stop_bus)
        address = self.bus.stdout.readline().strip()
        self.assertTrue(address, "private D-Bus session failed to start")
        self.vela = Session(size="1920x1080@100", env={
            "VELA_SYNC_FILE": os.environ.get("VELA_QT_SYNC_FILE", "0"), "DBUS_SESSION_BUS_ADDRESS": address,
            "VELA_DEBUG_ANIMATION": "1", **self.compositor_env})
        self.vela.start()
        self.addCleanup(self.vela.stop)
        # Snips and desktop files must stay in this isolated test session.
        pictures = os.path.join(self.vela.directory, "Pictures")
        desktop = os.path.join(self.vela.directory, "Desktop")
        os.makedirs(pictures)
        os.makedirs(desktop)
        os.makedirs(self.vela.env["XDG_CONFIG_HOME"], exist_ok=True)
        # Config watches this directory for atomic preference updates.
        os.makedirs(os.path.join(self.vela.env["XDG_CONFIG_HOME"], "Vela"), exist_ok=True)
        with open(os.path.join(self.vela.env["XDG_CONFIG_HOME"], "user-dirs.dirs"), "w") as file:
            file.write(f'XDG_PICTURES_DIR="{pictures}"\nXDG_DESKTOP_DIR="{desktop}"\n')
        wallpaper = os.path.join(self.vela.directory, "wallpaper.svg")
        with open(wallpaper, "w") as file:
            file.write(self.wallpaper)
        env = dict(self.vela.client_env, QT_QPA_PLATFORM="wayland",
                   QT_WAYLAND_SHELL_INTEGRATION="layer-shell", QT_WAYLAND_RECONNECT="0",
                   QT_FORCE_STDERR_LOGGING="1", QSG_RENDER_LOOP="threaded", QSG_RHI_BACKEND="opengl",
                   WAYLAND_DEBUG="client", VELA_WALLPAPER=wallpaper,
                   VELA_DEBUG_SELECTION="1")
        self.shell = subprocess.Popen([SHELL], env=env, stdout=self.vela.log, stderr=self.vela.log)
        self.vela.clients.append(self.shell)
        self.vela.wait_for(lambda s: self.mapped(s, "vela-taskbar") and self.mapped(s, "vela-wallpaper"),
                           timeout=20, what="the real Qt desktop")

    def stop_bus(self):
        self.bus.terminate()
        try:
            self.bus.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.bus.kill()
            self.bus.wait(timeout=3)
        self.bus.stdout.close()

    @staticmethod
    def mapped(state, namespace):
        return any(layer["namespace"] == namespace and layer["mapped"] for layer in state["layers"])

    def check_desktop(self):
        self.assertIsNone(self.shell.poll(), self.vela.log_text()[-4000:])
        state = self.vela.state()
        self.assertTrue(self.mapped(state, "vela-taskbar"))
        self.assertTrue(self.mapped(state, "vela-wallpaper"))
        self.assertNotIn("layer_surface has never been configured", self.vela.log_text())

    def slow_animations(self):
        """animation-duration=300 (Settings' slider), so captures land inside
        the animations."""
        os.makedirs(os.path.dirname(self.vela.config_path()), exist_ok=True)
        with open(self.vela.config_path(), "w") as conf:
            conf.write("animation-duration=300\n")
        self.vela.command("reload-config")
        time.sleep(1.0) # the shell sees the file

    def background_input(self, *actions):
        """vela-input running while the test watches: one virtual keyboard
        for the whole sequence (a new one takes the focus from the panels)."""
        process = subprocess.Popen([tool("vela-input"), *map(str, actions)], env=self.vela.client_env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(process.wait)
        return process

    @staticmethod
    def layer(state, namespace):
        return next((layer for layer in state["layers"] if layer["namespace"] == namespace and layer["mapped"]), None)

    def shell_command(self, line):
        """A line on the shell's own socket, as Files sends them
        ("properties [...]")."""
        path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), f"vela-shell-{self.vela.display}.sock")
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(path)
            connection.sendall(line.encode() + b"\n")


@unittest.skipUnless(os.path.exists(SHELL), "vela-shell not built (VELA_BUILD_SHELL=OFF)")
class QtShell(RealShell):
    def secondary_output(self, transform="90", scale=1, side="above", size=(1920, 1080)):
        """Configure a real Qt desktop at any side of the main output. Above or
        left of it means a negative position: the compositor then moves the
        layout's origin back to (0, 0), and the main output with it."""
        if not any(o["name"] == "HEADLESS-2" for o in self.vela.state()["outputs"]):
            self.vela.command(f"test-output add {size[0]}x{size[1]}")
            self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="secondary output")
        swapped = "90" in transform or "270" in transform
        width, height = size[::-1] if swapped else size
        width, height = int(width / scale), int(height / scale)
        main = next(o for o in self.vela.state()["outputs"] if o["name"] == "HEADLESS-1")
        x, y = {"above": (0, -height), "left": (-width, 0),
                "right": (main["w"], 0), "below": (0, main["h"])}[side]
        x, y = main["x"] + x, main["y"] + y
        self.vela.randr("--output", "HEADLESS-2", "--mode", f"{size[0]}x{size[1]}",
                        "--scale", str(scale), "--pos", f"{x},{y}",
                        "--transform", transform)
        state = self.vela.wait_for(
            lambda s: any(layer["namespace"] == "vela-wallpaper" and layer["mapped"]
                          and layer["output"] == "HEADLESS-2" and layer["w"] == width
                          and layer["h"] == height for layer in s["layers"]),
            what="secondary Qt wallpaper configured")
        return next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")

    def settled_wallpaper(self, output=1):
        """Wait for the asynchronous wallpaper load and opacity animation to finish."""
        last, stable_since = None, time.monotonic()

        def settled(_):
            nonlocal last, stable_since
            image = self.vela.pixels(output=output)
            samples = {image.at(image.width * x // 4, image.height * y // 4)
                       for x in (1, 2, 3) for y in (1, 2, 3)}
            if last is None or image.rows != last.rows or len(samples) == 1:
                stable_since = time.monotonic()
            last = image
            return time.monotonic() - stable_since >= 0.3

        self.vela.wait_for(settled, timeout=8, read_state=False, what="wallpaper loaded and stationary")
        return last


    def test_right_click_dismisses_the_hovered_notification(self):
        """Right-click closes only the hovered toast without invoking its default action."""
        if not shutil.which("gdbus"):
            self.skipTest("gdbus required for desktop notification")

        # Monitor the freedesktop protocol so an accidental default-action
        # invocation can't pass merely because it also closes the popup.
        signal_log = os.path.join(self.vela.directory, "notification-signals.log")
        with open(signal_log, "w+") as log:
            monitor = subprocess.Popen([
                "gdbus", "monitor", "--session", "--dest", "org.freedesktop.Notifications",
                "--object-path", "/org/freedesktop/Notifications",
            ], env=self.vela.client_env, stdout=log, stderr=subprocess.STDOUT)
            try:
                time.sleep(0.3) # let the monitor subscribe before sending
                for summary in ("First toast", "Second toast"):
                    subprocess.run([
                        "gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
                        "--object-path", "/org/freedesktop/Notifications",
                        "--method", "org.freedesktop.Notifications.Notify",
                        "Vela functional test", "0", "", summary,
                        "A right-click should dismiss this notification",
                        "['default', 'Open']", "{}", "120000",
                    ], env=self.vela.client_env, check=True, capture_output=True, text=True, timeout=8)

                state = self.vela.wait_for(
                    lambda s: (p := self.layer(s, "vela-notifications")) and p["h"] > 160,
                    timeout=8, what="two notification popups")
                popup = self.layer(state, "vela-notifications")
                initial_height = popup["h"]
                x, y = popup["x"] + 75, popup["y"] + 55
                time.sleep(0.3) # the toast's entrance animation has finished

                self.vela.input("move", x, y, "sleep", 100, "click", "right")
                self.vela.wait_for(
                    lambda s: (p := self.layer(s, "vela-notifications"))
                    and p["h"] < initial_height - 20,
                    timeout=8, what="only the hovered toast dismissed")

                # The other toast moves into the same place and remains clickable.
                self.vela.input("move", x, y, "sleep", 100, "click", "right")
                self.vela.wait_for(
                    lambda s: not self.mapped(s, "vela-notifications"),
                    timeout=8, what="remaining toast dismissed")
                self.check_desktop()
            finally:
                if monitor.poll() is None:
                    monitor.terminate()
                try:
                    monitor.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    monitor.kill()
                    monitor.wait(timeout=3)

            log.seek(0)
            signals = log.read()
        self.assertGreaterEqual(signals.count("NotificationClosed"), 2, signals)
        self.assertNotIn("ActionInvoked", signals)

    def test_selected_primary_controls_run_dialog_and_notification_remap(self):
        """The real Qt layers follow the selected output across off/on."""
        if not shutil.which("gdbus"):
            self.skipTest("gdbus required for desktop notification")

        self.secondary_output(transform="normal", side="right")
        conf_path = os.path.join(self.vela.env["XDG_CONFIG_HOME"], "Vela", "vela-shell.conf")
        with open(conf_path, "w") as conf:
            conf.write("[display]\nprimaryOutput=HEADLESS-2\n")
        # Allow Config's QFileSystemWatcher/debounce to deliver the update.
        time.sleep(0.5)

        self.vela.keys("super+r")
        self.vela.wait_for(
            lambda s: (layer := self.layer(s, "vela-run")) and layer["output"] == "HEADLESS-2",
            timeout=8, what="Run dialog on explicitly selected output")
        self.vela.keys("Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-run"), what="Run dialog closed")

        subprocess.run([
            "gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications",
            "--object-path", "/org/freedesktop/Notifications",
            "--method", "org.freedesktop.Notifications.Notify",
            "Vela functional test", "0", "", "Hotplug notification",
            "Must remain actionable after remapping", "['default', 'Open']", "{}", "120000",
        ], env=self.vela.client_env, check=True, capture_output=True, text=True, timeout=8)
        self.vela.wait_for(
            lambda s: (layer := self.layer(s, "vela-notifications")) and layer["output"] == "HEADLESS-2",
            timeout=8, what="popup on explicitly selected output")

        self.vela.randr("--output", "HEADLESS-2", "--off")
        self.vela.wait_for(
            lambda s: (layer := self.layer(s, "vela-notifications")) and layer["output"] == "HEADLESS-1",
            timeout=8, what="existing popup remapped to fallback")
        self.vela.randr("--output", "HEADLESS-2", "--on")
        state = self.vela.wait_for(
            lambda s: (layer := self.layer(s, "vela-notifications")) and layer["output"] == "HEADLESS-2",
            timeout=8, what="existing popup remapped back to preferred")
        popup = self.layer(state, "vela-notifications")
        # The existing QML delegate must retain its default action after remap.
        self.vela.input("move", popup["x"] + 75, popup["y"] + 55,
                        "sleep", 100, "click", "left")
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-notifications"),
                           timeout=8, what="notification action dismissed the popup")
        self.check_desktop()

    def test_desktop_selection_matches_coordinates_on_transformed_outputs(self):
        """Growing, shrinking and releasing the band must preserve its coordinates on every transform."""
        for transform, scale, side in OUTPUT_CASES:
            with self.subTest(transform=transform, scale=scale, side=side):
                output = self.secondary_output(transform, scale, side)
                sx, sy = 120, 160
                # Avoid a half-physical-pixel endpoint in the strict framebuffer
                # oracle: at 125% scaling, y=950 maps to 1187.5 physical
                # pixels, which rasterizes to 1187 on RADV and 1188 on NVIDIA
                # for flipped-90. y=948 maps exactly to 1185 on both, keeping
                # the zero-tolerance transformed-output assertion meaningful.
                # A separate rasterization test is needed for half-pixel ties.
                ends = ((500, min(948, output["h"] - 120)), (330, min(560, output["h"] - 240)))
                self.vela.input("move", output["x"] + sx, output["y"] + sy)
                before = self.settled_wallpaper()
                trace_offset = len(self.vela.log_text())
                # Keep one virtual pointer alive throughout the complete drag,
                # including snapshots while its button is held.
                # Allow Qt to process device creation/focus before the press,
                # then the press before motion (as with an existing physical mouse).
                actions = ["move", output["x"] + sx, output["y"] + sy, "sleep", 100,
                           "down", "sleep", 100]
                for ex, ey in ends:
                    # Keep the virtual button down at each endpoint until the
                    # test confirms exactly that rendered selection. Previously
                    # a fixed sleep let the pointer reach the next endpoint or
                    # release while we were still polling a screenshot.
                    actions += ["move", output["x"] + ex, output["y"] + ey, "checkpoint"]
                actions += ["up", "move", output["x"] + sx, output["y"] + sy]
                with self.held_input(actions, checkpoints=True) as pointer:
                    # GPU traces show Qt/QML receives the requested logical
                    # coordinates unchanged, while the composited rectangle
                    # uses truncation at fractional physical-pixel boundaries
                    # (e.g. 330 * 1.25 -> 412, 950 * 1.25 -> 1187).
                    # Preserve exact bounds and interior pixel assertions; fix
                    # only the test's inconsistent round-to-nearest oracle.
                    px = before.width / output["w"]
                    py = before.height / output["h"]

                    def edge_x(x):
                        return int(x * px)

                    def edge_y(y):
                        return int(y * py)
                    for ex, ey in ends:
                        ready, _, _ = select.select([pointer.stdout], [], [], 5)
                        self.assertTrue(ready, f"virtual pointer did not reach {ex, ey} checkpoint")
                        self.assertEqual(pointer.stdout.readline(), b"READY\n",
                                         "virtual pointer checkpoint failed")
                        rectangle = (edge_x(sx), edge_y(sy), edge_x(ex), edge_y(ey))
                        observed = None

                        def correctly_drawn(_):
                            nonlocal observed
                            image = self.vela.pixels(output=1)
                            # Exact band edges; omit the live taskbar clock.
                            desktop_height = image.height - edge_y(48)
                            observed = image.changed_bounds(before, height=desktop_height)
                            return (observed == rectangle
                                    and all(image.at(edge_x(x), edge_y(y))
                                            != before.at(edge_x(x), edge_y(y))
                                            for x, y in ((sx + 40, sy + 40), (ex - 40, ey - 40))))

                        try:
                            self.vela.wait_for(correctly_drawn, timeout=4, read_state=False,
                                               what=f"desktop selection bounds {rectangle}")
                        except (AssertionError, TimeoutError) as error:
                            # wait_for raises TimeoutError on pixel mismatches:
                            # include the observed framebuffer bounds and input
                            # trace rather than silently losing both on timeout.
                            trace = [
                                line for line in self.vela.log_text()[trace_offset:].splitlines()
                                if "VELA_SELECTION " in line
                            ]
                            self.fail(f"{error}; observed bounds={observed}, "
                                      f"transform={transform}, scale={scale}, output={output}, "
                                      f"framebuffer={before.width}x{before.height}, "
                                      f"requested drag=({sx},{sy})->({ex},{ey}); "
                                      "Qt/QML selection trace:\n" + "\n".join(trace[-24:]))
                        pointer.stdin.write(b"\n")
                        pointer.stdin.flush()
                desktop_height = before.height - edge_y(48)
                self.vela.wait_for(lambda _: self.vela.pixels(output=1).rows[:desktop_height]
                                   == before.rows[:desktop_height],
                                   timeout=4, read_state=False, what="selection erased after release")

    def test_snipping_tool_crops_upright_pixels_on_transformed_outputs(self):
        """Both shortcuts save upright pixels for all eight transforms and four layout sides."""
        for transform, scale, side in OUTPUT_CASES:
            output = self.secondary_output(transform, scale, side)
            self.vela.input("move", 100, 100)
            before = self.settled_wallpaper()
            for shortcut in ("Print", "super+shift+s"):
                with self.subTest(shortcut=shortcut, transform=transform, scale=scale, side=side):
                    self.check_snip(output, before, shortcut)

    def test_software_cursor_damage_uses_upright_coordinates(self):
        """A moving software cursor must update both its old and new buffer regions."""
        for transform in ("90", "270", "flipped-90", "flipped-270"):
            with self.subTest(transform=transform):
                output = self.secondary_output(transform, 1, "above")
                # Qt's wallpaper owns the pointer shape. Force Vela's software
                # cursor with a shader filter, then capture WITHOUT PAINT_CURSORS.
                self.vela.command("color-filter on")
                self.vela.input("move", output["x"] + 60, output["y"] + 90, "sleep", 150)
                before = self.settled_wallpaper()
                first, second = (240, 380), (600, 780)

                def patch(image, position):
                    x, y = position
                    return tuple(row[(x - 8) * 3:(x + 40) * 3]
                                 for row in image.rows[y - 8:y + 40])

                self.vela.input("move", output["x"] + first[0], output["y"] + first[1], "sleep", 150)
                self.vela.wait_for(
                    lambda _: patch(self.vela.pixels(output=1), first) != patch(before, first),
                    read_state=False, timeout=4, what="composited cursor visible at new position")
                self.vela.input("move", output["x"] + second[0], output["y"] + second[1], "sleep", 150)

                def moved_and_erased(_):
                    image = self.vela.pixels(output=1)
                    return (patch(image, first) == patch(before, first)
                            and patch(image, second) != patch(before, second))

                self.vela.wait_for(moved_and_erased, read_state=False, timeout=4,
                                   what="software cursor erased and redrawn on rotated output")

    def test_cursor_remains_at_the_pointer_coordinates_on_every_transform(self):
        """wlroots' cursor must agree with Qt surface pixels, not an inverted capture oracle."""
        expected = None
        path = os.path.join(self.vela.directory, "with-cursor.png")
        for transform, _, side in OUTPUT_CASES:
            with self.subTest(transform=transform, side=side):
                output = self.secondary_output(transform, 1, side)
                self.vela.input("move", 100, 100)
                before = self.settled_wallpaper()
                self.vela.input("move", output["x"] + 200, output["y"] + 300, "sleep", 100)

                def cursor_at_pointer(_):
                    self.vela.screenshot(path, output=1, cursor=True)
                    image = Image.read(path)
                    bounds = image.changed_bounds(before, height=image.height - 96)
                    return (bounds is not None and bounds[0] >= 180 and bounds[1] >= 280
                            and bounds[2] <= 250 and bounds[3] <= 350)

                self.vela.wait_for(cursor_at_pointer, timeout=3, read_state=False,
                                   what="cursor drawn at its actual upright pointer coordinates")
                image = Image.read(path)
                white = {(x, y) for y in range(280, 350) for x in range(180, 250)
                         if image.at(x, y) == (255, 255, 255)}
                self.assertTrue(white, "the upright cursor's opaque white pixels")
                if expected is None:
                    expected = (image.changed_bounds(before, height=image.height - 96), white)
                else:
                    self.assertEqual((image.changed_bounds(before, height=image.height - 96), white), expected,
                                     "cursor position and shape changed with output orientation")

    def check_snip(self, output, before, shortcut):
        """Drag a secondary-screen crop and compare the saved PNG with its original pixels."""
        self.vela.keys(shortcut)
        self.vela.wait_for(lambda s: {layer["output"] for layer in s["layers"]
                                    if layer["namespace"] == "vela-snip" and layer["mapped"]}
                           == {"HEADLESS-1", "HEADLESS-2"}, timeout=4,
                           what="snipping overlays on both outputs")
        self.settled_wallpaper() # The first Qt overlay frame must finish before dragging.
        pictures = os.path.join(self.vela.directory, "Pictures", "Screenshot")
        previous = {path: os.stat(os.path.join(pictures, path)).st_ino
                    for path in os.listdir(pictures)} if os.path.isdir(pictures) else {}
        # Odd edges expose independent origin/width rounding at 125% and 150%.
        x1, y1, x2, y2 = 121, 161, 522, 423
        self.vela.input("move", output["x"] + x1, output["y"] + y1, "sleep", 100, "down",
                        "sleep", 100, "move", output["x"] + x2, output["y"] + y2,
                        "sleep", 200, "up")
        self.vela.wait_for(lambda s: not any(layer["namespace"] == "vela-snip" and layer["mapped"]
                                           for layer in s["layers"]), what="snipping completed")
        paths = [os.path.join(pictures, name) for name in os.listdir(pictures)
                 if name.endswith(".png") and previous.get(name) != os.stat(os.path.join(pictures, name)).st_ino]
        self.assertEqual(len(paths), 1, "one snip saved to the isolated Pictures directory")
        cut = Image.read(paths[0])
        # Qt maps screen logical coordinates to physical image pixels using
        # the actual captured dimensions (which may be odd at fractional scale).
        sx, sy = before.width / output["w"], before.height / output["h"]
        left, right = (int(x * sx + 0.5) for x in (x1, x2))
        top, bottom = (int(y * sy + 0.5) for y in (y1, y2))
        self.assertEqual((cut.width, cut.height), (right - left, bottom - top))
        self.assertEqual(cut.rows, [row[left * 3:right * 3] for row in before.rows[top:bottom]])

    def test_magnifier_keeps_portrait_selection_and_cursor_aligned(self):
        """Zoomed Qt selection and the wlroots cursor must indicate the same portrait pixels."""
        for transform, scale, side in (("90", 1, "above"), ("270", 1.5, "left")):
            with self.subTest(transform=transform, scale=scale):
                output = self.secondary_output(transform, scale, side)
                self.vela.input("move", output["x"] + 120, output["y"] + 160)
                self.vela.command("magnifier on")
                before = self.settled_wallpaper()
                # Zoom starts at 2x and keeps the initial pointer pixel still.
                # The endpoints stay inside that view, so dragging does not pan.
                left, top, right, bottom = (int(v * scale) for v in (120, 160, 540, 960))
                # A freshly created virtual pointer has no Qt hover/focus
                # until it moves. Establish it on the actual wallpaper before
                # the press; otherwise the band may never start, especially
                # with a transformed output or magnification enabled.
                actions = ["move", output["x"] + 120, output["y"] + 160,
                           "sleep", 100, "down", "sleep", 100,
                           "move", output["x"] + 330, output["y"] + 560,
                           "sleep", 2500, "up"]
                try:
                    with self.held_input(actions):
                        last_bounds = None
                        def aligned(_):
                            nonlocal last_bounds
                            path = os.path.join(self.vela.directory, "magnified-selection.png")
                            self.vela.screenshot(path, output=1, cursor=True)
                            image = Image.read(path)
                            margin = int(40 * scale)
                            bounds = image.changed_bounds(before, height=image.height - int(96 * scale))
                            last_bounds = bounds
                            # At 2x, Catmull-Rom's four-texel footprint reaches
                            # three physical pixels beyond the band, even at 150%.
                            if (bounds is None or abs(bounds[0] - left) > 3 or abs(bounds[1] - top) > 3
                                    or bounds[2] > right + margin or bounds[3] > bottom + margin):
                                return False
                            band = all(image.at(x, y) != before.at(x, y)
                                       for x, y in ((left + margin, top + margin), (right - margin, bottom - margin)))
                            cursor = any(image.at(x, y) == (255, 255, 255)
                                         for y in range(bottom - margin, bottom + margin)
                                         for x in range(right - margin, right + margin))
                            return band and cursor

                        try:
                            self.vela.wait_for(aligned, timeout=2, read_state=False,
                                               what="magnified selection and cursor aligned")
                        except AssertionError as error:
                            self.fail(f"{error}\nChanged bounds: {last_bounds}; band: {(left, top, right, bottom)}")
                finally:
                    self.vela.command("magnifier off")
                    # Reopened while still zooming out, it would keep the
                    # cursor's place on screen, not the pointer's point.
                    time.sleep(0.5)

    @contextmanager
    def held_input(self, actions, checkpoints=False):
        """Keep a virtual button pressed until each screenshot has been checked."""
        pointer = subprocess.Popen([tool("vela-input"), *map(str, actions)], env=self.vela.client_env,
                                   stdin=subprocess.PIPE if checkpoints else None,
                                   stdout=subprocess.PIPE if checkpoints else None)
        failed = False
        try:
            yield pointer
        except BaseException:
            failed = True
            raise
        finally:
            if checkpoints:
                # EOF unblocks a client paused on checkpoint after a failed
                # image assertion. Preserve the original assertion failure.
                pointer.stdin.close()
            try:
                pointer.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pointer.kill()
                pointer.wait()
            if checkpoints:
                pointer.stdout.close()
            if not failed:
                self.assertEqual(pointer.returncode, 0)

    def test_snipping_with_odd_and_4k_output_dimensions(self):
        """Capture sizes need not divide evenly by the fractional scale."""
        for size, transform, scale in (((1367, 769), "90", 1.25), ((3840, 2160), "270", 2)):
            with self.subTest(size=size, transform=transform, scale=scale):
                output = self.secondary_output(transform, scale, "left", size=size)
                self.vela.input("move", 100, 100)
                before = self.settled_wallpaper()
                for shortcut in ("Print", "super+shift+s"):
                    self.check_snip(output, before, shortcut)

    def test_settings_rotation_keeps_upper_taskbar_out_of_main_output(self):
        """Rotate only the upper screen through Settings, with the real Qt desktop mapped."""
        settings = os.path.join(BUILD, "settings", "vela-displays-test")
        self.assertTrue(os.path.exists(settings), "native Settings test client missing")
        # Match the reported layout: 2560x1440 at 125%, with a 1080p monitor
        # immediately above it. Settings must choose the new Y itself.
        self.vela.randr("--output", "HEADLESS-1", "--mode", "2560x1440", "--scale", "1.25", "--pos", "0,0")
        self.vela.command("test-output add 1920x1080")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="upper monitor connected")
        self.vela.randr("--output", "HEADLESS-2", "--scale", "1", "--pos", "0,-1080")
        self.vela.wait_for(lambda s: sum(layer["namespace"] == "vela-taskbar" and layer["mapped"]
                                       for layer in s["layers"]) == 2, timeout=15,
                           what="a real taskbar on each monitor")
        window = self.vela.open_window()
        self.vela.keys("super+up")
        self.vela.wait_for(lambda s: s.window(window)["maximized"], what="main-output pattern maximized")
        self.vela.wait_still(window)
        before = self.vela.pixels(output=0)
        # Above the main output: the layout starts at the upper monitor, so
        # the main one sits right below it, at its height.
        for transform, size in (("90", (1080, 1920)), ("270", (1080, 1920)), ("normal", (1920, 1080))):
            with self.subTest(transform=transform):
                result = subprocess.run([settings, "--apply-client", self.vela.display, "HEADLESS-2",
                                         json.dumps({"transform": transform})], env=self.vela.client_env,
                                        capture_output=True, text=True, timeout=7)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                state = self.vela.state() # The native client exits only after server confirmation.
                main, upper = (next(o for o in state["outputs"] if o["name"] == name)
                               for name in ("HEADLESS-1", "HEADLESS-2"))
                self.assertEqual((upper["x"], upper["y"], upper["w"], upper["h"]), (0, 0, *size))
                self.assertEqual(upper["y"] + upper["h"], main["y"])
                self.assertEqual((main["x"], main["y"], main["w"], main["h"]), (0, size[1], 2048, 1152))
                def layers_placed(s):
                    layers = {layer["namespace"]: layer for layer in s["layers"]
                              if layer["mapped"] and layer["output"] == "HEADLESS-2"}
                    if not {"vela-wallpaper", "vela-taskbar"} <= layers.keys():
                        return False
                    wallpaper, taskbar = layers["vela-wallpaper"], layers["vela-taskbar"]
                    return ((wallpaper["x"], wallpaper["y"], wallpaper["w"], wallpaper["h"])
                            == (0, 0, *size)
                            and (taskbar["x"], taskbar["y"], taskbar["w"], taskbar["h"])
                            == (0, size[1] - 48, size[0], 48))

                self.vela.wait_for(layers_placed, what="both upper layers confined to their own monitor")
                after = self.vela.pixels(output=0)
                # Exclude the main taskbar clock and window edges; an upper
                # taskbar at the old Y would cut directly through these rows.
                self.assertEqual(before.rows[64:-96], after.rows[64:-96],
                                 "upper-monitor contents leaked into the main monitor")
                self.check_desktop()

    def test_minimize_flies_to_the_button_on_its_own_output(self):
        """Every output has a taskbar with the window's button: minimizing (and
        restoring) flies to and from the one on the window's own output."""
        self.vela.command("test-output add 1920x1080")
        self.vela.wait_for(lambda s: sum(layer["namespace"] == "vela-taskbar" and layer["mapped"]
                                         for layer in s["layers"]) == 2, timeout=15, what="two taskbars")
        window = self.vela.open_window(400, 300)
        for name in ("HEADLESS-1", "HEADLESS-2"):
            with self.subTest(output=name):
                state = self.vela.state()
                if state.window(window)["output"] != name:
                    w = state.window(window)
                    target = next(o for o in state["outputs"] if o["name"] == name)
                    sx, sy = w["x"] + 50, w["y"] + 50
                    tx, ty = target["x"] + target["w"] // 2, target["y"] + 300
                    self.vela.input("move", sx, sy, "keydown", "super", "down", "sleep", "50",
                                    "move", (sx + tx) // 2, (sy + ty) // 2, "sleep", "50",
                                    "move", tx, ty, "sleep", "100", "up", "keyup", "super")
                    self.vela.wait_for(lambda s: s.window(window)["output"] == name, what=f"window on {name}")
                    self.vela.wait_still(window)
                output = next(o for o in self.vela.state()["outputs"] if o["name"] == name)
                for action in ("minimize", "restore"):
                    mark = len(self.vela.log_text())
                    self.vela.command(f"window {window} {action}")
                    self.vela.wait_for(lambda _: f"animation: {action} #" in self.vela.log_text()[mark:]
                                       and " retired " in self.vela.log_text()[mark:], read_state=False,
                                       what=f"{action} on {name}")
                    flight = [tuple(float(v) for v in m.groups()) for m in re.finditer(
                        rf"animation: {action} #\d+ geometry ([-\d.]+) ([-\d.]+)", self.vela.log_text()[mark:])]
                    button = flight[-1] if action == "minimize" else flight[0]
                    self.assertTrue(output["x"] <= button[0] < output["x"] + output["w"]
                                    and output["y"] + output["h"] - 48 <= button[1] < output["y"] + output["h"],
                                    f"{action} on {name}: the button at {button}, outside {output}")

    def test_a_qt_app_animates_with_its_own_images(self):
        """Settings (Qt, drawn on the GPU) through Vela's title bar and its taskbar
        button: every animation starts, and its snapshots hold the app's images."""
        env = dict(self.vela.client_env)
        env.pop("QT_WAYLAND_RECONNECT", None)
        settings = subprocess.Popen([os.path.join(BUILD, "settings", "vela-settings")], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.vela.clients.append(settings)
        state = self.vela.wait_for(lambda s: any(w["app"] == "vela-settings" for w in s.windows), timeout=15,
                                   what="Settings")
        window = next(w["id"] for w in state.windows if w["app"] == "vela-settings")
        self.vela.wait_still(window)
        time.sleep(1.0) # its first frames

        def act(name, *actions):
            mark = len(self.vela.log_text())
            self.vela.input(*actions, "sleep", 400)
            self.vela.wait_for(lambda _: re.search(rf"animation: {name} #\d+ retired", self.vela.log_text()[mark:]),
                               read_state=False, what=name)
            text = self.vela.log_text()[mark:]
            surfaces = [int(m[1]) for m in re.finditer(rf"animation: {name} #\d+ geometry .* surfaces (\d+)", text)]
            self.assertTrue(surfaces and min(surfaces) > 0, f"{name}: the snapshot lacks Settings' images")
            return text

        def button(part):
            w = self.vela.state().window(window)
            return w["x"] + w["w"] - {"close": 23, "maximize": 69, "minimize": 115}[part], w["y"] + 15

        act("morph", "move", *button("maximize"), "sleep", 300, "down", "sleep", 30, "up")
        act("morph", "move", *button("maximize"), "sleep", 300, "down", "sleep", 30, "up")
        text = act("minimize", "move", *button("minimize"), "sleep", 300, "down", "sleep", 30, "up")
        x, y = (float(v) for v in re.findall(r"animation: minimize #\d+ geometry ([-\d.]+) ([-\d.]+)", text)[-1])
        act("restore", "move", int(x), int(y), "sleep", 300, "down", "sleep", 30, "up") # its taskbar button
        self.assertFalse(self.vela.state().window(window)["minimized"])
        act("close", "move", *button("close"), "sleep", 300, "down", "sleep", 30, "up")

    def test_settings_bar_continues_its_content(self):
        """docs/visual-coherence.md §6.6, D8: the shell sends the bars' Mica
        colors, computed as Vela's apps compute their content's, so the bar
        (the compositor's) and Settings' content (Qt's) meet without a seam,
        tinted by the wallpaper."""
        env = dict(self.vela.client_env)
        env.pop("QT_WAYLAND_RECONNECT", None)
        settings = subprocess.Popen([os.path.join(BUILD, "settings", "vela-settings")], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.vela.clients.append(settings)
        state = self.vela.wait_for(lambda s: any(w["app"] == "vela-settings" for w in s.windows), timeout=15,
                                   what="Settings")
        window = next(w["id"] for w in state.windows if w["app"] == "vela-settings")
        self.vela.wait_still(window)

        def seam(_):
            w = self.vela.state().window(window)
            image = self.vela.pixels()
            x = w["x"] + w["w"] - 3 * 46 - 10 # between the title and the buttons
            bar, content = image.at(x, w["y"] + 16), image.at(x, w["y"] + 32 + 6)
            return bar == content and bar != (32, 32, 32) # the same, and not the untinted gray
        self.vela.wait_for(seam, timeout=8, read_state=False, what="the bar continuing Settings' content")

    def test_panels_animate(self):
        """Start, Quick Settings, the notification center and Task View don't
        just appear (docs/visual-coherence.md K5): at 300% (Settings' slider,
        so a capture lands inside the animation) the panel's first frames still
        show what's behind its top edge."""
        self.slow_animations()
        for name, key, namespace in (("Start", "super", "vela-start-menu"),
                                     ("Quick Settings", "super+a", "vela-quick-settings"),
                                     ("the notification center", "super+n", "vela-notification-center"),
                                     ("Task View", "super+Tab", "vela-task-view")):
            with self.subTest(panel=name):
                before = self.vela.pixels()
                self.vela.input("key", key)
                # Its first frames: an instant panel would already look settled.
                self.vela.wait_for(lambda s: self.mapped(s, namespace), what=f"{name} open")
                early = self.vela.pixels()
                time.sleep(1.5)
                settled = self.vela.pixels()
                box = settled.changed_bounds(before, height=settled.height - 48)
                self.assertIsNotNone(box, f"{name} didn't open")
                # The panel's top band: sliding in or fading, its first frames
                # still show what's behind it there.
                x0, y0, x1, _ = box
                band = [(x, y) for y in range(y0 + 2, y0 + 42, 4) for x in range(x0 + 4, x1 - 4, 8)]
                moving = sum(1 for x, y in band
                             if max(abs(a - b) for a, b in zip(early.at(x, y), settled.at(x, y))) > 24)
                self.assertGreater(moving, len(band) // 4, f"{name} appeared without an animation")
                self.vela.input("key", "Escape")
                self.vela.wait_for(lambda s: not self.mapped(s, namespace), timeout=5, what=f"{name} closed")
                time.sleep(1.5)

    def test_super_repeatedly_opens_and_closes_the_real_menu(self):
        # One virtual keyboard, as on the user's desktop. Creating a fresh
        # keyboard for each key would briefly change focus and close the menu.
        actions = []
        for _ in range(10):
            actions += ["key", "super", "sleep", "400", "key", "super", "sleep", "350"]
        self.vela.input(*actions)
        self.check_desktop()
        self.assertFalse(self.mapped(self.vela.state(), "vela-start-menu"))

    def test_one_input_session_keeps_the_menu_open_between_lines(self):
        """vela-input - (scripts/recording_core.py's InputSession): one
        keyboard answers OK or ERROR per line and leaves the menu open. A
        new vela-input per action closes Start in the recorder's isolated
        session, though not in this one."""
        session = subprocess.Popen([tool("vela-input"), "-"], env=self.vela.client_env,
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

        def send(*actions):
            session.stdin.write("\t".join(actions) + "\n")
            session.stdin.flush()
            return session.stdout.readline().strip()

        try:
            self.assertEqual(send("key", "super"), "OK")
            self.vela.wait_for(lambda s: self.mapped(s, "vela-start-menu"), what="Start open")
            time.sleep(0.5)
            self.assertEqual(send("type", "Files"), "OK")
            self.assertEqual(send("nonsense"), "ERROR")
            time.sleep(0.5)
            self.assertTrue(self.mapped(self.vela.state(), "vela-start-menu"),
                            "a later line of the same session closed Start")
            self.assertEqual(send("key", "Escape"), "OK")
            self.vela.wait_for(lambda s: not self.mapped(s, "vela-start-menu"), timeout=5, what="Start closed")
        finally:
            session.stdin.close()
            status = session.wait(timeout=5)
            session.stdout.close()
        self.assertEqual(status, 1, "a refused line must fail the session")

    def test_reopening_during_close_animation_keeps_the_menu_open(self):
        self.vela.input("key", "super", "sleep", "450", "key", "super", "sleep", "40",
                        "key", "super", "sleep", "600")
        self.check_desktop()
        self.assertTrue(self.mapped(self.vela.state(), "vela-start-menu"),
                        "the previous close animation hid the reopened menu")

    def test_hidden_menu_releases_native_surface_before_next_open(self):
        for _ in range(3):
            offset = len(self.vela.log_text())
            self.vela.input("key", "super", "sleep", "450", "key", "super", "sleep", "350")
            self.check_desktop()
            log = self.vela.log_text()[offset:]
            creation = re.search(r'get_layer_surface\(new id zwlr_layer_surface_v1([@#]\d+), '
                                 r'wl_surface([@#]\d+).*?"vela-start-menu"', log)
            self.assertIsNotNone(creation, "Start layer surface missing from protocol trace")
            role, surface = map(re.escape, creation.groups())
            # Real protocol ordering, including reopening after destruction:
            # a non-null buffer must follow this role's configure and ACK.
            configure = re.search(rf'zwlr_layer_surface_v1{role}\.configure\(', log)
            ack = re.search(rf'zwlr_layer_surface_v1{role}\.ack_configure\(', log)
            attach = re.search(rf'wl_surface{surface}\.attach\(wl_buffer', log)
            self.assertIsNotNone(configure)
            self.assertIsNotNone(ack)
            self.assertIsNotNone(attach)
            self.assertLess(configure.start(), ack.start())
            self.assertLess(ack.start(), attach.start())
            self.assertRegex(log, rf'wl_surface{surface}\.destroy\(\)',
                          "hidden Start menu retained its native surface and EGL buffer queue")
            # Blur must bind to the newly created surface on every reopening,
            # rather than retain hooks to a destroyed Qt platform window.
            effect = re.search(r'get_background_effect\(new id ext_background_effect_surface_v1'
                               rf'([@#]\d+), wl_surface{surface}\)', log)
            self.assertIsNotNone(effect, "reopened menu lost compositor blur")
            self.assertRegex(log, rf'ext_background_effect_surface_v1{re.escape(effect[1])}'
                                 r'\.set_blur_region\(wl_region')

    def test_other_panels_can_reopen_after_native_surface_release(self):
        for key, namespace in [("super+r", "vela-run"), ("super+a", "vela-quick-settings"),
                               ("super+n", "vela-notification-center"), ("super+v", "vela-clipboard")]:
            with self.subTest(panel=namespace):
                for _ in range(3):
                    offset = len(self.vela.log_text())
                    self.vela.input("key", key, "sleep", "450", "key", "Escape", "sleep", "350")
                    self.check_desktop()
                    self.assertFalse(self.mapped(self.vela.state(), namespace))
                    log = self.vela.log_text()[offset:]
                    creation = re.search(r'get_layer_surface\(.*?wl_surface([@#]\d+).*?'
                                         rf'"{namespace}"', log)
                    self.assertIsNotNone(creation, f"{namespace} did not reopen")
                    surface = re.escape(creation[1])
                    self.assertRegex(log, rf'wl_surface{surface}\.attach\(wl_buffer')
                    self.assertRegex(log, rf'wl_surface{surface}\.destroy\(\)')

    # ------------------------------------------------------ Quick Settings --
    # docs/visual-coherence.md §6.9 and §6.10. Its window is the panel with
    # 46 units of room for the shadow, cut 12 units from the output's edge and
    # from the taskbar.

    QS = "vela-quick-settings"
    ROOM = 46

    def panel_box(self, layer):
        """Where the panel rests in its window, in output pixels (scale 1)."""
        return (layer["x"] + self.ROOM, layer["y"] + self.ROOM,
                layer["x"] + layer["w"] - 12, layer["y"] + layer["h"] - 12)

    def test_quick_settings_repeatedly_opens_and_closes(self):
        actions = []
        for _ in range(10):
            actions += ["key", "super+a", "sleep", "400", "key", "super+a", "sleep", "350"]
        self.vela.input(*actions)
        self.check_desktop()
        self.vela.wait_for(lambda s: not self.mapped(s, self.QS), what="Quick Settings closed")

    def test_reopening_quick_settings_during_its_exit_turns_back(self):
        """Reopened while leaving, it turns back: its window never goes away."""
        process = self.background_input("key", "super+a", "sleep", "600", "key", "super+a", "sleep", "60",
                                        "key", "super+a", "sleep", "800")
        self.vela.wait_for(lambda s: self.mapped(s, self.QS), what="Quick Settings open")
        while process.poll() is None:
            self.assertTrue(self.mapped(self.vela.state(), self.QS), "it closed before turning back")
        self.check_desktop()
        self.assertTrue(self.mapped(self.vela.state(), self.QS))

    def test_a_click_in_quick_settings_shadow_closes_it(self):
        """The room around the panel is its window's: a click there closes it
        like a click outside; on the panel's own background it doesn't."""
        self.vela.input("key", "super+a")
        layer = self.settled_layer(self.QS)
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, self.QS), what="Quick Settings closed")
        left, top, right, bottom = self.panel_box(layer)
        # On the panel, in its margin left of the tiles.
        self.vela.input("key", "super+a", "sleep", "900", "move", left + 6, (top + bottom) // 2,
                        "click", "left", "sleep", "700")
        self.assertTrue(self.mapped(self.vela.state(), self.QS), "a click on the panel closed it")
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, self.QS), what="Quick Settings closed")
        # In the shadow's room, left of the panel.
        self.vela.input("key", "super+a", "sleep", "900", "move", layer["x"] + 20, (top + bottom) // 2,
                        "click", "left", "sleep", "700")
        self.vela.wait_for(lambda s: not self.mapped(s, self.QS), timeout=3,
                           what="Quick Settings closed by a click in its shadow")
        self.check_desktop()

    # ---------------------------------------------------- the other panels --
    # docs/visual-coherence.md §6.9: Start, the Notification Center and the
    # clipboard on PanelMotion, like Quick Settings: (key, namespace, units
    # of shadow room left of the panel). Start is centered: room on both
    # sides; the others touch the output's right edge.

    EDGE_PANELS = (("super", "vela-start-menu"), ("super+n", "vela-notification-center"),
                   ("super+v", "vela-clipboard"))

    def test_edge_panels_repeatedly_open_and_close(self):
        for key, namespace in self.EDGE_PANELS[1:]: # Start: test_super_repeatedly_opens_and_closes_the_real_menu
            with self.subTest(panel=namespace):
                actions = []
                for _ in range(10):
                    actions += ["key", key, "sleep", "400", "key", key, "sleep", "350"]
                self.vela.input(*actions)
                self.check_desktop()
                self.vela.wait_for(lambda s: not self.mapped(s, namespace), what=f"{namespace} closed")

    def test_reopening_edge_panels_during_their_exit_turns_back(self):
        """Reopened while leaving, each turns back: its window never goes
        away."""
        for key, namespace in self.EDGE_PANELS:
            with self.subTest(panel=namespace):
                process = self.background_input("key", key, "sleep", "600", "key", key, "sleep", "60",
                                                "key", key, "sleep", "800")
                self.vela.wait_for(lambda s: self.mapped(s, namespace), what=f"{namespace} open")
                while process.poll() is None:
                    self.assertTrue(self.mapped(self.vela.state(), namespace), "it closed before turning back")
                self.check_desktop()
                self.assertTrue(self.mapped(self.vela.state(), namespace))
                self.vela.input("key", "Escape")
                self.vela.wait_for(lambda s: not self.mapped(s, namespace), what=f"{namespace} closed")

    def test_a_click_in_an_edge_panels_shadow_closes_it(self):
        """As for Quick Settings: a click in the room left of the panel
        closes it, one on the panel's own background doesn't."""
        for key, namespace in self.EDGE_PANELS:
            with self.subTest(panel=namespace):
                self.vela.input("key", key)
                layer = self.settled_layer(namespace)
                self.vela.input("key", "Escape")
                self.vela.wait_for(lambda s: not self.mapped(s, namespace), what=f"{namespace} closed")
                left, top, right, bottom = self.panel_box(layer)
                # Low in the panel: the Notification Center's calendar, below
                # the gap between its two panels.
                y = bottom - 40
                self.vela.input("key", key, "sleep", "900", "move", left + 6, y, "click", "left", "sleep", "700")
                self.assertTrue(self.mapped(self.vela.state(), namespace), "a click on the panel closed it")
                self.vela.input("key", "Escape")
                self.vela.wait_for(lambda s: not self.mapped(s, namespace), what=f"{namespace} closed")
                self.vela.input("key", key, "sleep", "900", "move", layer["x"] + 20, y, "click", "left",
                                "sleep", "700")
                self.vela.wait_for(lambda s: not self.mapped(s, namespace), timeout=3,
                                   what=f"{namespace} closed by a click in its shadow")
                self.check_desktop()

    def test_dialogs_leave_animated_menus_and_task_view_at_once(self):
        """docs/visual-coherence.md D16 and K5: dialogs and panels leave with
        an animation (`normal`, 600 ms at 300%), menus and Task View vanish
        at once."""
        self.slow_animations()
        cases = (("Run", lambda: self.vela.input("key", "super+r"), "vela-run", True),
                 ("Properties", lambda: self.shell_command(f'properties {json.dumps([self.vela.directory])}'),
                  "vela-properties", True),
                 ("the clipboard", lambda: self.vela.input("key", "super+v"), "vela-clipboard", True),
                 ("the desktop menu", lambda: self.vela.input("move", 900, 500, "sleep", "300", "click", "right"),
                  "vela-context-menu", False),
                 ("Task View", lambda: self.vela.input("key", "super+Tab"), "vela-task-view", False))
        took = {}
        for name, show, namespace, _ in cases:
            show()
            self.vela.wait_for(lambda s: self.mapped(s, namespace), what=f"{name} open")
            time.sleep(1.5)
            start = time.monotonic()
            self.vela.input("key", "Escape")
            self.vela.wait_for(lambda s: not self.mapped(s, namespace), timeout=5, what=f"{name} closed")
            took[name] = time.monotonic() - start
            time.sleep(0.5)
        self.check_desktop()
        for name, _, _, animated in cases:
            with self.subTest(panel=name):
                if animated:
                    self.assertGreater(took[name], 0.45, f"{name} vanished at once: {took}")
                else:
                    self.assertLess(took[name], 0.3, f"{name} left with an animation: {took}")

    def settled_layer(self, namespace):
        """The layer once its geometry has stayed the same for a second (the
        volume's row appears when the audio service answers)."""
        last, since = None, time.monotonic()
        deadline = since + 10
        while time.monotonic() < deadline:
            layer = self.layer(self.vela.state(), namespace)
            box = layer and (layer["x"], layer["y"], layer["w"], layer["h"])
            if box != last:
                last, since = box, time.monotonic()
            elif layer and time.monotonic() - since >= 1.0:
                return layer
            time.sleep(0.05)
        self.fail(f"{namespace} doesn't settle: {last}")

    def panel_top(self, image, x0, y0, y1):
        """The panel's top edge in a capture: the first row of [y0, y1) whose
        color jumps from the row above (the shadow only darkens smoothly)."""
        for y in range(y0 + 1, y1):
            if max(abs(a - b) for a, b in zip(image.at(x0, y), image.at(x0, y - 1))) > 25:
                return y
        return None

    def test_quick_settings_height_between_pages(self):
        """Going to a page and back, the window changes height twice, before
        the panel grows and after it shrinks, never on a frame of the panel's
        animation, and ends as the panel plus its shadow's room."""
        self.vela.input("key", "super+a")
        main = self.settled_layer(self.QS)
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, self.QS), what="Quick Settings closed")
        output = self.vela.state().output
        self.assertEqual(main["x"] + main["w"], output["w"], "not at the output's right edge")
        self.assertEqual(main["y"] + main["h"], output["h"] - 48, "not on the taskbar")
        x = main["x"] + self.ROOM + 10 # in the panel's left margin
        main_top = main["y"] + self.ROOM

        # Win+Ctrl+V: the sound page, which every machine has (it's taller).
        process = self.background_input("key", "super+a", "sleep", "900", "key", "super+ctrl+v", "sleep", "1200",
                                        "key", "Escape", "sleep", "1200")
        heights, tops = [], []
        while process.poll() is None:
            layer = self.layer(self.vela.state(), self.QS)
            if layer is None:
                continue
            if not heights or heights[-1] != (layer["y"], layer["h"]):
                heights.append((layer["y"], layer["h"]))
            if layer["h"] > main["h"] and len(tops) < 12:
                # The panel's top while it grows, in a thin strip.
                path = os.path.join(self.vela.directory, "strip.png")
                self.vela.screenshot(path, region=(x, 0, 1, layer["y"] + layer["h"]))
                tops.append(self.panel_top(Image.read(path), 0, layer["y"] + 1, layer["y"] + layer["h"]))
        self.check_desktop()
        self.assertEqual(len(heights), 3, f"the window resized on the panel's frames: {heights}")
        self.assertEqual(heights[0], (main["y"], main["h"]))
        self.assertEqual(heights[2], (main["y"], main["h"]))
        page_y, page_h = heights[1]
        self.assertGreater(page_h, main["h"])
        self.assertEqual(page_y + page_h, main["y"] + main["h"], "the window's bottom moved")
        page_top = page_y + self.ROOM
        # The panel grew inside the larger window: some frames in between.
        self.assertTrue(any(t is not None and page_top < t < main_top for t in tops),
                        f"the panel jumped to its height: tops {tops}, from {main_top} to {page_top}")
        # Back on the main page, still open: the panel fills its window but
        # the room.
        layer = self.settled_layer(self.QS)
        self.assertEqual((layer["y"], layer["h"]), (main["y"], main["h"]))
        image = self.vela.pixels()
        self.assertEqual(self.panel_top(image, x, layer["y"] + 1, layer["y"] + layer["h"]), main_top)


# Black and white stripes 4 pixels wide: blurred they become one gray, seen
# through a translucent panel or a shadow they stay stripes.
STRIPES = ('<svg xmlns="http://www.w3.org/2000/svg" width="1920" height="1080" viewBox="0 0 1920 1080">'
           '<pattern id="p" width="8" height="8" patternUnits="userSpaceOnUse">'
           '<rect width="4" height="8" fill="#fff"/></pattern>'
           '<rect width="1920" height="1080" fill="#000"/><rect width="1920" height="1080" fill="url(#p)"/></svg>')


class PanelBlur(RealShell):
    """The blur under a sliding panel (docs/visual-coherence.md §6.9, D17)."""

    wallpaper = STRIPES
    # How much of the stripes a row shows (0-255): through the panel with blur
    # behind it nothing; through the panel without blur about 110; through its
    # shadow or nothing, more than 130 (one row of the panel's antialiased
    # edge can be anything).
    BLURRED = 6
    UNCOVERED = 125
    EDGE = 3
    PARTIAL = 60 # the panel over the output's edge

    def blur_regions(self, namespace):
        """The blur regions the shell asked for this layer's latest surface,
        in order, from the protocol log: each a list of (x, y, w, h)."""
        log = self.vela.log_text()
        surface = re.findall(rf'get_layer_surface\(.*?wl_surface[@#](\d+).*?"{namespace}"', log)[-1]
        effect = re.findall(r'get_background_effect\(new id ext_background_effect_surface_v1[@#](\d+), '
                            rf'wl_surface[@#]{surface}\)', log)[-1]
        start = log.rindex(f"ext_background_effect_surface_v1#{effect}, wl_surface#{surface}")
        rects, regions = {}, []
        for line in log[start:].splitlines():
            add = re.search(r'-> wl_region[@#](\d+)\.add\((-?\d+), (-?\d+), (\d+), (\d+)\)', line)
            if add:
                rects.setdefault(add[1], []).append(tuple(int(v) for v in add.groups()[1:]))
            region = re.search(rf'-> ext_background_effect_surface_v1[@#]{effect}\.set_blur_region\('
                               r'wl_region[@#](\d+)\)', line)
            if region:
                regions.append(rects.pop(region[1], []))
        return regions

    @staticmethod
    def stripes(image, x):
        """For each row, the contrast between the 8 pixels from x and the
        stripes 4 pixels further."""
        return [max(max(abs(a - b) for a, b in zip(image.at(x + i, y), image.at(x + i + 4, y))) for i in range(4))
                for y in range(image.height)]

    def rising_blur(self, key, namespace, room_left=46, width=360, inset=8):
        """Every frame of the entrance has blur behind the whole panel, also
        below where it rests (it rises from behind the taskbar); at rest its
        shadow there isn't blurred. A strip `inset` units into the panel,
        where it is plain surface."""
        self.slow_animations()
        self.vela.input("key", key)
        layer = self.vela.wait_for(lambda s: self.layer(s, namespace), what=f"{namespace} open")
        layer = self.layer(layer, namespace)
        x = layer["x"] + room_left + inset
        bottom = layer["y"] + layer["h"] # the window's, on the taskbar
        rest_bottom = bottom - 12
        path = os.path.join(self.vela.directory, "strip.png")
        frames = []
        deadline = time.monotonic() + 1.2
        while time.monotonic() < deadline:
            self.vela.screenshot(path, region=(x, 0, 12, bottom))
            frames.append(self.stripes(Image.read(path), 0))
        time.sleep(1.5)
        settled = self.layer(self.vela.state(), namespace)
        self.assertEqual((settled["y"], settled["h"]), (layer["y"], layer["h"]), "the window changed size")
        self.vela.screenshot(path, region=(x, 0, 12, bottom))
        rest = self.stripes(Image.read(path), 0)
        top = next(y for y, c in enumerate(rest) if c <= self.BLURRED)
        panel_rows = rest_bottom - (layer["y"] + 46)
        enough = panel_rows * 3 // 4
        self.assertGreater(sum(1 for c in rest if c <= self.BLURRED), enough)
        # At rest the blur is the panel's only: its shadow, in the same
        # surface, shows the stripes, darker.
        shadow = rest[rest_bottom + 2:bottom]
        self.assertTrue(all(c > self.UNCOVERED for c in shadow), f"the shadow is blurred: {shadow}")
        regions = self.blur_regions(namespace)
        self.assertEqual(regions[-1], [(room_left, 46, width, layer["h"] - 46 - 12)],
                         "the region at rest isn't the panel")

        sliding = 0
        for number, rows in enumerate(frames):
            blurred = [y for y, c in enumerate(rows) if c <= self.BLURRED]
            if len(blurred) < enough:
                continue # still faint: the blur follows its opacity
            if blurred[0] <= top + 1:
                continue # at rest
            # Below the blurred rows down to the window's bottom: the shadow
            # or the wallpaper, never the panel without blur. EDGE rows: the
            # panel's antialiased edge and the shadow just below it, blurred
            # a little inside the travel (§6.9); without the travel the
            # band would be as tall as the slide.
            unblurred = [y for y in range(blurred[-1] + 1, bottom) if rows[y] <= self.UNCOVERED]
            self.assertLessEqual(len(unblurred), self.EDGE, f"frame {number}: the panel without blur at rows "
                                                            f"{unblurred} ({[rows[y] for y in unblurred]})")
            if blurred[-1] > rest_bottom + 2:
                sliding += 1
        self.assertGreater(sliding, 0, "no capture landed while the panel was below where it rests")

    def test_quick_settings_has_blur_behind_it_while_it_slides(self):
        self.rising_blur("super+a", "vela-quick-settings")

    def test_start_has_blur_behind_it_while_it_rises(self):
        self.rising_blur("super", "vela-start-menu", width=640)

    def test_the_clipboard_has_blur_behind_it_while_it_rises(self):
        # Left of its title: 3 units in.
        self.rising_blur("super+v", "vela-clipboard", inset=3)

    def test_the_notification_center_has_blur_behind_it_while_it_slides(self):
        """It comes from the right: every frame has blur behind the whole
        panel, also right of where it rests (the window cuts it at the
        output's edge); at rest its shadow there isn't blurred."""
        self.slow_animations()
        namespace = "vela-notification-center"
        self.vela.input("key", "super+n")
        layer = self.vela.wait_for(lambda s: self.layer(s, namespace), what="the Notification Center open")
        layer = self.layer(layer, namespace)
        # A row in the calendar's bottom margin, from the window's left edge
        # to the output's.
        y = layer["y"] + layer["h"] - 12 - 6
        x0, right = layer["x"], layer["x"] + layer["w"]
        rest_right = right - 12 - x0
        path = os.path.join(self.vela.directory, "row.png")

        def columns():
            """For each column, the contrast with the stripe 4 pixels to its
            right (to its left in the last 4, at the output's edge)."""
            self.vela.screenshot(path, region=(x0, y, layer["w"], 1))
            image = Image.read(path)
            return [max(abs(a - b) for a, b in zip(image.at(x, 0), image.at(x + 4 if x + 4 < image.width else x - 4, 0)))
                    for x in range(image.width)]

        frames = []
        deadline = time.monotonic() + 1.2
        while time.monotonic() < deadline:
            frames.append(columns())
        time.sleep(1.5)
        rest = columns()
        resting = [x for x, c in enumerate(rest) if c <= self.BLURRED]
        left = resting[0]
        self.assertLessEqual(abs(left - 47), 1, f"the panel doesn't rest where expected: {left}") # after its border
        # How many columns from the first are blurred when all of the panel
        # is in the window.
        span = resting[-1] - left
        shadow = rest[rest_right + 2:]
        self.assertTrue(all(c > self.UNCOVERED for c in shadow), f"the shadow is blurred: {shadow}")
        regions = self.blur_regions(namespace)
        self.assertEqual(len(regions[-1]), 2, "at rest the region isn't the two panels")
        for rect in regions[-1]:
            self.assertEqual((rect[0], rect[2]), (46, 360))
        self.assertEqual(sum(r[3] for r in regions[-1]) + 12, layer["h"] - 46 - 12, "not the two panels")

        sliding = 0
        for number, cols in enumerate(frames):
            blurred = [x for x, c in enumerate(cols) if c <= self.BLURRED]
            if len(blurred) < 300:
                continue # still faint: the blur follows its opacity
            if blurred[0] <= left + 1:
                continue # at rest
            # As many columns as at rest, from where the panel is in this
            # frame, cut by the output's edge: blurred, if only partly at
            # the edge (the blur there reads the pixels it has), but for its
            # right edge and the columns that read past it (4). Without the
            # travel the 12 and more right of where it rests show the
            # stripes.
            unblurred = [x for x in range(blurred[0], min(blurred[0] + span, len(cols)))
                         if cols[x] > self.PARTIAL]
            self.assertLessEqual(len(unblurred), 4, f"frame {number}: the panel without blur at columns "
                                                    f"{unblurred} ({[cols[x] for x in unblurred]})")
            sliding += 1
        self.assertGreater(sliding, 0, "no capture landed while the panel was sliding")


if __name__ == "__main__":
    unittest.main()
