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


if __name__ == "__main__":
    unittest.main()
