# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Vela's title bar (docs/renderer.md §9) for apps that accept it through
xdg-decoration: 32 high, above the content, with the title written by the
compositor and the three buttons; closing, maximizing with a double click.
It's found on screen, not from the state."""

import glob
import os
import re
import subprocess
import time
import unittest

from harness import Session, tool

BAR = 32
BUTTON = 46


class Decorations(unittest.TestCase):
    def setUp(self):
        self.vela = Session()
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def open(self):
        window = self.vela.open_window(400, 300, decorated=True)
        state = self.vela.wait_for(lambda s: s["focused"] == window)
        w = state.window(window)
        image = self.vela.pixels()
        # From the top, in the column halfway along the title: wallpaper and
        # shadow are black, the bar isn't.
        column = w["x"] + 200
        top = next(y for y in range(image.height) if image.at(column, y) != (0, 0, 0))
        return window, w, image, top

    def test_bar_above_the_content(self):
        window, w, image, top = self.open()
        # The app's content starts right below: vela-pattern encodes the pixel
        # coordinates (R = x, G = y).
        self.assertEqual(image.at(w["x"] + 100, top + BAR), (100, 0, 0))
        self.assertEqual(image.at(w["x"] + 100, top + BAR - 1)[:2] != (100, 0), True)
        # The title ("vela-pattern") is written: antialiasing, several colors.
        self.assertGreaterEqual(len(image.colors(w["x"] + 12, top, w["x"] + 200, top + BAR)), 4)
        # And the button glyphs.
        for i in range(1, 4):
            left = w["x"] + w["w"] - i * BUTTON
            self.assertGreaterEqual(len(image.colors(left, top, left + BUTTON, top + BAR)), 2, f"button {i}")

    def test_close_button(self):
        window, w, image, top = self.open()
        self.vela.input("move", w["x"] + w["w"] - BUTTON // 2, top + BAR // 2, "sleep", "50", "click")
        self.vela.wait_for(lambda s: not s.has(window), what="closed by its close button")

    def test_double_click_on_the_title_maximizes(self):
        window, w, image, top = self.open()
        self.vela.input("move", w["x"] + 150, top + BAR // 2, "sleep", "50", "click", "sleep", "60", "click")
        self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")

    def test_title_drag_does_not_prime_double_click(self):
        # A completed drag followed by a click within 400 ms must never
        # maximize: only two genuine title-bar clicks are a double click.
        window, w, image, top = self.open()
        drag(self.vela, w, 12, 3)
        moved = self.vela.state().window(window)
        self.assertFalse(moved["maximized"])
        self.vela.input("move", moved["x"] + 150, moved["y"] + BAR // 2, "click")
        self.assertFalse(self.vela.state().window(window)["maximized"])


class BarImagesAtEveryPosition(unittest.TestCase):
    """Title, icon and glyphs are drawn at the output's scale, so they are
    copied 1:1 (docs/renderer.md §3.4) wherever the window is: the same raster
    gives the same pixels at every position, also where the window's edge falls
    on half a pixel. Dragged by whole units, the window's edge goes through
    every phase a scale has (at 125%: 0, ¼, ½, ¾ of a pixel)."""

    POSITIONS = 40

    def test_same_pixels_at_every_position(self):
        self.check_positions(None, self.POSITIONS)

    def test_title_with_ink_outside_its_line_box(self):
        # A "j"'s tail left of the pen and marks stacked above a letter: the
        # image grows left and up by whole logical units (text.h), and is
        # copied 1:1 at every position all the same.
        self.check_positions(INK_OUTSIDE, self.POSITIONS // 2)

    def check_positions(self, title, positions):
        for scale in (1.25, 1.5, 1.75, 2.25):
            with self.subTest(scale=scale, title=title), Session(scale=scale, size="2560x1440@60") as vela:
                window = vela.open_window(400, 300, decorated=True, title=title)
                vela.wait_for(lambda s: s["focused"] == window)
                reference = bar_images(vela.pixels(), vela.state().window(window), scale)
                self.assertEqual(len(reference), 4, "title and three glyphs")
                for i in range(1, positions):
                    # Back and forth, so the window stays away from the edges
                    # (and their snap zones).
                    step = 1 + i % 5
                    sign = 1 if (i // 10) % 2 == 0 else -1
                    drag(vela, vela.state().window(window), sign * step, sign * (i % 3))
                    w = vela.state().window(window)
                    self.assertFalse(w["maximized"])
                    images = bar_images(vela.pixels(), w, scale)
                    self.assertEqual(len(images), len(reference), f"position {i}")
                    for name, image, expected in zip(("title", "minimize", "maximize", "close"), images, reference):
                        self.assertTrue(image == expected, f"position {i} ({w['x']}, {w['y']}): the {name} changed")


# A title whose ink leaves its line box: a "j"'s tail on the left, marks
# stacked above the "x" (circumflex, tilde, acute). No spaces, so the bar sees
# it as one image.
INK_OUTSIDE = "jupyter-x\u0302\u0303\u0301"


class TitleLettersStayPut(unittest.TestCase):
    """Marks stacked above a letter make the title's image taller (text.h): the
    image moves up by whole logical units, so the line box, and every letter
    in it, stays on the pixels it has without the marks. The same window at
    the same place, titled with and without them, must show the same pixels
    from the middle of the bar down, where no mark reaches."""

    def test_marks_do_not_move_the_letters(self):
        for scale in (1.0, 1.25, 1.5, 1.75, 2.25):
            with self.subTest(scale=scale), Session(scale=scale, size="2560x1440@60") as vela:
                plain = self.lower_half(vela, "xxxx", scale)
                marked = self.lower_half(vela, "x\u0302\u0303\u0301xxx", scale)
                self.assertEqual(plain[0], marked[0], "the window opened elsewhere")
                self.assertTrue(plain[1] == marked[1], "the marks moved the letters below them")

    def lower_half(self, vela, title, scale):
        """The window's place and the bar's pixels from its middle row down,
        across its width; the window is closed afterwards."""
        window = vela.open_window(400, 300, decorated=True, title=title)
        vela.wait_for(lambda s: s["focused"] == window)
        w = vela.state().window(window)
        image = vela.pixels()
        top = bar_top(image, w, scale)
        middle = top + round(BAR / 2 * scale)
        left = round((w["x"] + 9) * scale)
        right = round((w["x"] + w["w"] - 3 * BUTTON - 4) * scale)
        rows = tuple(image.rows[y][left * 3:right * 3] for y in range(middle, top + int(BAR * scale) - 2))
        vela.command(f"window {window} close")
        vela.wait_for(lambda s: not s.has(window), what="closed")
        return (w["x"], w["y"]), rows


class TitleAcrossTheOutputEdge(unittest.TestCase):
    """Dragged partly off the output's left edge, the window moves its title
    by as many pixels as the window: "jupyter-lab"'s image starts left of the
    line box (the "j"'s tail, text.h), so its left edge crosses the output's
    edge before the letters do, and the letters must not jump a pixel then
    (geometry.h, vela_edges_to_pixels). The columns both positions show must
    match, shifted by the window's move."""

    def test_the_letters_move_with_the_window(self):
        for scale, positions in ((1.5, (-15, -13, -11, -9, -7)), (1.25, (-20, -16, -12, -8, -4))):
            with self.subTest(scale=scale), Session(scale=scale, size="1920x1080@60") as vela:
                window = vela.open_window(400, 300, decorated=True, title="jupyter-lab")
                vela.wait_for(lambda s: s["focused"] == window)
                previous = None
                for x in positions:
                    move_to(vela, window, x)
                    w = vela.state().window(window)
                    image = vela.pixels()
                    rows = range(round(w["y"] * scale) + 2, round((w["y"] + BAR) * scale) - 2)
                    bar = [image.rows[y] for y in rows]
                    if previous is not None:
                        shift = round((x - previous[0]) * scale)
                        width = round(120 * scale)
                        before = [row[:width * 3] for row in previous[1]]
                        after = [row[shift * 3:(shift + width) * 3] for row in bar]
                        self.assertTrue(before == after, f"from x={previous[0]} to x={x}: the title moved"
                                        f" other than {shift} pixels")
                    previous = (x, bar)


def move_to(vela, window, x):
    """Drags the window by its bar until its left edge is at `x` units."""
    for _ in range(3):
        w = vela.state().window(window)
        if w["x"] == x:
            return
        drag(vela, w, x - w["x"], 0)
    raise AssertionError(f"the window doesn't go to x={x}")


class TitleFollowsTheWidth(unittest.TestCase):
    """A new width alone draws the title again only when it would cut it at
    another glyph (decoration.c; compositor.text checks the widths a title
    stays the same at). Narrowed and widened by its border, the bar must
    still show what a fresh drawing shows: never a stale title."""

    def test_cut_and_whole_again(self):
        for scale in (1.0, 1.25):
            with self.subTest(scale=scale), Session(scale=scale, size="1920x1080@60") as vela:
                window = vela.open_window(400, 300, decorated=True)
                vela.wait_for(lambda s: s["focused"] == window)
                whole = title_image(vela, window, scale)
                # The width that cuts the title about halfway: its room is the
                # bar minus the margin, the three buttons and 8 units
                # (decoration.c), and the title is about as wide as its ink.
                title = len(whole[0]) // 3 / scale
                cut_width = BEFORE_TITLE_ROOM + round(title / 2)
                # 40 narrower: still room for "vela-pattern", the same pixels.
                resize_right(vela, window, -40)
                self.assertTrue(title_image(vela, window, scale) == whole, "narrower, with room: the title changed")
                # Too narrow for it: cut with an ellipsis, narrower.
                resize_right(vela, window, cut_width - 360)
                cut = title_image(vela, window, scale)
                self.assertFalse(cut == whole, "too narrow: the title wasn't cut")
                self.assertLess(len(cut[0]), len(whole[0]), "too narrow: the title isn't narrower")
                # A little wider, still too narrow: never the whole title.
                resize_right(vela, window, 6)
                self.assertFalse(title_image(vela, window, scale) == whole, "still too narrow: the whole title")
                # Back to the first width: the whole title, the same pixels.
                resize_right(vela, window, 400 - cut_width - 6)
                self.assertTrue(title_image(vela, window, scale) == whole, "wide again: not the whole title")


# A title's room ends this far from the bar's right edge: three buttons and 8
# units (decoration.c), and starts 12 from its left (no icon).
BEFORE_TITLE_ROOM = 12 + 3 * BUTTON + 8


def resize_right(vela, window, dx):
    """Drags the window's right border by `dx` units (its invisible border, 4
    units outside the edge, at half height)."""
    w = vela.state().window(window)
    x = w["x"] + w["w"] + 4
    y = w["y"] + w["h"] // 2
    vela.input("move", x, y, "sleep", "50", "down", "sleep", "50", "move", x + dx // 2, y, "sleep", "50",
               "move", x + dx, y, "sleep", "100", "up")
    vela.wait_for(lambda s: s.window(window)["w"] == w["w"] + dx, what=f"{dx} wider")
    vela.wait_still(window)


def title_image(vela, window, scale):
    """The title's pixels (the first of the bar's images: vela-pattern has no
    icon), once the bar has settled. The bar's color is read just left of the
    buttons, where a title never reaches whatever the width."""
    previous = None
    for _ in range(20):
        w = vela.state().window(window)
        images = bar_images(vela.pixels(), w, scale, column=w["x"] + w["w"] - 3 * BUTTON - 4)
        if images and images == previous:
            return images[0]
        previous = images
        time.sleep(0.05)
    raise AssertionError("the title bar doesn't settle")


def bar_top(image, w, scale, column=None):
    """The bar's first row, in pixels: above it, wallpaper and shadow are black.
    Read in `column` (units; by default 200 into the window)."""
    column = round((w["x"] + 200 if column is None else column) * scale)
    return next(y for y in range(image.height) if image.at(column, y) != (0, 0, 0))


def drag(vela, w, dx, dy):
    """Drags the window by its bar by (dx, dy) units. The drag starts past 4
    units, where the window is grabbed: from there the cursor moves by (dx, dy)."""
    x = w["x"] + 200
    y = w["y"] + BAR // 2
    # Exercise the actual decoration/title-bar drag, as before. Vela now
    # excludes completed drags from double-click detection, so there is no
    # need to wait an additional 400 ms between the 156 moves.
    vela.input("move", x, y, "sleep", "50", "down", "sleep", "50",
               "move", x, y + 10, "sleep", "50",
               "move", x + dx, y + 10 + dy, "sleep", "50", "up")
    def moved(state):
        now = state.window(w["id"])
        return abs(now["x"] - w["x"] - dx) <= 1 and abs(now["y"] - w["y"] - dy) <= 1
    vela.wait_for(moved, what=f"the window moved by {dx}, {dy}")


def bar_images(image, w, scale, column=None):
    """The bar's images (icon, title, glyphs), each cropped to its own pixels:
    runs of columns with something drawn on the bar's color, separated by at
    least 5 units. Away from the rounded corners and from the content. The
    bar's color is read in `column` (units; by default 200 into the window)."""
    column = w["x"] + 200 if column is None else column
    top = bar_top(image, w, scale, column)
    middle = top + round(BAR / 2 * scale)
    background = image.at(round(column * scale), middle)
    left = right = round(column * scale)
    while image.at(left - 1, middle) != (0, 0, 0):
        left -= 1
    while image.at(right, middle) != (0, 0, 0):
        right += 1
    rows = range(top + 2, top + int(BAR * scale) - 2)
    columns = range(left + round(9 * scale), right - round(9 * scale))
    drawn = [x for x in columns if any(image.at(x, y) != background for y in rows)]
    runs = []
    for x in drawn:
        if runs and x - runs[-1][1] <= 5 * scale:
            runs[-1][1] = x
        else:
            runs.append([x, x])
    crops = []
    for x0, x1 in runs:
        used = [y for y in rows if any(image.at(x, y) != background for x in range(x0, x1 + 1))]
        crops.append(tuple(image.rows[y][x0 * 3:(x1 + 1) * 3] for y in range(used[0], used[-1] + 1)))
    return crops


if __name__ == "__main__":
    unittest.main()


class ShellColors(unittest.TestCase):
    """docs/visual-coherence.md §6.6: the compositor draws with the colors the
    shell computes. The accent tints the snap preview (with rounded corners),
    `mica` sets the bars' backgrounds, and the buttons' hover fades."""

    def test_the_snap_preview_takes_the_accent(self):
        with Session() as vela:
            vela.command("theme dark dark #ff0000")
            window = vela.open_window(400, 300, decorated=True)
            w = vela.wait_for(lambda s: s["focused"] == window).window(window)
            x, y = w["x"] + 200, w["y"] + BAR // 2
            # Dragged to the left edge and held there while we look.
            drag = subprocess.Popen([tool("vela-input"), "move", str(x), str(y), "sleep", "300", "down", "sleep", "50",
                                     "move", str(x), str(y + 10), "sleep", "50", "move", "0", "400", "sleep", "1500",
                                     "move", "960", "400", "sleep", "50", "up"], env=vela.client_env)
            try:
                time.sleep(1.0)
                image = vela.pixels()
            finally:
                drag.wait(timeout=10)
            # Black below it: the accent at the preview's 28%, blended in
            # linear light and stored in sRGB.
            red, green, blue = image.at(700, 900)
            expected = round(255 * (1.055 * 0.28 ** (1 / 2.4) - 0.055))
            self.assertTrue(abs(red - expected) <= 2 and green <= 2 and blue <= 2, (red, green, blue, expected))
            # Rounded: the corner itself stays black.
            self.assertEqual(image.at(0, 0), (0, 0, 0))
            self.assertEqual(image.at(20, 20)[0], red)

    def test_a_resting_snap_preview_wakes_nothing(self):
        # Once the preview has grown in, holding the window still at the edge
        # plans no frame: its tick sets the same color again, which is no
        # change to the scene. Each frame the compositor plans is a line of
        # the animation log, drawn or not.
        with Session(env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
            window = vela.open_window(400, 300, decorated=True)
            w = vela.wait_for(lambda s: s["focused"] == window).window(window)
            x, y = w["x"] + 200, w["y"] + BAR // 2
            drag = subprocess.Popen([tool("vela-input"), "move", str(x), str(y), "sleep", "300", "down", "sleep", "50",
                                     "move", str(x), str(y + 10), "sleep", "50", "move", "0", "400", "sleep", "3000",
                                     "move", "960", "400", "sleep", "50", "up"], env=vela.client_env)
            try:
                time.sleep(1.5) # the preview has grown in
                before = vela.log_text().count("animation: frame ")
                time.sleep(1.0)
                after = vela.log_text().count("animation: frame ")
                shown = "animation: snap-preview" in vela.log_text()
            finally:
                drag.wait(timeout=10)
            self.assertTrue(shown, "the preview was shown")
            self.assertEqual(after - before, 0, "frames planned while the preview rests")

    def test_mica_colors_the_bars(self):
        with Session() as vela:
            vela.command("mica #102030 #405060")
            # Side by side, the second one active.
            first = vela.open_window(400, 300, decorated=True)
            vela.wait_for(lambda s: s["focused"] == first)
            vela.keys("super+Left")
            second = vela.open_window(400, 300, decorated=True)
            vela.wait_for(lambda s: s["focused"] == second)
            vela.keys("super+Right")
            time.sleep(0.6) # the morphs

            def spot(window):
                w = vela.state().window(window)
                return w["x"] + w["w"] - 3 * BUTTON - 10, w["y"] + BAR // 2 # between the title and the buttons
            vela.wait_for(lambda _: vela.pixels().at(*spot(second)) == (0x10, 0x20, 0x30), read_state=False,
                          what="the active bar in Mica's active color")
            self.assertEqual(vela.pixels().at(*spot(first)), (0x40, 0x50, 0x60), "the inactive one")
            # Malformed hex must not be partially parsed and applied.
            vela.command("mica #12zzzz #abcdef")
            self.assertEqual(vela.pixels().at(*spot(second)), (0x10, 0x20, 0x30))
            self.assertEqual(vela.pixels().at(*spot(first)), (0x40, 0x50, 0x60))

    def test_bar_images_follow_what_they_are_drawn_from(self):
        # The glyphs take the text's color: from the mode and the window's
        # state, not from Mica, which only recolors the background.
        with Session() as vela:
            vela.command("theme dark dark")
            window = vela.open_window(400, 300, decorated=True)
            w = vela.wait_for(lambda s: s["focused"] == window).window(window)
            minimize = (w["x"] + w["w"] - 3 * BUTTON + BUTTON // 2, w["y"] + BAR // 2)
            background = (w["x"] + w["w"] - 3 * BUTTON - 10, w["y"] + BAR // 2)

            def glyph():
                # The minimize line (one pixel, over two rows: blended with
                # the bar), the pixel farthest from the bar's color.
                image = vela.pixels()
                bar = sum(image.at(*background))
                column = [image.at(minimize[0], minimize[1] + dy) for dy in range(-3, 4)]
                return max(column, key=lambda p: abs(sum(p) - bar))

            def settled(test, what):
                vela.wait_for(lambda _: test(glyph()), read_state=False, what=what)

            settled(lambda p: min(p) > 160, "a white glyph, dark and active")
            vela.command("mica #102030 #405060")
            vela.wait_for(lambda _: vela.pixels().at(*background) == (0x10, 0x20, 0x30), read_state=False,
                          what="Mica's color")
            self.assertGreater(min(glyph()), 160, "Mica leaves the glyph as it is")
            vela.command("theme light light")
            settled(lambda p: max(p) < 120, "a dark glyph, light")
            vela.command("theme dark dark #ff0000")
            settled(lambda p: min(p) > 160, "white again")
            other = vela.open_window(200, 150, decorated=True)
            vela.wait_for(lambda s: s["focused"] == other)
            settled(lambda p: 110 < min(p) and max(p) < 150, "gray, inactive")

    def test_the_close_hover_fades(self):
        for hz in (60, 180):
            with self.subTest(hz=hz), Session(size=f"1920x1080@{hz}", env={"VELA_DEBUG_ANIMATION": "1"}) as vela:
                window = vela.open_window(400, 300, decorated=True)
                w = vela.wait_for(lambda s: s["focused"] == window).window(window)
                close = (w["x"] + w["w"] - BUTTON // 2, w["y"] + BAR // 2)
                away = (w["x"] + 100, w["y"] + 150)

                def fills(after):
                    out = {}
                    for m in FILL.finditer(vela.log_text()[after:]):
                        out.setdefault(int(m[1]), []).append(float(m[2]))
                    return out

                mark = len(vela.log_text())
                vela.input("move", *close)
                vela.wait_for(lambda _: any(v[-1] == 1.0 for v in fills(mark).values()), read_state=False,
                              what="Close red")
                (values,) = fills(mark).values()
                # Every frame a new color, up to the full red within `fast`
                # (120 ms) plus one period.
                rising = values[:values.index(1.0) + 1]
                self.assertTrue(all(b > a for a, b in zip(rising, rising[1:])), values)
                self.assertLessEqual(len(rising) - 1, 120 * hz / 1000 + 1, values)

                # Left halfway: it turns back from where it was.
                vela.input("move", *away)
                time.sleep(0.5)
                mark = len(vela.log_text())
                vela.input("move", *close, "sleep", "40", "move", *away)
                vela.wait_for(lambda _: len(fills(mark)) == 2 and list(fills(mark).values())[1][-1] == 0.0,
                              read_state=False, what="back to none")
                going, back = fills(mark).values()
                self.assertTrue(0.05 < going[-1] < 0.95, going)
                self.assertAlmostEqual(back[0], going[-1], places=3)
                # As for rising, test movement up to the first target sample.
                # Rounded logs and predicted frames can repeat the endpoint
                # before wall-clock retirement; the fade must stay there.
                falling = back[:back.index(0.0) + 1]
                self.assertTrue(all(b < a for a, b in zip(falling, falling[1:])), back)
                self.assertTrue(all(v == 0.0 for v in back[len(falling):]), back)
                self.assertLessEqual(len(falling) - 1, 120 * hz / 1000 + 1, back)


FILL = re.compile(r"animation: close-hover #(\d+) fill ([\d.]+)")


class IconCache(unittest.TestCase):
    """The bar's app icon under a client that keeps changing its app_id
    (docs/memory-allocation-policy.md, "Aggregate growth"): the icon cache
    evicts, a bar still shows the icon whose entry went, and an evicted icon
    is drawn again when asked for. The budget itself is counted by
    compositor.icon-cache and compositor.icons."""

    # More app ids than VELA_ICON_CACHE_BUDGET (256, icon_cache.h).
    CYCLED = 600
    MAGENTA = (255, 0, 255)
    CYAN = (0, 255, 255)

    @staticmethod
    def write_icon(vela, color):
        """The private theme's icon of "vela.test.held": a square of `color`."""
        theme = os.path.join(vela.env["XDG_DATA_HOME"], "icons", "hicolor")
        apps = os.path.join(theme, "scalable", "apps")
        os.makedirs(apps, exist_ok=True)
        with open(os.path.join(theme, "index.theme"), "w") as index:
            index.write("[Icon Theme]\nName=Hicolor\nDirectories=scalable/apps\n\n"
                        "[scalable/apps]\nSize=48\nType=Scalable\n")
        with open(os.path.join(apps, "vela.test.held.svg"), "w") as svg:
            svg.write('<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">'
                      '<rect width="16" height="16" fill="#{:02x}{:02x}{:02x}"/></svg>\n'.format(*color))

    def icon(self, vela, window):
        """The color at the middle of the window's bar icon (scale 1)."""
        image = vela.pixels()
        w = vela.state().window(window)
        top = bar_top(image, w, 1.0)
        return image.at(w["x"] + 12 + 8, top + BAR // 2)

    def wait_for_icon(self, vela, window, color, what):
        def shown(_):
            return all(abs(a - b) <= 2 for a, b in zip(self.icon(vela, window), color))
        vela.wait_for(shown, read_state=False, what=what)

    def test_evicted_icon_stays_on_screen_and_comes_back(self):
        with Session() as vela:
            self.write_icon(vela, self.MAGENTA)
            held = vela.open_window(400, 300, app_id="vela.test.held", decorated=True)
            vela.wait_for(lambda s: s["focused"] == held)
            self.wait_for_icon(vela, held, self.MAGENTA, "the held window's icon")
            # From here a cyan icon has been drawn again: its entry was evicted.
            self.write_icon(vela, self.CYAN)
            before = set(glob.glob(os.path.join(vela.directory, "client-*.log")))
            cycler = vela.open_window(command=[tool("vela-pattern"), "--decorated", "--app-id", "vela.test.held",
                                               "--cycle-app-ids", str(self.CYCLED), "400", "300"])
            (log,) = set(glob.glob(os.path.join(vela.directory, "client-*.log"))) - before

            def cycled(_):
                with open(log, errors="replace") as output:
                    return "vela-pattern: app-ids-cycled" in output.read()
            vela.wait_for(cycled, timeout=60, read_state=False, what="the app ids cycled")
            vela.wait_for(lambda s: s["focused"] == cycler)
            # Back to "vela.test.held", after more app ids than the budget:
            # searched and drawn again.
            self.wait_for_icon(vela, cycler, self.CYAN, "the cycling window's icon, drawn again")
            # The first window's bar was never redrawn: its own copy, whose
            # cache entry has been evicted, is still on screen.
            vela.command(f"window {cycler} close")
            vela.wait_for(lambda s: not s.has(cycler), what="the cycling window closed")
            self.wait_for_icon(vela, held, self.MAGENTA, "the held window's icon, after its entry was evicted")
