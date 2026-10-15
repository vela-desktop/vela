# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The actual Qt shell: panels, mixed-output coordinates, selection and snips.

Fake shell sockets and vela-panel cannot exercise Qt's layer-surface lifecycle.
Run the real shell on a private session bus and isolated configuration.
"""

from contextlib import contextmanager
import json
import os
import re
import subprocess
import time
import unittest

from harness import BUILD, Image, Session, tool

SHELL = os.path.join(BUILD, "shell", "vela-shell")
OUTPUT_CASES = (("normal", 1, "above"), ("90", 1, "above"), ("180", 1.25, "right"),
                ("270", 1.5, "left"), ("flipped", 1, "below"), ("flipped-90", 1.25, "right"),
                ("flipped-180", 1.5, "left"), ("flipped-270", 1, "below"))


@unittest.skipUnless(os.path.exists(SHELL), "vela-shell not built (VELA_BUILD_SHELL=OFF)")
class QtShell(unittest.TestCase):
    def setUp(self):
        self.bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.addCleanup(self.stop_bus)
        address = self.bus.stdout.readline().strip()
        self.assertTrue(address, "private D-Bus session failed to start")
        self.vela = Session(size="1920x1080@100", env={
            "VELA_SYNC_FILE": os.environ.get("VELA_QT_SYNC_FILE", "0"), "DBUS_SESSION_BUS_ADDRESS": address,
            "VELA_DEBUG_ANIMATION": "1"})
        self.vela.start()
        self.addCleanup(self.vela.stop)
        # Snips and desktop files must stay in this isolated test session.
        pictures = os.path.join(self.vela.directory, "Pictures")
        desktop = os.path.join(self.vela.directory, "Desktop")
        os.makedirs(pictures)
        os.makedirs(desktop)
        os.makedirs(self.vela.env["XDG_CONFIG_HOME"], exist_ok=True)
        with open(os.path.join(self.vela.env["XDG_CONFIG_HOME"], "user-dirs.dirs"), "w") as file:
            file.write(f'XDG_PICTURES_DIR="{pictures}"\nXDG_DESKTOP_DIR="{desktop}"\n')
        wallpaper = os.path.join(self.vela.directory, "wallpaper.svg")
        with open(wallpaper, "w") as file:
            file.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
                       '<linearGradient id="g" x2="1" y2="1">'
                       '<stop stop-color="#15324a"/><stop offset="1" stop-color="#da805a"/>'
                       '</linearGradient><path fill="url(#g)" d="M0 0H100V100H0Z"/></svg>')
        env = dict(self.vela.client_env, QT_QPA_PLATFORM="wayland",
                   QT_WAYLAND_SHELL_INTEGRATION="layer-shell", QT_WAYLAND_RECONNECT="0",
                   QT_FORCE_STDERR_LOGGING="1", QSG_RENDER_LOOP="threaded", QSG_RHI_BACKEND="opengl",
                   WAYLAND_DEBUG="client", VELA_WALLPAPER=wallpaper)
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

    def test_desktop_selection_matches_coordinates_on_transformed_outputs(self):
        """Growing, shrinking and releasing the band must preserve its coordinates on every transform."""
        for transform, scale, side in OUTPUT_CASES:
            with self.subTest(transform=transform, scale=scale, side=side):
                output = self.secondary_output(transform, scale, side)
                sx, sy = 120, 160
                ends = ((500, min(950, output["h"] - 120)), (330, min(560, output["h"] - 240)))
                self.vela.input("move", output["x"] + sx, output["y"] + sy)
                before = self.settled_wallpaper()
                # Keep one virtual pointer alive throughout the complete drag,
                # including snapshots while its button is held.
                # Allow Qt to process device creation/focus before the press,
                # then the press before motion (as with an existing physical mouse).
                actions = ["move", output["x"] + sx, output["y"] + sy, "sleep", 100,
                           "down", "sleep", 100]
                for ex, ey in ends:
                    actions += ["move", output["x"] + ex, output["y"] + ey, "sleep", 2000]
                actions += ["up", "move", output["x"] + sx, output["y"] + sy]
                with self.held_input(actions):
                    for ex, ey in ends:
                        rectangle = tuple(int(v * scale) for v in (sx, sy, ex, ey))

                        def correctly_drawn(_):
                            image = self.vela.pixels(output=1)
                            # Exact band edges; omit the live taskbar clock.
                            desktop_height = image.height - int(48 * scale)
                            return (image.changed_bounds(before, height=desktop_height) == rectangle
                                    and all(image.at(int(x * scale), int(y * scale))
                                            != before.at(int(x * scale), int(y * scale))
                                            for x, y in ((sx + 40, sy + 40), (ex - 40, ey - 40))))

                        self.vela.wait_for(correctly_drawn, timeout=4, read_state=False,
                                           what=f"desktop selection bounds {rectangle}")
                desktop_height = before.height - int(48 * scale)
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
                actions = ["down", "sleep", 100, "move", output["x"] + 330, output["y"] + 560,
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
    def held_input(self, actions):
        """Keep a pointer alive while inspecting its drag, then release and reap it."""
        pointer = subprocess.Popen([tool("vela-input"), *map(str, actions)], env=self.vela.client_env)
        try:
            yield
        finally:
            try:
                pointer.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pointer.kill()
                pointer.wait()
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

    def test_panels_animate(self):
        """Start, Quick Settings, the notification center and Task View don't
        just appear (docs/visual-coherence.md K5): at 300% (Settings' slider,
        so a capture lands inside the animation) the panel's first frames still
        show what's behind its top edge."""
        os.makedirs(os.path.dirname(self.vela.config_path()), exist_ok=True)
        with open(self.vela.config_path(), "w") as conf:
            conf.write("animation-duration=300\n")
        self.vela.command("reload-config")
        time.sleep(1.0) # the shell sees the file
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
                               ("super+n", "vela-notification-center")]:
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


if __name__ == "__main__":
    unittest.main()
