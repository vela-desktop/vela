# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("corners", Path(__file__).parents[1] / "check-corners.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


def item(x, y, w, h, radius, **more):
    """An item of the dump: a drawn Rectangle unless told otherwise."""
    value = {"type": "QQuickRectangle", "id": more.pop("id", ""), "name": "", "file": "Test.qml", "owner": "",
             "x": x, "y": y, "w": w, "h": h, "radii": [radius] * 4 if not isinstance(radius, list) else radius,
             "alpha": 1.0, "border": 0.0, "borderAlpha": 0.0, "opacity": 1.0, "picture": False}
    value.update(more)
    return value


def kinds(*items):
    return [(f[0], f[2], f[5]) for f in c.findings(list(items))]


PANEL = item(0, 0, 300, 200, 28, id="panel")


class Nested(unittest.TestCase):
    def test_the_outer_radius_less_the_gap_passes(self):
        self.assertEqual(kinds(PANEL, item(12, 12, 100, 50, 16, id="tile")), [])

    def test_another_radius_in_the_corner_is_reported(self):
        self.assertEqual(kinds(PANEL, item(12, 12, 100, 50, 4, id="tile")), [("nested", "Test.qml#tile", 4)])
        # The same radius inside as outside: the usual mistake.
        self.assertEqual(kinds(PANEL, item(12, 12, 100, 60, 28, id="tile")), [("nested", "Test.qml#tile", 28)])

    def test_every_corner_it_reaches(self):
        full = item(8, 8, 284, 184, 4, id="card")
        self.assertEqual([f[7] for f in c.findings([PANEL, full])],
                         ["top left", "top right", "bottom left", "bottom right"])

    def test_out_of_the_corners_reach_is_its_own(self):
        self.assertEqual(kinds(PANEL, item(28, 28, 100, 50, 4, id="far")), [])
        self.assertEqual(kinds(PANEL, item(100, 12, 100, 50, 4, id="middle")), [])
        # A square outer shape asks nothing.
        self.assertEqual(kinds(item(0, 0, 300, 200, 0), item(4, 4, 100, 50, 9)), [])

    def test_unequal_gaps_take_any_radius_between(self):
        for radius, fine in ((4, True), (11, True), (18, True), (2, False), (24, False)):
            found = kinds(PANEL, item(24, 10, 60, 60, radius, id="button"))
            self.assertEqual(found == [], fine, radius)

    def test_pills_and_circles_are_their_own_shapes(self):
        circle = item(4, 4, 32, 32, 16, id="avatar")
        self.assertEqual(kinds(PANEL, circle), [])
        self.assertEqual([f[2] for f in c.findings([PANEL, circle], pills=True)], ["Test.qml#avatar"])

    def test_the_smallest_shape_around_decides(self):
        card = item(8, 8, 200, 150, 20, id="card")
        # Right for the card (20 less 4), though not for the panel behind it.
        self.assertEqual(kinds(PANEL, card, item(12, 12, 40, 30, 16, id="chip")), [])

    def test_a_hover_fill_counts_though_clear(self):
        clear = item(4, 4, 292, 30, 4, id="row", alpha=0.0)
        self.assertEqual(kinds(PANEL, clear), [("nested", "Test.qml#row", 4)] * 2)
        # Clear and square: not a shape.
        self.assertEqual(kinds(PANEL, item(0, 140, 300, 60, 0, id="footer", alpha=0.0)), [])
        # Not shown: not there.
        self.assertEqual(kinds(PANEL, item(4, 4, 292, 30, 4, opacity=0.0)), [])


class SameBoxAndShadows(unittest.TestCase):
    def test_a_shadow_takes_its_targets_corners(self):
        shadow = item(0, 0, 300, 200, 8, file="Shadow.qml", owner="Panel.qml#shadow/")
        self.assertEqual(kinds(PANEL, shadow), [("shadow", "its Shadow", (8, 8, 8, 8))])
        self.assertEqual(kinds(PANEL, item(0, 0, 300, 200, 28, file="Shadow.qml")), [])

    def test_two_shapes_in_one_box(self):
        self.assertEqual(kinds(PANEL, item(0, 0, 300, 200, 12, id="edge", alpha=0.0, border=1.0, borderAlpha=1.0)),
                         [("same box", "Test.qml#edge", (12, 12, 12, 12))])


class Pictures(unittest.TestCase):
    def picture(self, x, y, w, h, **more):
        return item(x, y, w, h, 0, type="QQuickImage", picture=True, **more)

    def test_a_square_picture_in_a_round_corner(self):
        self.assertEqual(kinds(PANEL, self.picture(6, 6, 288, 120, id="shot")), [("nested", "Test.qml#shot", 0)] * 2)

    def test_a_round_clip_gives_it_corners(self):
        clip = item(6, 6, 288, 120, 22, type="RoundClip_QMLTYPE_3", alpha=0.0)
        self.assertEqual(kinds(PANEL, clip, self.picture(6, 6, 288, 120, id="shot")), [])
        # Larger than its clip: cut to it.
        self.assertEqual(kinds(PANEL, clip, self.picture(0, 0, 300, 200, id="shot")), [])
        # Smaller than its clip: its corners are still square.
        self.assertEqual(kinds(PANEL, clip, self.picture(9, 6, 282, 120, id="shot")),
                         [("nested", "Test.qml#shot", 0)] * 2)

    def test_icons_are_not_rectangles(self):
        self.assertEqual(kinds(PANEL, self.picture(6, 6, 20, 20)), [])
        self.assertEqual(kinds(PANEL, self.picture(6, 6, 64, 64, file="Icon.qml")), [])


class Directory(unittest.TestCase):
    def test_moments_and_exit_code(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for moment, tile in (("fine", 16), ("wrong", 4), ("wrong-again", 4)):
                (root / moment).mkdir()
                (root / moment / "vela-shell-1.json").write_text(json.dumps(
                    [{"title": "", "items": [PANEL, item(12, 12, 100, 50, tile, id="tile")]}]))
            seen = c.check(root)
            self.assertEqual(len(seen), 1)
            self.assertEqual(next(iter(seen.values())), {"wrong", "wrong-again"})


if __name__ == "__main__":
    unittest.main()
