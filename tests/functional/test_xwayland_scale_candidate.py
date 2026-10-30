# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live integration test for the KWin-style global XWayland scale candidate.

The output scale is *not* yet applied to X11 surfaces. This test only verifies
the compositor tracks the correct maximum scale after hotplug and reconfigure
without changing Wayland applications' own monitor scales.
"""
import unittest

from harness import Session


class XwaylandScaleCandidate(unittest.TestCase):
    def setUp(self):
        self.vela = Session(scale="HEADLESS-1=1.25,HEADLESS-2=1",
                            size="2560x1440@60",
                            env={"VELA_XWAYLAND_SCALE_TRACE": "1",
                                 "VELA_XWAYLAND_BACKEND": "compat"})
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def test_maximum_output_scale_tracks_hotplug_and_changes(self):
        self.vela.wait_for_log(
            "Xwayland candidate application scale 1.250 (not applied)")
        self.vela.command("test-output add 1920x1080")
        state = self.vela.wait_for(lambda s: len(s["outputs"]) == 2,
                                   what="mixed-scale outputs")
        scales = {o["name"]: o["scale"] for o in state["outputs"]}
        self.assertEqual(scales, {"HEADLESS-1": 1.25, "HEADLESS-2": 1})
        self.vela.randr("--output", "HEADLESS-1", "--scale", "1")
        self.vela.wait_for(
            lambda s: all(o["scale"] == 1 for o in s["outputs"]),
            what="both outputs at 100 percent")
        self.vela.wait_for_log(
            "Xwayland candidate application scale 1.000 (not applied)")
        self.vela.randr("--output", "HEADLESS-2", "--scale", "1.5")
        self.vela.wait_for(
            lambda s: any(o["name"] == "HEADLESS-2" and o["scale"] == 1.5
                          for o in s["outputs"]),
            what="secondary output changed to 150 percent")
        self.vela.wait_for_log(
            "Xwayland candidate application scale 1.500 (not applied)")
        self.vela.command("test-output remove HEADLESS-2")
        self.vela.wait_for(lambda s: len(s["outputs"]) == 1,
                           what="secondary output removed")
        # The original 100% output is the only remaining output.
        # Logs may contain the previous 1.0 update; check the latest one.
        lines = [line for line in self.vela.log_text().splitlines()
                 if "Xwayland candidate application scale " in line]
        self.assertTrue(lines)
        self.assertIn("1.000 (not applied)", lines[-1])
