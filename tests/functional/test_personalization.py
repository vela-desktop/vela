# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Personalization's choices in vela-shell.conf, as the real Qt shell
follows them live: Settings writes the file, the shell rereads it (Config).

Each test writes the keys as Settings would (an atomic replace) and checks
the pixels of the surface they change against the same surface before.
"""

import os
import time
import unittest

import test_shell


class Personalization(test_shell.RealShell):

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


if __name__ == "__main__":
    unittest.main()
