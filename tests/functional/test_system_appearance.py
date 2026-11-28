# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The system appearance chosen in Settings (docs/system-appearance.md), as
the compositor applies it at once: the cursor theme and size (kcminputrc, at
startup and on "reload-cursor") and the icon theme of the title bars ("reload-icons",
kdeglobals). Read on screen."""

import os
import shutil
import struct
import tempfile
import time
import unittest

from harness import Image, Session, tool

BAR = 32
ICON_X = 12  # VELA_DECORATION_ICON_X
ICON_SIZE = 16
TITLE_AFTER_ICON = ICON_X + ICON_SIZE + 8  # decoration.c

RED = (255, 0, 0)
BLUE = (0, 0, 255)
YELLOW = (255, 255, 0)
GREEN = (0, 255, 0)  # vela-pattern --cursor surface


def write_cursor_theme(icons, name, colors, delay=0, sizes=(24, 48)):
    """An Xcursor theme in `icons` with one cursor, "default", square, at each
    nominal size of `sizes`, its hotspot at the top left: one frame per color,
    shown `delay` ms each (an animated cursor when there are more)."""
    cursors = os.path.join(icons, name, "cursors")
    os.makedirs(cursors, exist_ok=True)
    with open(os.path.join(icons, name, "index.theme"), "w") as index:
        index.write(f"[Icon Theme]\nName={name}\n")
    image_type = 0xFFFD0002
    header, chunk = 16, 36
    images = [(size, color) for size in sizes for color in colors]
    data = bytearray(struct.pack("<4sIII", b"Xcur", header, 0x10000, len(images)))
    position = header + 12 * len(images)
    for size, _ in images:
        data += struct.pack("<III", image_type, size, position)
        position += chunk + size * size * 4
    for size, color in images:
        data += struct.pack("<IIIIIIIII", chunk, image_type, size, 1, size, size, 0, 0, delay)
        pixel = struct.pack("<I", 0xFF000000 | color[0] << 16 | color[1] << 8 | color[2])
        data += pixel * (size * size)
    with open(os.path.join(cursors, "default"), "wb") as cursor:
        cursor.write(data)


def dimmed(color, background, opacity=0.6):
    """`color` at `opacity` over `background`, blended as the renderer does:
    in linear light."""
    def linear(c):
        c /= 255
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

    def srgb(c):
        c = c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
        return round(c * 255)
    return tuple(srgb(opacity * linear(c) + (1 - opacity) * linear(b)) for c, b in zip(color, background))


def close(a, b, tolerance=3):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


class CursorTheme(unittest.TestCase):
    """reload-cursor: the pointer takes the new theme at once wherever it is,
    an app's own cursor and a hidden one stay as the app asked, and nothing
    of the old theme stays in use (an ASan build turns a use of it into a
    failure)."""

    def session(self, theme="vela-red", env=None, size=None):
        """A session whose kcminputrc chooses `theme` before it starts, with
        the test's cursor themes the only ones it finds (XCURSOR_PATH)."""
        self.base = tempfile.mkdtemp(prefix="vela-cursor-")
        self.addCleanup(shutil.rmtree, self.base, ignore_errors=True)
        config = os.path.join(self.base, "config")
        os.makedirs(config)
        icons = os.path.join(self.base, "icons")
        write_cursor_theme(icons, "vela-red", [RED])
        write_cursor_theme(icons, "vela-blue", [BLUE])
        write_cursor_theme(icons, "vela-blinking", [RED, YELLOW], delay=20)
        self.write_choice(config, theme, size)
        vela = Session(env={"XDG_CONFIG_HOME": config, "XCURSOR_PATH": icons, **(env or {})})
        vela.start()
        self.addCleanup(vela.stop)
        return vela

    @staticmethod
    def write_choice(config, theme, size=None):
        with open(os.path.join(config, "kcminputrc"), "w") as kcminputrc:
            kcminputrc.write(f"[Mouse]\ncursorTheme={theme}\n" + (f"cursorSize={size}\n" if size else ""))

    def choose(self, vela, theme, size=None):
        """What Settings does: kcminputrc written, then "reload-cursor"."""
        self.write_choice(vela.env["XDG_CONFIG_HOME"], theme, size)
        vela.command("reload-cursor")

    def cursor_at(self, vela, x, y, inside=4):
        """The pixel `inside` pixels into the pointer at (x, y), the cursor
        drawn."""
        path = os.path.join(vela.directory, "cursor.png")
        vela.screenshot(path, region=(x + inside, y + inside, 1, 1), cursor=True)
        return Image.read(path).at(0, 0)

    def plain_at(self, vela, x, y, inside=4):
        path = os.path.join(vela.directory, "plain.png")
        vela.screenshot(path, region=(x + inside, y + inside, 1, 1), cursor=False)
        return Image.read(path).at(0, 0)

    def wait_cursor(self, vela, x, y, color, what):
        vela.wait_for(lambda _: close(self.cursor_at(vela, x, y), color), read_state=False, what=what)

    def test_over_the_desktop(self):
        # Written before the compositor first loads it (lazily, on the first
        # pointer image): kcminputrc's theme from the start.
        vela = self.session("vela-red")
        vela.input("move", "300", "300", "sleep", "50", "move", "310", "300")
        self.wait_cursor(vela, 310, 300, RED, "the theme from kcminputrc")
        self.choose(vela, "vela-blue")
        self.wait_cursor(vela, 310, 300, BLUE, "the new theme, without moving")
        vela.input("move", "320", "300")
        self.wait_cursor(vela, 320, 300, BLUE, "the new theme, moving")

    def test_over_a_border_after_an_apps_own_cursor(self):
        # The pointer leaves an app's own cursor for a window border, where
        # the compositor shows a cursor of its own (a resize one: missing from
        # the theme, so its "default"). The new theme replaces that one too,
        # before the old theme goes.
        vela = self.session("vela-red")
        window = vela.open_focused(400, 300, command=[tool("vela-pattern"), "--cursor", "surface", "400", "300"])
        w = vela.state().window(window)
        inside = (w["x"] + 200, w["y"] + 150)
        border = (w["x"] + w["w"] + 3, w["y"] + 150)
        vela.input("move", *map(str, inside), "sleep", "100", "move", str(inside[0] + 5), str(inside[1]))
        self.wait_cursor(vela, inside[0] + 5, inside[1], GREEN, "the app's own cursor")
        vela.input("move", *map(str, border), "sleep", "100", "move", str(border[0] + 1), str(border[1]))
        self.wait_cursor(vela, border[0] + 1, border[1], RED, "the border's cursor")
        self.choose(vela, "vela-blue")
        self.wait_cursor(vela, border[0] + 1, border[1], BLUE, "the border's cursor, new theme")
        # Back inside: the app's cursor; a new theme leaves it alone.
        vela.input("move", *map(str, inside), "sleep", "100", "move", str(inside[0] + 5), str(inside[1]))
        self.wait_cursor(vela, inside[0] + 5, inside[1], GREEN, "the app's own cursor again")
        self.choose(vela, "vela-red")
        time.sleep(0.2)
        self.assertTrue(close(self.cursor_at(vela, inside[0] + 5, inside[1]), GREEN))
        vela.input("move", "100", "100", "sleep", "100", "move", "105", "100")
        self.wait_cursor(vela, 105, 100, RED, "the desktop, in the theme chosen last")

    def test_a_hidden_cursor_stays_hidden(self):
        vela = self.session("vela-red")
        window = vela.open_focused(400, 300, command=[tool("vela-pattern"), "--cursor", "hidden", "400", "300"])
        w = vela.state().window(window)
        x, y = w["x"] + 200, w["y"] + 150
        vela.input("move", str(x), str(y), "sleep", "100", "move", str(x + 5), str(y))
        under = self.plain_at(vela, x + 5, y)
        vela.wait_for(lambda _: self.cursor_at(vela, x + 5, y) == under, read_state=False,
                      what="no cursor over the game")
        self.choose(vela, "vela-blue")
        time.sleep(0.2)
        self.assertEqual(self.cursor_at(vela, x + 5, y), under, "a new theme showed the hidden cursor")

    def test_animated_cursors_and_outputs(self):
        # An animated cursor's timer runs on the old theme's images: replaced
        # while it runs, with outputs coming and going around it.
        vela = self.session("vela-blinking")
        vela.input("move", "300", "300", "sleep", "50", "move", "310", "300")
        self.wait_cursor(vela, 310, 300, RED, "the first frame")
        self.wait_cursor(vela, 310, 300, YELLOW, "the second frame")
        for theme in ("vela-red", "vela-blinking", "vela-blue"):
            self.choose(vela, theme)
            vela.command("test-output add 1280x720")
            time.sleep(0.1)
            vela.command("test-output remove HEADLESS-2")
            time.sleep(0.1)
        self.wait_cursor(vela, 310, 300, BLUE, "the theme chosen last")
        self.assertTrue(vela.state()["outputs"], "the compositor is still there")

    def test_bad_names_are_refused(self):
        # A name that leaves the cursor directories is not loaded: the
        # pointer falls back to XCURSOR_THEME, never to the theme next door.
        vela = self.session("vela-red", env={"XCURSOR_THEME": "vela-blue"})
        write_cursor_theme(self.base, "outside", [YELLOW])
        vela.input("move", "300", "300", "sleep", "50", "move", "310", "300")
        self.wait_cursor(vela, 310, 300, RED, "the theme from kcminputrc")
        for name in ("../outside", ".", ".."):
            self.choose(vela, name)
            self.wait_cursor(vela, 310, 300, BLUE, f"XCURSOR_THEME instead of {name!r}")
            self.choose(vela, "vela-red")
            self.wait_cursor(vela, 310, 300, RED, "the theme from kcminputrc again")

    def test_size(self):
        # 48 from kcminputrc at startup, 24 at once: the pixel 40 in is the
        # cursor's, then no longer.
        vela = self.session("vela-red", size=48)
        vela.input("move", "300", "300", "sleep", "50", "move", "310", "300")
        self.wait_cursor(vela, 310, 300, RED, "the theme from kcminputrc")
        vela.wait_for(lambda _: close(self.cursor_at(vela, 310, 300, inside=40), RED), read_state=False,
                      what="a cursor 48 wide")
        self.choose(vela, "vela-red", 24)
        vela.wait_for(lambda _: not close(self.cursor_at(vela, 310, 300, inside=40), RED, 60), read_state=False,
                      what="a cursor 24 wide")
        self.assertTrue(close(self.cursor_at(vela, 310, 300, inside=20), RED), "still the theme's cursor")
        # Out of range: XCURSOR_SIZE's instead (unset here: 24).
        self.choose(vela, "vela-red", 4096)
        time.sleep(0.2)
        self.assertFalse(close(self.cursor_at(vela, 310, 300, inside=40), RED, 60))
        self.assertTrue(close(self.cursor_at(vela, 310, 300, inside=20), RED))


class IconTheme(unittest.TestCase):
    """reload-icons: every title bar takes the new icon theme's icon, keeps an
    inactive window's dimmer icon, and moves its title when an icon comes or
    goes."""

    @staticmethod
    def write_theme(vela, name, icons):
        """An icon theme in XDG_DATA_HOME with a square SVG per app id."""
        theme = os.path.join(vela.env["XDG_DATA_HOME"], "icons", name)
        apps = os.path.join(theme, "scalable", "apps")
        os.makedirs(apps, exist_ok=True)
        with open(os.path.join(theme, "index.theme"), "w") as index:
            index.write(f"[Icon Theme]\nName={name}\nDirectories=scalable/apps\n\n"
                        "[scalable/apps]\nSize=48\nType=Scalable\n")
        for app_id, color in icons.items():
            with open(os.path.join(apps, f"{app_id}.svg"), "w") as svg:
                svg.write('<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">'
                          '<rect width="16" height="16" fill="#{:02x}{:02x}{:02x}"/></svg>\n'.format(*color))

    @staticmethod
    def choose(vela, theme):
        os.makedirs(vela.env["XDG_CONFIG_HOME"], exist_ok=True)
        with open(os.path.join(vela.env["XDG_CONFIG_HOME"], "kdeglobals"), "w") as config:
            config.write(f"[Icons]\nTheme={theme}\n")

    @staticmethod
    def bar(vela, w):
        """The middle row of the bar of window `w` (scale 1), from its left
        edge, and the bar's own color."""
        image = vela.pixels()
        column = w["x"] + 200
        top = next(y for y in range(image.height) if image.at(column, y) != (0, 0, 0))
        y = top + BAR // 2
        row = [image.at(w["x"] + x, y) for x in range(0, 200)]
        return row, image.at(column + 60, y)

    def first_drawn(self, row, background, start):
        return next((x for x in range(start, len(row)) if not close(row[x], background, 6)), None)

    def test_icons_follow_the_theme(self):
        with Session() as vela:
            self.write_theme(vela, "vela-red", {"vela.test.themed": RED})
            self.write_theme(vela, "vela-blue", {"vela.test.themed": BLUE, "vela.test.late": BLUE})
            self.choose(vela, "vela-red")
            inactive = vela.open_focused(400, 300, app_id="vela.test.themed", decorated=True)
            # Dragged to the left by its bar: the next window opens beside it.
            w = vela.state().window(inactive)
            x, y = w["x"] + 200, w["y"] + BAR // 2
            vela.input("move", x, y, "sleep", "300", "down", "sleep", "50", "move", x, y + 10, "sleep", "50",
                       "move", 250, y + 10, "sleep", "50", "up")
            vela.wait_for(lambda s: s.window(inactive)["x"] < 100, what="the first window on the left")
            late = vela.open_focused(400, 300, app_id="vela.test.late", decorated=True)
            vela.wait_for(lambda s: s.window(late)["x"] > 500, what="the second window beside it")
            time.sleep(0.3)
            state = vela.state()
            self.assertEqual(state["focused"], late)
            w_inactive, w_late = state.window(inactive), state.window(late)

            row, background = self.bar(vela, w_inactive)
            dimmed_red = row[ICON_X + 8]
            # Inactive: the icon at 60% over the bar.
            expected = dimmed(RED, background)
            self.assertTrue(close(dimmed_red, expected, 6), (dimmed_red, expected))
            row, background = self.bar(vela, w_late)
            # No icon in vela-red: the title from the margin.
            title_before = self.first_drawn(row, background, 9)
            self.assertLess(title_before, ICON_X + 4, row[:40])

            self.choose(vela, "vela-blue")
            vela.command("reload-icons")

            def blue_and_dimmed(_):
                row, background = self.bar(vela, w_inactive)
                return close(row[ICON_X + 8], dimmed(BLUE, background), 6)
            vela.wait_for(blue_and_dimmed, read_state=False, what="the inactive window's icon, blue and dimmed")

            def icon_then_title(_):
                row, background = self.bar(vela, w_late)
                if not close(row[ICON_X + 8], BLUE, 8):
                    return False
                title = self.first_drawn(row, background, ICON_X + ICON_SIZE + 2)
                return title is not None and title >= TITLE_AFTER_ICON - 2
            vela.wait_for(icon_then_title, read_state=False,
                          what="the late window's icon, its title moved after it")


if __name__ == "__main__":
    unittest.main()
