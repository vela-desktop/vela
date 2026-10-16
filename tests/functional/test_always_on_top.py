# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Always on top: the window stays above normal ones when another is raised,
the socket command and the window action agree, the flag is gone when the
window closes, and fullscreen windows ignore it."""

import socket
import unittest

from harness import Session, tool


class AlwaysOnTop(unittest.TestCase):
    def setUp(self):
        self.vela = Session()
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def status(self):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(self.vela.socket_path)
            connection.sendall(b"always-on-top status\n")
            return connection.recv(16).decode().strip()

    def z(self, window):
        return self.vela.state().window(window)["z"]

    def test_stays_above_a_raised_window(self):
        a = self.vela.open_window(300, 200)
        b = self.vela.open_window(300, 200)
        self.vela.command(f"window {a} activate")
        self.vela.wait_for(lambda s: s["focused"] == a, what="a active")
        self.vela.command("always-on-top on")
        self.vela.wait_for(lambda s: s.window(a)["alwaysOnTop"], what="a always on top")
        self.assertEqual(self.status(), "on")
        self.vela.command(f"window {b} activate")
        self.vela.wait_for(lambda s: s["focused"] == b, what="b active")
        self.assertGreater(self.z(a), self.z(b))
        # Back to normal: raising b again puts it on top.
        self.vela.command(f"window {a} always-on-top off")
        self.vela.wait_for(lambda s: not s.window(a)["alwaysOnTop"], what="a normal")
        self.vela.command(f"window {a} activate")
        self.vela.command(f"window {b} activate")
        self.vela.wait_for(lambda s: s["focused"] == b, what="b active")
        self.assertGreater(self.z(b), self.z(a))

    def test_toggle_and_close_resets(self):
        a = self.vela.open_window(300, 200)
        self.vela.command("always-on-top toggle")
        self.vela.wait_for(lambda s: s.window(a)["alwaysOnTop"], what="on")
        self.vela.command("always-on-top toggle")
        self.vela.wait_for(lambda s: not s.window(a)["alwaysOnTop"], what="off")
        self.vela.command("always-on-top on")
        self.vela.wait_for(lambda s: s.window(a)["alwaysOnTop"], what="on")
        self.vela.command(f"window {a} close")
        self.vela.wait_for(lambda s: not s.has(a), what="closed")
        c = self.vela.open_window(300, 200)
        self.assertFalse(self.vela.state().window(c)["alwaysOnTop"])
        self.assertEqual(self.status(), "off")

    def test_fullscreen_window_is_above_anyway(self):
        a = self.vela.open_window(300, 200)
        self.vela.command("always-on-top on")
        self.vela.wait_for(lambda s: s.window(a)["alwaysOnTop"], what="on")
        f = self.vela.open_window(command=[tool("vela-pattern"), "--fullscreen"])
        state = self.vela.wait_for(lambda s: s.window(f)["fullscreen"], what="fullscreen")
        self.assertFalse(state.window(f)["alwaysOnTop"])
        self.assertTrue(state.window(a)["alwaysOnTop"])
        self.assertEqual(state.window(f)["z"], -1)  # the fullscreen layer, above the windows


if __name__ == "__main__":
    unittest.main()
