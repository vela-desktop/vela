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

from harness import BUILD, Session


class Outputs(unittest.TestCase):
    def setUp(self):
        self.vela = Session(scale="HEADLESS-1=1,HEADLESS-2=1.5", size="1920x1080@60")
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def add_second(self):
        self.vela.command("test-output add 1920x1080")
        return self.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two outputs")

    def test_hotplug_with_its_own_scale(self):
        state = self.add_second()
        scales = {output["name"]: output["scale"] for output in state["outputs"]}
        self.assertEqual(scales, {"HEADLESS-1": 1, "HEADLESS-2": 1.5})
        second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
        self.assertEqual((second["w"], second["h"]), (1280, 720)) # 1920x1080 at 150%
        self.vela.command("test-output remove HEADLESS-2")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 1, what="one output again")

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
        self.assertEqual((second["x"], second["y"], second["w"], second["h"]), (960, -540, 960, 540))
        listed = dict(line.split(" ", 1) for line in self.vela.randr().splitlines())
        self.assertEqual(listed["HEADLESS-2"], "1920x1080@0.000 960,-540 2.0000")
        # A window maximized there fills exactly its pixels.
        window = self.vela.open_window(300, 200)
        start = self.vela.state().window(window)
        grab_x, grab_y = start["x"] + 50, start["y"] + 50
        self.vela.input("move", grab_x, grab_y, "keydown", "super", "down", "sleep", "50",
                        "move", 1200, -300, "sleep", "100", "up", "keyup", "super")
        self.vela.wait_for(lambda s: s.window(window)["output"] == "HEADLESS-2", what="on the second output")
        self.vela.keys("super+up")
        state = self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")
        maximized = state.window(window)
        self.assertEqual((maximized["x"], maximized["w"]), (960, 960))

    def settings_preview(self):
        executable = os.path.join(BUILD, "settings", "vela-displays-test")
        if not os.path.exists(executable):
            self.skipTest("native Settings backend test client not built")
        process = subprocess.Popen([executable, "--preview-client", self.vela.display],
                                   env=self.vela.client_env, stdout=subprocess.DEVNULL,
                                   stderr=self.vela.log)
        self.vela.clients.append(process)
        self.vela.wait_for(lambda s: s.output["scale"] == 1.5,
                           what="native Settings preview applied")
        return process

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
