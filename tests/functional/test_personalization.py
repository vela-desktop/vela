# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Personalization's choices in vela-shell.conf, as the real Qt shell
follows them live: Settings writes the file, the shell rereads it (Config).

Each test writes the keys as Settings would (an atomic replace) and checks
the pixels of the surface they change against the same surface before.
"""

import os
import tempfile
import time
import unittest

from harness import BUILD, Session
import test_shell

LOCK = os.path.join(BUILD, "lock", "vela-lock")
# A distribution whose logo is the session's own icon (test_start_icon).
OS_RELEASE = os.path.join(tempfile.mkdtemp(prefix="vela-os-release-"), "os-release")
with open(OS_RELEASE, "w") as release:
    release.write('NAME="Test Linux"\nLOGO=vela-test-logo\n')


def square(color):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><rect width="10" height="10" fill="{color}"/></svg>'


class Personalization(test_shell.RealShell):
    # The clock stopped (and carets that don't blink): its pixels change
    # only with what a test chooses.
    shell_env = {"VELA_STILL": "2026-01-14T13:45:07", "VELA_OS_RELEASE": OS_RELEASE}

    def write_shell_conf(self, text):
        """vela-shell.conf replaced as QSettings does, then the shell's
        debounce (Config) gives it time to reread it."""
        path = os.path.join(self.vela.env["XDG_CONFIG_HOME"], "Vela", "vela-shell.conf")
        with open(path + ".new", "w") as file:
            file.write(text)
        os.replace(path + ".new", path)

    def region(self, x, y, w, h, output=0):
        """These logical pixels of the output, as rows of bytes (scale 1)."""
        image = self.vela.pixels(output=output)
        return [row[x * 3:(x + w) * 3] for row in image.rows[y:y + h]]

    def wait_region(self, box, condition, what, timeout=8):
        found = None

        def check(_):
            nonlocal found
            found = self.region(*box)
            return condition(found)
        self.vela.wait_for(check, timeout=timeout, read_state=False, what=what)
        return found

    def settled_region(self, box, still=1.0, timeout=15):
        """The region once it stops changing for `still` seconds: at startup
        the bar takes the wallpaper's tint and the tray fills in."""
        deadline = time.monotonic() + timeout
        last, since = self.region(*box), time.monotonic()
        while time.monotonic() < deadline:
            time.sleep(0.25)
            now = self.region(*box)
            if now != last:
                last, since = now, time.monotonic()
            elif time.monotonic() - since >= still:
                return now
        self.fail(f"the region {box} never settled")

    def clock_box(self):
        """The taskbar's clock and the corner after it: the bar's right end."""
        output = self.vela.state().output
        return (output["w"] - 220, output["h"] - 48, 220, 48)

    def toggles_clock(self, key, off, on):
        """The clock's pixels with the key at `off`, then back at `on`: they
        change, and come back exactly."""
        box = self.clock_box()
        before = self.settled_region(box)
        self.write_shell_conf(f"[{key.split('/')[0]}]\n{key.split('/')[1]}={off}\n")
        changed = self.wait_region(box, lambda r: r != before, f"the clock with {key}={off}")
        self.write_shell_conf(f"[{key.split('/')[0]}]\n{key.split('/')[1]}={on}\n")
        self.wait_region(box, lambda r: r == before, f"the clock with {key}={on} again")
        return before, changed

    def test_hiding_the_pins_moves_the_selection(self):
        # Start open, a pinned tile selected, then Settings hides the pins:
        # Enter launches the first of all apps, not the hidden pinned one.
        # Each app only touches a file of its own.
        applications = os.path.join(self.vela.env["XDG_DATA_HOME"], "applications")
        os.makedirs(applications, exist_ok=True)
        pinned = os.path.join(self.vela.directory, "launched-pinned")
        first = os.path.join(self.vela.directory, "launched-first")
        for name, title, marker in (("vela-files.desktop", "Pinned Files", pinned),
                                    ("aaaa-first.desktop", "AAAA First", first)):
            with open(os.path.join(applications, name), "w") as file:
                file.write(f"[Desktop Entry]\nType=Application\nName={title}\nExec=/usr/bin/touch {marker}\n")
        time.sleep(1.0) # the app list sees them
        # One keyboard throughout (a new one would close Start): Start, the
        # first key selects the first pinned tile; the pins go; Enter.
        process = self.background_input("key", "super", "sleep", 1500, "key", "Right", "sleep", 3000,
                                        "key", "Return")
        self.vela.wait_for(lambda s: self.layer(s, "vela-start-menu"), what="Start open")
        time.sleep(2.0) # past the Right key
        self.write_shell_conf("[start]\nshowPinned=false\n")
        process.wait(timeout=15)
        self.vela.wait_for(lambda _: os.path.exists(first) or os.path.exists(pinned), timeout=8,
                           read_state=False, what="an app launched")
        time.sleep(0.5)
        self.assertTrue(os.path.exists(first), "the first of all apps didn't launch")
        self.assertFalse(os.path.exists(pinned), "Enter launched the hidden pinned app")
        self.check_desktop()

    def test_the_clock_hides_its_date(self):
        # Without the date the time sits alone, in the middle of the bar.
        before, without = self.toggles_clock("taskbar/showDate", "false", "true")
        # The date's line is gone: under the clock's middle, in its last
        # 100 pixels (the tray's icons are before them), hardly any colors
        # are left of the text's.
        def colors(rows):
            return {row[x:x + 3] for row in rows[28:42] for x in range(120 * 3, 212 * 3, 3)}
        self.assertLess(len(colors(without)), len(colors(before)) / 3, "the date's line is still drawn")
        self.check_desktop()

    def test_the_clock_turns_12_hour(self):
        # 13:45 reads 1:45 PM.
        self.toggles_clock("clock/hours", "12", "24")
        self.check_desktop()

    def test_the_clock_shows_seconds(self):
        # 13:45 reads 13:45:07, the clock wider.
        self.toggles_clock("taskbar/clockSeconds", "true", "false")
        self.check_desktop()

    def test_the_clock_hides_the_number_of_notifications(self):
        # One notification: "1" by the clock, until Settings hides it.
        test_shell.QtShell.send_notification(self, "Counted", actions="[]", expire_timeout=1500)
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-notifications"), timeout=10, what="the popup gone")
        box = self.clock_box()
        counted = self.settled_region(box)
        self.write_shell_conf("[taskbar]\nnotificationCount=false\n")
        hidden = self.wait_region(box, lambda r: r != counted, "the clock without the number")
        # "Do not disturb"'s bell isn't a count: it shows anyway.
        self.write_shell_conf("[taskbar]\nnotificationCount=false\n[notifications]\ndoNotDisturb=true\n")
        self.wait_region(box, lambda r: r != hidden and r != counted, "the muted bell by the clock")
        self.write_shell_conf("[taskbar]\nnotificationCount=true\n")
        self.wait_region(box, lambda r: r == counted, "the number back")
        self.check_desktop()

    def test_the_notification_center_follows_the_time_format(self):
        # A notification's time in the center reads as the taskbar's clock.
        test_shell.QtShell.send_notification(self, "Time format", actions="[]", expire_timeout=1500)
        self.vela.wait_for(lambda s: self.layer(s, "vela-notifications"), timeout=8, what="the popup shown")
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-notifications"), timeout=10, what="the popup gone")

        def center():
            self.vela.input("key", "super+n")
            state = self.vela.wait_for(lambda s: self.layer(s, "vela-notification-center"), what="the center open")
            time.sleep(1.0) # its entrance over
            layer = self.layer(state, "vela-notification-center")
            # Its first notification's row, under the header.
            shot = self.region(layer["x"], layer["y"], layer["w"], min(layer["h"], 200))
            self.vela.input("key", "Escape")
            self.vela.wait_for(lambda s: not self.mapped(s, "vela-notification-center"), timeout=5,
                               what="the center closed")
            return shot

        hour24 = center()
        self.write_shell_conf("[clock]\nhours=12\n")
        time.sleep(1.0)
        self.assertTrue(center() != hour24, "the notification's time didn't change")
        self.write_shell_conf("[clock]\nhours=24\n")
        time.sleep(1.0)
        self.assertTrue(center() == hour24, "the notification's time didn't come back")
        self.check_desktop()

    def taskbar_has(self, test):
        """Whether some pixel of the taskbar passes the test (r, g, b)."""
        output = self.vela.state().output
        rows = self.region(0, output["h"] - 48, output["w"], 48)
        return any(test(row[x], row[x + 1], row[x + 2]) for row in rows for x in range(0, len(row), 3))

    def test_start_icon(self):
        # Vela's sail, then a picture (red), the distribution's logo (green,
        # os-release's LOGO= found as a loose file), and the sail again.
        red = lambda r, g, b: r > 200 and g < 40 and b < 40
        green = lambda r, g, b: g > 200 and r < 40 and b < 40
        picture = os.path.join(self.vela.directory, "Pictures", "start.svg")
        with open(picture, "w") as file:
            file.write(square("#ff0000"))
        icons = os.path.join(self.vela.env["XDG_DATA_HOME"], "icons")
        os.makedirs(icons, exist_ok=True)
        with open(os.path.join(icons, "vela-test-logo.svg"), "w") as file:
            file.write(square("#00ff00"))
        output = self.vela.state().output
        bar = (0, output["h"] - 48, output["w"], 48)
        sail = self.settled_region(bar)
        self.assertFalse(self.taskbar_has(red) or self.taskbar_has(green), "the bar already has the colors")

        self.write_shell_conf(f"[start]\nicon=picture\niconPicture={picture}\n")
        self.vela.wait_for(lambda _: self.taskbar_has(red), timeout=8, read_state=False, what="the picture on Start")
        self.write_shell_conf(f"[start]\nicon=distribution\niconPicture={picture}\n")
        self.vela.wait_for(lambda _: self.taskbar_has(green) and not self.taskbar_has(red), timeout=8,
                           read_state=False, what="the distribution's logo on Start")
        # A picture that is gone: the sail, not an empty button.
        os.remove(picture)
        self.write_shell_conf(f"[start]\nicon=picture\niconPicture={picture}\n")
        self.wait_region(bar, lambda r: r == sail, "the sail back")
        self.check_desktop()

    def test_start_picture_follows_its_file(self):
        # The picture chosen is edited or deleted, Settings writes nothing:
        # the button shows the new image, then the sail.
        red = lambda r, g, b: r > 200 and g < 40 and b < 40
        blue = lambda r, g, b: b > 200 and r < 40 and g < 40
        picture = os.path.join(self.vela.directory, "Pictures", "start.svg")
        with open(picture, "w") as file:
            file.write(square("#ff0000"))
        output = self.vela.state().output
        bar = (0, output["h"] - 48, output["w"], 48)
        sail = self.settled_region(bar)
        self.write_shell_conf(f"[start]\nicon=picture\niconPicture={picture}\n")
        self.vela.wait_for(lambda _: self.taskbar_has(red), timeout=8, read_state=False, what="the picture on Start")
        with open(picture, "w") as file: # painted over in place
            file.write(square("#0000ff"))
        self.vela.wait_for(lambda _: self.taskbar_has(blue) and not self.taskbar_has(red), timeout=8,
                           read_state=False, what="the new image on Start")
        os.remove(picture)
        self.wait_region(bar, lambda r: r == sail, "the sail once the picture is gone")
        self.check_desktop()

    def test_desktop_icons_follow_settings(self):
        # A file on the desktop; Settings > Background hides, then shows it.
        # The shell read desktop/showIcons only at startup before, and the
        # desktop's own menu was the only switch.
        desktop = os.path.join(self.vela.directory, "Desktop")
        with open(os.path.join(desktop, "notes.txt"), "w") as file:
            file.write("hello\n")
        box = (0, 0, 240, 320) # the first column of icons
        self.write_shell_conf("[desktop]\nshowIcons=false\n")
        time.sleep(1.0)
        hidden = self.region(*box)
        self.write_shell_conf("[desktop]\nshowIcons=true\n")
        self.wait_region(box, lambda r: r != hidden, "the desktop icon shown")
        self.write_shell_conf("[desktop]\nshowIcons=false\n")
        self.wait_region(box, lambda r: r == hidden, "the desktop icon hidden again")
        self.check_desktop()

    def open_start(self):
        self.vela.input("key", "super")
        state = self.vela.wait_for(lambda s: self.layer(s, "vela-start-menu"), what="Start open")
        time.sleep(0.8) # its entrance over
        return self.layer(state, "vela-start-menu")

    def close_start(self):
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-start-menu"), timeout=5, what="Start closed")

    def test_start_hides_its_pinned_apps(self):
        # Vela Files is pinned to Start by default: a .desktop of that name
        # in this session's data puts a tile there. With start/showPinned
        # off, Start opens on all apps: the tiles under the search differ.
        applications = os.path.join(self.vela.env["XDG_DATA_HOME"], "applications")
        os.makedirs(applications, exist_ok=True)
        with open(os.path.join(applications, "vela-files.desktop"), "w") as file:
            file.write("[Desktop Entry]\nType=Application\nName=Pinned Files\nExec=true\nIcon=system-file-manager\n")
        time.sleep(1.0) # the app list sees it

        def middle():
            layer = self.open_start()
            # Between the search box (its caret blinks) and the footer.
            box = (layer["x"], layer["y"] + layer["h"] // 4, layer["w"], layer["h"] // 2)
            shot = self.region(*box)
            self.close_start()
            return shot

        shown = middle()
        self.write_shell_conf("[start]\nshowPinned=false\n")
        time.sleep(1.0)
        hidden = middle()
        self.assertTrue(shown != hidden, "Start looks the same with its pinned apps hidden")
        self.write_shell_conf("[start]\nshowPinned=true\n")
        time.sleep(1.0)
        self.assertTrue(middle() == shown, "the pinned apps didn't come back")
        self.check_desktop()


@unittest.skipUnless(os.path.exists(LOCK), "vela-lock not built")
class LockClock(unittest.TestCase):
    """The real lock screen's clock, in the format of the taskbar's. Nothing
    is typed: PAM is never asked."""

    def time_width(self, hours, size="1920x1080@60"):
        """How wide the lock screen's time is drawn with clock/hours=."""
        with Session(size=size, env={"VELA_LOCK": LOCK}) as vela:
            folder = os.path.join(vela.env["XDG_CONFIG_HOME"], "Vela")
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, "vela-shell.conf"), "w") as file:
                file.write(f"[clock]\nhours={hours}\n")
            vela.command("lock")
            output = vela.wait_for(lambda s: s["locked"], what="locked").output

            width = 0

            def drawn(_):
                nonlocal width
                image = vela.pixels()
                # The time's box (lock/main.cpp): from 12% of the height, 150
                # tall; its text is near white over the dimmed wallpaper.
                top = int(output["h"] * 0.12)
                columns = [x for x in range(image.width)
                           if any(min(image.at(x, y)) > 200 for y in range(top + 20, top + 130, 3))]
                width = columns[-1] - columns[0] if columns else 0
                return width > 0
            vela.wait_for(drawn, timeout=10, read_state=False, what="the lock screen's time")
            time.sleep(0.5) # its first frames over
            drawn(None)
            return width

    def test_the_lock_screen_follows_the_time_format(self):
        # "1:45 PM" is wider than "13:45" whatever the time: AM or PM after it.
        self.assertGreater(self.time_width(12), self.time_width(24) + 60)

    def test_the_lock_screen_time_fits_a_narrow_output(self):
        # 300 wide: a 12-hour time at 120 pixels (about 340 wide) would be
        # cut at both sides.
        self.assertLessEqual(self.time_width(12, size="300x700@60"), 300 * 0.9 + 2)


if __name__ == "__main__":
    unittest.main()
