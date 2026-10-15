# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("shared", Path(__file__).parents[1] / "check-shared-values.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

MOTION_H = """
#define VELA_MOTION_FAST_MS 120.0
#define VELA_MOTION_NORMAL_MS 200.0
#define VELA_MOTION_SLOW_MS 250.0
#define VELA_MOTION_LONG_MS 300.0
#define VELA_DECELERATE_CURVE 0.0, 0.0, 0.2, 1.0
"""
THEME_QML = """
    readonly property var scale: ({ fast: 120, normal: 200, slow: 250, long: 300 })
    readonly property var decelerate: [0.0, 0.0, 0.2, 1.0, 1.0, 1.0]
"""
PROMPT_CPP = "constexpr int fadeOutMs = 200 + 60;"


class SharedValues(unittest.TestCase):
    def test_the_repository_agrees(self):
        self.assertEqual(m.check(m.MOTION_H.read_text(), m.THEME_QML.read_text(), m.PROMPT_CPP.read_text()), [])

    def test_equal_copies_pass(self):
        self.assertEqual(m.check(MOTION_H, THEME_QML, PROMPT_CPP), [])

    def test_a_different_duration_fails(self):
        failures = m.check(MOTION_H.replace("FAST_MS 120.0", "FAST_MS 150.0"), THEME_QML, PROMPT_CPP)
        self.assertEqual(failures, ["fast: 150 ms in motion.h, 120 in Theme.qml"])

    def test_a_different_curve_fails(self):
        failures = m.check(MOTION_H, THEME_QML.replace("0.2, 1.0, 1.0", "0.1, 1.0, 1.0"), PROMPT_CPP)
        self.assertEqual(len(failures), 1)
        self.assertIn("decelerate", failures[0])

    def test_a_missing_token_fails(self):
        failures = m.check(MOTION_H, THEME_QML.replace(", long: 300", ""), PROMPT_CPP)
        self.assertEqual(len(failures), 1)
        self.assertIn("scale has", failures[0])

    def test_a_prompt_that_exits_before_its_fade_fails(self):
        failures = m.check(MOTION_H, THEME_QML, "constexpr int fadeOutMs = 150 + 20;")
        self.assertEqual(len(failures), 1)
        self.assertIn("fadeOutMs 170", failures[0])


if __name__ == "__main__":
    unittest.main()
