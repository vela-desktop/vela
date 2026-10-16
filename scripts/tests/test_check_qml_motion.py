# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("motion", Path(__file__).parents[1] / "check-qml-motion.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def reports(qml: str) -> list[tuple[int, str]]:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "Test.qml"
        path.write_text(qml)
        return [(r.line, r.rule) for r in m.check_file(path, "Test.qml")]


class Durations(unittest.TestCase):
    def test_tokens_and_their_expressions_pass(self):
        self.assertEqual(reports("""
Item {
    NumberAnimation { duration: Theme.fast }
    NumberAnimation { duration: Theme.normal * 2; easing.type: Easing.OutCubic }
    NumberAnimation { duration: motion.duration }
    NumberAnimation { duration: 0 }
    property int duration: Theme.slow
    readonly property real duration: effects ? factor : 1
}
"""), [])

    def test_a_literal_is_reported(self):
        self.assertEqual(reports("""
Item {
    NumberAnimation { duration: 150 }
    ColorAnimation {
        duration: visible ? 80 : 0
    }
}
"""), [(3, "duration is not a Theme token"), (5, "duration is not a Theme token")])

    def test_a_comment_explains_the_exception(self):
        self.assertEqual(reports("""
Item {
    NumberAnimation { duration: 50 } // motion: the shake's own rhythm
    // motion: the spinner's turn
    RotationAnimator { duration: 1100 }
}
"""), [])


class StateColors(unittest.TestCase):
    def test_a_state_color_on_a_rectangle_is_reported(self):
        self.assertEqual(reports("""
Item {
    Rectangle {
        color: mouse.containsMouse ? Theme.subtleHover : "transparent"
        border.color: selected ? Theme.accent : Theme.stroke
        MouseArea { id: mouse }
    }
}
"""), [(4, "state color on a Rectangle, not a Fill"), (5, "state border.color on a Rectangle, not a Fill")])

    def test_a_fill_passes(self):
        self.assertEqual(reports("""
Item {
    Fill {
        color: control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control
    }
    background: Fill { color: pressed ? "red" : "blue" }
}
"""), [])

    def test_a_rectangle_with_its_own_fade_passes(self):
        self.assertEqual(reports("""
Rectangle {
    color: hovered ? Theme.controlHover : Theme.control
    Behavior on color { ColorAnimation { duration: Theme.fast } }
}
"""), [])

    def test_a_color_without_states_passes(self):
        self.assertEqual(reports("""
Rectangle {
    color: Theme.light ? "#ffffff" : "#2b2b2b"
    Text { color: "hovered" }
}
"""), [])

    def test_continuation_lines_count(self):
        self.assertEqual(reports("""
Rectangle {
    color: active
        ? Theme.accent
        : mouse.pressed ? Theme.subtlePressed : Theme.subtleRest
}
"""), [(3, "state color on a Rectangle, not a Fill")])

    def test_the_innermost_object_decides(self):
        self.assertEqual(reports("""
Rectangle {
    Fill {
        color: hovered ? Theme.controlHover : Theme.control
    }
    Text { color: hovered ? Theme.text : Theme.textSecondary }
}
"""), [])


class Repository(unittest.TestCase):
    def test_vela_controls_is_clean(self):
        controls = [r for r in m.check() if r.path.startswith("common/controls/")]
        self.assertEqual([str(r) for r in controls], [])


if __name__ == "__main__":
    unittest.main()
