# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Several outputs: hot plugged and unplugged, each with its own scale; the
windows of an output that goes away move to another and return to their place
when the output comes back (vela_views_check_outputs); scale and position
chosen as Settings > Display does (wlr-output-management)."""

import os
import signal
import subprocess
import unittest

from harness import BUILD, Session, tool


class Outputs(unittest.TestCase):
    def setUp(self):
        self.vela = Session(scale="HEADLESS-1=1,HEADLESS-2=1.5", size="1920x1080@60")
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def add_second(self):
        self.vela.command("test-output add 1920x1080")
        return self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two outputs")

    def open_transient_pair(self, maximized=False):
        # The child is a real xdg_toplevel with set_parent, NOT an xdg_popup.
        # Wait for the mapped parent before moving it during the delay.
        options = [tool("vela-pattern"), "--transient-delay", "4000"]
        if maximized:
            options.append("--transient-maximized")
        options += ["360", "260"]
        parent = self.vela.open_window(command=options)
        return parent

    def move_window_to_second(self, window, output):
        start = self.vela.state().window(window)
        grab_x, grab_y = start["x"] + 50, start["y"] + 50
        target_x, target_y = output["x"] + 200, output["y"] + 200
        self.vela.input("move", grab_x, grab_y, "keydown", "super", "down", "sleep", "50",
                        "move", (grab_x + target_x) // 2, (grab_y + target_y) // 2, "sleep", "50",
                        "move", target_x, target_y, "sleep", "100", "up", "keyup", "super")
        self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-2",
                           what="transient parent moved to output B")

    def child_of(self, parent):
        state = self.vela.wait_for(
            lambda s: any(w["id"] != parent for w in s.windows),
            timeout=10, what="mapped XDG transient child")
        child = next(w["id"] for w in state.windows if w["id"] != parent)
        return state, child

    def test_transient_follows_parent_not_cursor_and_uses_output_scale(self):
        state = self.add_second()
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        parent = self.open_transient_pair()
        self.move_window_to_second(parent, second)
        # Leave the pointer on A: a child must still map on its parent's B.
        self.vela.input("move", 100, 100)
        state, child = self.child_of(parent)
        self.assertEqual(state.window(child)["output"], "HEADLESS-2")
        dialog = state.window(child)
        self.assertGreaterEqual(dialog["x"], second["x"])
        self.assertLessEqual(dialog["x"] + dialog["w"], second["x"] + second["w"])

    def test_initially_maximized_transient_uses_parent_output_dimensions(self):
        state = self.add_second()
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        parent = self.open_transient_pair(maximized=True)
        self.move_window_to_second(parent, second)
        self.vela.input("move", 100, 100)
        state, child = self.child_of(parent)
        dialog = state.window(child)
        self.assertEqual(dialog["output"], "HEADLESS-2")
        self.assertTrue(dialog["maximized"])
        self.assertEqual((dialog["x"], dialog["y"], dialog["w"], dialog["h"]),
                         (second["x"], second["y"], second["w"], second["h"]))

    def test_hotplug_with_its_own_scale(self):
        state = self.add_second()
        scales = {output["name"]: output["scale"] for output in state["outputs"]}
        self.assertEqual(scales, {"HEADLESS-1": 1, "HEADLESS-2": 1.5})
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        self.assertEqual((second["w"], second["h"]), (1280, 720)) # 1920x1080 at 150%
        self.vela.command("test-output remove HEADLESS-2")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 1, what="one output again")

    def test_unplugging_the_middle_output_closes_the_gap(self):
        state = self.add_second()
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        self.vela.command("test-output add 1920x1080")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 3, what="three outputs")
        third = next(o for o in state["outputs"] if o["name"] == "HEADLESS-3")
        self.assertEqual(third["x"], second["x"] + second["w"])

        self.vela.command("test-output remove HEADLESS-2")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 2 and any(
            o["name"] == "HEADLESS-3" and o["x"] == second["x"] for o in s["outputs"]),
            what="third output fills the gap left by the middle one")
        third = next(o for o in state["outputs"] if o["name"] == "HEADLESS-3")
        self.assertEqual(third["y"], 0)

    def test_resize_follows_the_output_when_the_middle_monitor_is_unplugged(self):
        self.add_second()
        self.vela.command("test-output add 1920x1080")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 3, what="three outputs")
        third = next(o for o in state["outputs"] if o["name"] == "HEADLESS-3")
        window = self.vela.open_window(300, 200)
        frame = self.vela.state().window(window)
        self.vela.input("move", frame["x"] + 50, frame["y"] + 50, "keydown", "super", "down",
                        "sleep", "50", "move", third["x"] + 200, third["y"] + 200,
                        "sleep", "50", "up", "keyup", "super")
        state = self.vela.wait_for(lambda s: s.window(window)["output"] == third["name"])
        frame = state.window(window)
        grab_x, grab_y = frame["x"] + frame["w"] - 20, frame["y"] + frame["h"] - 20
        with self.vela.input_session() as send:
            send("move", grab_x, grab_y, "keydown", "super", "down", "right",
                 "move", grab_x + 30, grab_y + 20)
            try:
                self.vela.wait_for(lambda s: s.window(window)["w"] == frame["w"] + 30)
                self.vela.command("test-output remove HEADLESS-2")
                moved = self.vela.wait_for(lambda s: len(s["outputs"]) == 2
                                           and s.window(window)["x"] < frame["x"])
                dx = moved.window(window)["x"] - frame["x"]
                send("rel", 30, 20)
                self.vela.wait_for(lambda s: s.window(window)["w"] == frame["w"] + 60
                                   and s.window(window)["x"] == frame["x"] + dx,
                                   what="the ongoing resize continues from the translated window")
            finally:
                send("up", "right", "keyup", "super")

    def test_resize_continues_when_only_the_pointer_moves_on_hotplug(self):
        self.add_second()
        self.vela.command("test-output add 1920x1080")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 3, what="three outputs")
        third = next(o for o in state["outputs"] if o["name"] == "HEADLESS-3")
        window = self.vela.open_window(300, 200)
        frame = self.vela.state().window(window)
        self.vela.input("move", frame["x"] + 50, frame["y"] + 50, "keydown", "super", "down",
                        "sleep", "50", "move", 150, 200, "sleep", "50", "up", "keyup", "super")
        state = self.vela.wait_for(lambda s: s.window(window)["x"] == 100, what="the window near the left edge")
        frame = state.window(window)
        grab_x, grab_y = frame["x"] + frame["w"] - 20, frame["y"] + frame["h"] - 20
        target_x = third["x"] + 100
        width = frame["w"] + target_x - grab_x
        with self.vela.input_session() as send:
            send("move", grab_x, grab_y, "keydown", "super", "down", "right", "move", target_x, grab_y)
            try:
                wide = self.vela.wait_for(lambda s: s.window(window)["w"] == width,
                                          what="the resize pointer on the third output")
                self.assertEqual(wide.window(window)["output"], "HEADLESS-1")
                self.vela.command("test-output remove HEADLESS-2")
                self.vela.wait_for(lambda s: any(o["name"] == third["name"] and o["x"] == 1920 for o in s["outputs"]),
                                   what="the pointer's output shifts left")
                send("rel", 30, 0)
                self.vela.wait_for(lambda s: s.window(window)["w"] == width + 30
                                   and s.window(window)["x"] == frame["x"],
                                   what="the resize continues after only the pointer moves")
            finally:
                send("up", "right", "keyup", "super")

    def test_windows_are_rescued_and_come_back(self):
        state = self.add_second()
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        window = self.vela.open_window(300, 200)
        # Super+drag: the window onto the second output.
        start = self.vela.state().window(window)
        grab_x, grab_y = start["x"] + 50, start["y"] + 50
        target_x, target_y = second["x"] + 200, second["y"] + 200
        self.vela.input("move", grab_x, grab_y, "keydown", "super", "down", "sleep", "50",
                        "move", (grab_x + target_x) // 2, (grab_y + target_y) // 2, "sleep", "50",
                        "move", target_x, target_y, "sleep", "100", "up", "keyup", "super")
        state = self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-2", what="on the second output")
        placed = state.window(window)
        # The output turns off (Settings > Display, wlr-output-management): the
        # window moves to the first one, entirely inside.
        self.vela.randr("--output", "HEADLESS-2", "--off")
        state = self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-1", what="rescued to the first output")
        rescued, first = state.window(window), state.output
        self.assertGreaterEqual(rescued["x"], first["x"])
        self.assertLessEqual(rescued["x"] + rescued["w"], first["x"] + first["w"])
        # The output comes back: the window returns where it was.
        self.vela.randr("--output", "HEADLESS-2", "--on")
        state = self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-2", what="back on its output")
        back = state.window(window)
        self.assertEqual((back["x"], back["y"]), (placed["x"], placed["y"]))

    def test_scale_and_position_like_settings(self):
        self.add_second()
        # The second one at 200% and above the first, right-aligned.
        self.vela.randr("--output", "HEADLESS-2", "--scale", "2", "--pos", "960,-540")
        state = self.vela.wait_for(lambda s: any(o["name"] == "HEADLESS-2" and o["scale"] == 2 for o in s["outputs"]),
                                   what="the second output at 200%")
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        # Output coordinates are rebased to the first nonnegative desktop
        # quadrant, but the relative "above and right-aligned" arrangement stays.
        self.assertEqual((second["x"], second["y"], second["w"], second["h"]), (960, 0, 960, 540))
        primary = next(o for o in state["outputs"] if o["name"] == "HEADLESS-1")
        self.assertEqual((primary["x"], primary["y"]), (0, 540))
        listed = dict(line.split(" ", 1) for line in self.vela.randr().splitlines())
        self.assertEqual(listed["HEADLESS-2"], "1920x1080@0.000 960,0 2.0000")
        # A window maximized there fills exactly its pixels.
        window = self.vela.open_window(300, 200)
        start = self.vela.state().window(window)
        grab_x, grab_y = start["x"] + 50, start["y"] + 50
        self.vela.input("move", grab_x, grab_y, "keydown", "super", "down", "sleep", "50",
                        "move", 1200, 300, "sleep", "100", "up", "keyup", "super")
        self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-2", what="on the second output")
        self.vela.keys("super+up")
        self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")
        # The state flag precedes the client's configure acknowledgement and
        # resized buffer. Check the final geometry after those have arrived.
        state = self.vela.wait_for(lambda s: s.window(window)["w"] == second["w"],
                                   what="the maximized client's resized buffer")
        maximized = state.window(window)
        self.assertEqual((maximized["x"], maximized["w"]), (960, 960))

    def test_qhd_maximize_and_edge_snap_use_its_own_geometry(self):
        # The resolutions from #301, at 100% scale and with distinct positions.
        self.vela.command("test-output add 1920x1200")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="the laptop panel")
        self.vela.randr("--output", "HEADLESS-2", "--scale", "1")
        self.vela.command("test-output add 2560x1440")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 3, what="the QHD output")
        qhd = next(o for o in state["outputs"] if o["name"] == "HEADLESS-3")
        x, y = qhd["x"], qhd["y"]
        self.assertEqual((qhd["w"], qhd["h"]), (2560, 1440))

        def drag(window, target_x, target_y, preview=None):
            frame = self.vela.state().window(window)
            grab_x, grab_y = frame["x"] + 50, frame["y"] + 50
            with self.vela.input_session() as send:
                send("move", grab_x, grab_y, "keydown", "super", "down", "sleep", "50",
                     "move", x + 700, y + 700, "sleep", "50", "move", target_x, target_y)
                try:
                    if preview is not None:
                        self.vela.wait_for(lambda s: s["snapPreview"] == list(preview),
                                           what=f"the snap preview fills QHD geometry {preview}")
                finally:
                    send("up", "keyup", "super")

        def check(window, expected):
            self.vela.wait_for(lambda s: tuple(s.window(window)[k] for k in ("x", "y", "w", "h")) == expected,
                               what=f"window fills QHD geometry {expected}")

        window = self.vela.open_window(300, 200)
        drag(window, x + 900, y + 700)
        self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-3")
        self.vela.keys("super+up")
        check(window, (x, y, 2560, 1440))

        for target, expected in (
            ((x + 2, y + 700), (x, y, 1280, 1440)),
            ((x + 2557, y + 700), (x + 1280, y, 1280, 1440)),
            ((x + 1280, y + 2), (x, y, 2560, 1440)),
        ):
            with self.subTest(target=target):
                window = self.vela.open_window(300, 200)
                drag(window, *target, preview=expected)
                check(window, expected)

    def test_above_layout_normalizes_origin_without_moving_window_to_other_monitor(self):
        # Reproduces Isaac's original topology: secondary 1920x1080 located
        # at y=-1080 above primary 1920x1080 at (0,0). XWayland and Wine
        # cannot use the negative monitor origin as a valid X11 root extent.
        window = self.vela.open_window(300, 200)
        before = self.vela.state().window(window)
        self.assertEqual(before["output"], "HEADLESS-1")
        self.add_second()
        self.vela.randr("--output", "HEADLESS-2", "--scale", "1", "--pos", "0,-1080")
        state = self.vela.wait_for(lambda s: any(
            o["name"] == "HEADLESS-1" and o["y"] == 1080 for o in s["outputs"]),
            what="negative request rebased to zero")
        positions = {o["name"]: (o["x"], o["y"]) for o in state["outputs"]}
        self.assertEqual(positions, {"HEADLESS-1": (0, 1080), "HEADLESS-2": (0, 0)})
        self.assertEqual((state.window(window)["x"], state.window(window)["y"]),
                         (before["x"], before["y"] + 1080))
        self.assertEqual(state.window(window)["output"], "HEADLESS-1")
        listed = dict(line.split(" ", 1) for line in self.vela.randr().splitlines())
        self.assertIn("0,1080", listed["HEADLESS-1"])
        self.assertIn("0,0", listed["HEADLESS-2"])
        # Move the same display right of the primary: the layout no longer needs
        # the shift, both return to y = 0 and the window with its output.
        self.vela.randr("--output", "HEADLESS-2", "--pos", "1920,1080")
        state = self.vela.wait_for(lambda s: any(
            o["name"] == "HEADLESS-1" and o["y"] == 0 for o in s["outputs"]),
            what="restored nonnegative side-by-side output layout")
        self.assertEqual((state.window(window)["x"], state.window(window)["y"]),
                         (before["x"], before["y"]))
        self.assertEqual(state.window(window)["output"], "HEADLESS-1")

    def settings_preview(self, disable=None):
        executable = os.path.join(BUILD, "settings", "vela-displays-test")
        if not os.path.exists(executable):
            self.skipTest("native Settings backend test client not built")
        command = [executable, "--preview-client", self.vela.display]
        if disable:
            command.append(disable)
        process = subprocess.Popen(command, env=self.vela.client_env,
                                   stdout=subprocess.DEVNULL, stderr=self.vela.log)
        self.vela.clients.append(process)
        if disable:
            self.vela.wait_for(lambda s: any(o["name"] == disable and not o["enabled"] for o in s["outputs"]),
                               what="native Settings disable preview applied")
        else:
            self.vela.wait_for(lambda s: s.output["scale"] == 1.5,
                               what="native Settings preview applied")
        return process

    def test_unplugging_the_disabled_output_cancels_its_preview(self):
        self.add_second()
        settings = self.settings_preview(disable="HEADLESS-2")
        self.vela.command("test-output remove HEADLESS-2")
        self.assertEqual(settings.wait(timeout=5), 0)
        self.assertEqual([o["name"] for o in self.vela.state()["outputs"]], ["HEADLESS-1"])

    def test_settings_crash_reverts_native_preview(self):
        before = self.vela.state().output
        settings = self.settings_preview()
        settings.kill()
        settings.wait(2)
        state = self.vela.wait_for(lambda s: s.output["scale"] == before["scale"],
                                   what="compositor recovery after Settings SIGKILL")
        for key in ("x", "y", "w", "h", "scale"):
            self.assertEqual(state.output[key], before[key])

    def test_settings_hang_reverts_native_preview(self):
        before = self.vela.state().output
        settings = self.settings_preview()
        os.kill(settings.pid, signal.SIGSTOP)
        try:
            state = self.vela.wait_for(lambda s: s.output["scale"] == before["scale"], timeout=20,
                                       what="compositor deadline with Settings stopped")
            for key in ("x", "y", "w", "h", "scale"):
                self.assertEqual(state.output[key], before[key])
        finally:
            os.kill(settings.pid, signal.SIGCONT)

    def test_unplugged_output_rescues_windows(self):
        state = self.add_second()
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        window = self.vela.open_window(300, 200)
        start = self.vela.state().window(window)
        self.vela.input("move", start["x"] + 50, start["y"] + 50, "keydown", "super", "down", "sleep", "50",
                        "move", second["x"] + 200, second["y"] + 200, "sleep", "100", "up", "keyup", "super")
        self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-2")
        # Unplugged entirely: the window stays reachable on the first.
        self.vela.command("test-output remove HEADLESS-2")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 1 and s.window(window)["output"] == "HEADLESS-1",
                                   what="rescued to the first output")
        rescued, first = state.window(window), state.output
        self.assertGreaterEqual(rescued["y"], first["y"])
        self.assertLessEqual(rescued["y"] + rescued["h"], first["y"] + first["h"])


if __name__ == "__main__":
    unittest.main()
