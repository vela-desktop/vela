# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""X11 apps through Xwayland (xwayland.c): a managed window is a window like
any other (centered, with Vela's title bar, maximized and closed from the
keyboard); an override-redirect menu stays where the app puts it. The
window is tools/vela-x11, which draws only on Expose; Xwayland starts with
it, so its window is created while the X window manager is being set up."""

import os
import subprocess
import time
import unittest

from harness import Session, tool

ORANGE = (255, 128, 0)
GREEN = (0, 192, 0)


class X11(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(tool("vela-x11")):
            self.skipTest("vela-x11 not built (needs xcb)")
        self.vela = Session()
        self.vela.start()
        if "Xwayland not available" in self.vela.log_text():
            self.vela.stop()
            self.skipTest("Xwayland not available")
        display = self.vela.wait_for_log("Xwayland on DISPLAY=", lambda line: line.split("DISPLAY=")[1].split()[0])
        self.env = dict(self.vela.client_env, DISPLAY=display)

    def tearDown(self):
        self.vela.stop()

    def open_x11(self, *args):
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen([tool("vela-x11"), *map(str, args)], env=self.env))
        state = self.vela.wait_for(lambda s: {w["id"] for w in s.windows} - before, timeout=15,
                                   what="the X11 window")
        window = next(iter({w["id"] for w in state.windows} - before))
        self.vela.wait_still(window)
        return window

    def test_window_is_managed(self):
        window = self.open_x11(300, 200)
        state = self.vela.wait_for(lambda s: s["focused"] == window, what="the X11 window has focus")
        w = state.window(window)
        out = state.output
        self.assertEqual((w["app"], w["title"]), ("vela.x11", "vela-x11"))
        self.assertEqual(w["w"], 300)
        self.assertEqual(w["x"], out["x"] + (out["w"] - 300) // 2)
        # The app's content below Vela's title bar.
        image = self.vela.pixels()
        self.assertEqual(image.at(w["x"] + 150, w["y"] + w["h"] - 60), ORANGE)

        self.vela.keys("super+Up")
        state = self.vela.wait_for(lambda s: s.window(window)["maximized"], what="maximized")
        self.assertEqual(state.window(window)["w"], out["w"])
        self.vela.keys("super+Down")
        state = self.vela.wait_for(lambda s: not s.window(window)["maximized"], what="restored")
        self.assertEqual(state.window(window)["w"], 300)

        self.vela.keys("alt+F4")
        self.vela.wait_for(lambda s: not s.has(window), what="the X11 window closed")

    def trace_events(self):
        return self.vela.query("trace-x11")

    def x11_control(self, *args):
        subprocess.run([tool("vela-x11"), *map(str, args)], env=self.env, check=True)

    def test_x11_trace_stats_query_and_reset(self):
        self.vela.command("trace-x11 on")
        initial = self.vela.query("trace-x11 stats")
        self.assertTrue(initial["enabled"])
        self.assertEqual(initial["windows"], [])
        self.open_x11(300, 200)
        stats = self.vela.query("trace-x11 stats")
        self.assertEqual(len(stats["windows"]), 1)
        view = stats["windows"][0]
        self.assertTrue(view["mapped"])
        self.assertTrue(view["has_buffer"])
        self.assertTrue(view["has_texture"])
        for key in ("submitted", "applied", "attaches", "null_attaches",
                    "buffer_changes", "damaged", "last_submit_ns", "last_apply_ns"):
            self.assertIsInstance(view[key], int)
            self.assertGreaterEqual(view[key], 0)
        self.vela.command("trace-x11 on")
        reset = self.vela.query("trace-x11 stats")
        self.assertEqual(reset["windows"][0]["submitted"], 0)
        self.assertEqual(reset["windows"][0]["applied"], 0)

    def test_x11_trace_lifecycle(self):
        self.assertEqual(self.trace_events(), [])
        self.vela.command("trace-x11 on")
        self.assertEqual(self.trace_events(), [])
        self.open_x11(300, 200)
        events = self.trace_events()
        self.assertTrue(any(" event=map " in e for e in events), events)
        self.assertTrue(any(" event=buffer-size " in e for e in events), events)
        self.assertTrue(all("xid=0x" in e and "time_ns=" in e for e in events), events)
        self.vela.command("trace-x11 off")
        retained = self.trace_events()
        self.open_x11(420, 260)
        self.assertEqual(self.trace_events(), retained)  # off really stops capture
        self.vela.command("trace-x11 clear")
        self.assertEqual(self.trace_events(), [])
        self.vela.command("trace-x11 on")
        snapshots = self.trace_events()
        self.assertEqual(sum(" event=snapshot " in e for e in snapshots), 2)
        self.vela.command("trace-x11 clear")
        self.assertEqual(self.trace_events(), [])

    def test_x11_trace_snapshot_includes_unmanaged_windows(self):
        self.open_x11("--menu", "100,120", 300, 200)
        self.vela.command("trace-x11 on")
        events = self.trace_events()
        self.assertTrue(any(" event=snapshot " in e and " managed=1 " in e for e in events), events)
        self.assertTrue(any(" event=snapshot " in e and " managed=0 " in e for e in events), events)
        self.assertTrue(all(" surface=" in e for e in events), events)

    def test_x11_trace_fullscreen_and_configure(self):
        self.vela.command("trace-x11 on")
        self.open_x11(300, 200)
        event = next(e for e in self.trace_events() if " event=map " in e and " managed=1 " in e)
        xid = next(field.split("=", 1)[1] for field in event.split() if field.startswith("xid="))
        self.x11_control("--configure", xid, 640, 420)
        self.vela.wait_for(
            lambda _: any(" event=request-configure " in e and "request=" in e and ":640x420" in e
                          for e in self.trace_events()),
            read_state=False, what="X11 ConfigureRequest recorded")
        self.vela.wait_for(
            lambda _: any(" event=configure-sent " in e and "640x420" in e for e in self.trace_events()),
            read_state=False, what="X11 configure response recorded")
        self.x11_control("--fullscreen", xid, "on")
        self.vela.wait_for(
            lambda _: any(" event=fullscreen-applied " in e and "velafullscreen=1" in e
                          for e in self.trace_events()),
            read_state=False, what="X11 fullscreen entered")
        self.x11_control("--fullscreen", xid, "off")
        self.vela.wait_for(
            lambda _: any(" event=fullscreen-applied " in e and "velafullscreen=0" in e
                          for e in self.trace_events()),
            read_state=False, what="X11 fullscreen left")
        events = self.trace_events()
        self.assertTrue(any(" event=request-fullscreen " in e and "requested=1" in e for e in events))
        self.assertTrue(any(" event=request-fullscreen " in e and "requested=0" in e for e in events))

    def test_x11_trace_is_bounded_and_ordered(self):
        self.vela.command("trace-x11 on")
        self.open_x11(300, 200)
        event = next(e for e in self.trace_events() if " event=map " in e and " managed=1 " in e)
        xid = next(field.split("=", 1)[1] for field in event.split() if field.startswith("xid="))
        self.vela.command("trace-x11 clear")
        self.x11_control("--flood", xid)
        self.vela.wait_for(lambda _: len(self.trace_events()) == 128,
                           read_state=False, what="128-entry bounded X11 trace")
        events = self.trace_events()
        sequences = [int(e.split("seq=", 1)[1].split()[0]) for e in events]
        self.assertEqual(sequences, list(range(sequences[0], sequences[0] + 128)))
        self.assertGreater(sequences[0], 0)  # oldest entries were evicted

    def test_menu_stays_where_the_app_puts_it(self):
        self.open_x11("--menu", "100,120", 300, 200)
        time.sleep(0.4)
        image = self.vela.pixels()
        self.assertEqual(image.at(160, 160), GREEN)
        self.assertEqual(image.at(105, 125), GREEN)
        self.assertNotEqual(image.at(95, 160), GREEN)


if __name__ == "__main__":
    unittest.main()
