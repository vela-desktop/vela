# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""sudo PTY transport must retain real logind/VT authorization checks."""

import os
import stat
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from vela_bench import BenchError
from vela_bench.pilot import require_no_graphical_sessions, tty_preconditions, VT_GETSTATE, VT_OPENQRY


class TTYTransport(unittest.TestCase):
    def setUp(self):
        self.session = {"Id": "7", "TTY": "tty3", "Type": "tty", "Active": "yes", "Remote": "no", "Seat": "seat0"}
        self.host = Mock()
        self.host.run.side_effect = lambda *args, **kwargs: SimpleNamespace(
            stdout="\n".join(f"{key}={value}" for key, value in self.session.items()))
        self.patches = []
        def start(name, **kwargs):
            context = patch(name, **kwargs)
            self.addCleanup(context.stop)
            return context.start()
        self.input = start("vela_bench.pilot.os.ttyname", return_value="/dev/pts/9")
        self.open = start("vela_bench.pilot.os.open", return_value=88)
        self.close = start("vela_bench.pilot.os.close")
        self.fstat = start("vela_bench.pilot.os.fstat", return_value=SimpleNamespace(st_mode=stat.S_IFCHR | 0o620, st_rdev=os.makedev(4, 3)))
        self.list = start("vela_bench.pilot.logind_sessions", side_effect=lambda *args: [dict(self.session)])
        def ioctl(fd, request, result, mutate):
            result[0] = 3 if request == VT_GETSTATE else 8
        self.ioctl = start("vela_bench.pilot.fcntl.ioctl", side_effect=ioctl)

    def test_sudo_pty_resolves_the_callers_local_tty_without_replacing_stdin(self):
        result = tty_preconditions(self.host)
        self.assertEqual(result, {"origin_vt": 3, "target_vt": 8, "origin_session": "7"})
        self.host.run.assert_called_once_with(["loginctl", "show-session", "self", "--all", "--no-pager"])
        self.assertEqual(self.open.call_args.args[0], "/dev/tty3")
        self.assertTrue(self.open.call_args.args[1] & os.O_NOCTTY)
        self.assertTrue(self.open.call_args.args[1] & os.O_NOFOLLOW)
        self.assertEqual([call.args[0] for call in self.ioctl.call_args_list], [88, 88])
        self.close.assert_called_once_with(88)

    def test_native_tty_uses_original_descriptor(self):
        self.input.return_value = "/dev/tty3"
        self.assertEqual(tty_preconditions(self.host, fd=5)["origin_vt"], 3)
        self.open.assert_not_called()
        self.close.assert_not_called()
        self.assertEqual([call.args[0] for call in self.ioctl.call_args_list], [5, 5])

    def test_graphical_terminal_cannot_claim_a_tty_with_environment_hints(self):
        self.session.update(Type="wayland", TTY="tty1")
        with patch.dict(os.environ, {"SUDO_TTY": "/dev/tty3", "XDG_SESSION_TYPE": "tty", "XDG_VTNR": "3"}):
            with self.assertRaisesRegex(BenchError, "own active local logind"):
                tty_preconditions(self.host)
        self.open.assert_not_called()
        self.ioctl.assert_not_called()

    def test_remote_or_inactive_or_other_seat_session_is_rejected(self):
        for key, value in (("Remote", "yes"), ("Active", "no"), ("Seat", "seat1"), ("TTY", "pts/7")):
            old = self.session[key]
            with self.subTest(key=key):
                self.session[key] = value
                with self.assertRaises(BenchError):
                    tty_preconditions(self.host)
                self.open.assert_not_called()
            self.session[key] = old

    def test_another_active_local_session_cannot_replace_the_callers_session(self):
        self.list.side_effect = None
        self.list.return_value = [dict(self.session, Id="different")]
        with self.assertRaisesRegex(BenchError, "Cannot verify"):
            tty_preconditions(self.host)
        self.open.assert_not_called()

    def test_direct_descriptor_must_match_logind_tty(self):
        self.input.return_value = "/dev/tty4"
        with self.assertRaisesRegex(BenchError, "does not match"):
            tty_preconditions(self.host)
        self.ioctl.assert_not_called()

    def test_kernel_active_vt_is_checked_even_when_logind_reports_active(self):
        def ioctl(fd, request, result, mutate):
            result[0] = 4
        self.ioctl.side_effect = ioctl
        with self.assertRaisesRegex(BenchError, "active TTY"):
            tty_preconditions(self.host)
        self.close.assert_called_once_with(88)

    def test_device_identity_is_checked_and_opened_descriptor_closed_on_failure(self):
        self.fstat.return_value = SimpleNamespace(st_mode=stat.S_IFCHR, st_rdev=os.makedev(136, 3))
        with self.assertRaisesRegex(BenchError, "virtual-console device"):
            tty_preconditions(self.host)
        self.close.assert_called_once_with(88)
        self.ioctl.assert_not_called()

    def test_open_graphical_sessions_are_still_refused(self):
        self.list.side_effect = None
        self.list.return_value = [dict(self.session), {"Id": "9", "Type": "wayland", "Class": "user"}]
        with self.assertRaisesRegex(BenchError, "graphical user session"):
            tty_preconditions(self.host)
        self.open.assert_not_called()


    def test_logged_out_wayland_remnants_and_login_greeter_allow_tty_preflight(self):
        self.list.side_effect = None
        self.list.return_value = [dict(self.session),
            {"Id": "2", "Type": "wayland", "Class": "user", "State": "closing", "Active": "no",
             "Service": "plasmalogin", "TTY": "tty2"},
            {"Id": "6", "Type": "wayland", "Class": "user", "State": "closing", "Active": "no",
             "Service": "plasmalogin", "TTY": "tty4"},
            {"Id": "c1", "Type": "wayland", "Class": "greeter", "State": "online", "Active": "no"}]
        self.assertEqual(tty_preconditions(self.host)["origin_vt"], 3)


class GraphicalSessionOccupancy(unittest.TestCase):
    def test_online_or_uncertain_desktops_block_even_when_not_in_foreground(self):
        for kind in ("wayland", "x11"):
            for state, active in (("online", "no"), ("active", "yes"), ("closing", "yes"),
                                  (None, "no"), ("closing", None)):
                with self.subTest(kind=kind, state=state, active=active):
                    session = {"Id": "6", "Name": "operator", "Type": kind, "Class": "user",
                               "Service": "plasmalogin", "TTY": "tty4"}
                    if state is not None:
                        session["State"] = state
                    if active is not None:
                        session["Active"] = active
                    for after in (False, True):
                        with self.assertRaisesRegex(BenchError, 'Blocking sessions:.*"Id": "6".*"Service": "plasmalogin"'):
                            require_no_graphical_sessions([session], after_authorization=after)

    def test_greeters_and_logged_out_sessions_are_allowed_at_every_takeover_check(self):
        for kind in ("wayland", "x11"):
            for after in (False, True):
                with self.subTest(kind=kind, after=after):
                    require_no_graphical_sessions([
                        {"Type": kind, "Class": "greeter", "State": "active", "Active": "yes"},
                        {"Type": kind, "Class": "user", "State": "closing", "Active": "no"}],
                        after_authorization=after)

    def test_logged_out_session_does_not_hide_another_online_desktop(self):
        sessions = [{"Id": "2", "Type": "wayland", "Class": "user", "State": "closing", "Active": "no"},
                    {"Id": "8", "Type": "wayland", "Class": "user", "State": "online", "Active": "no"}]
        with self.assertRaises(BenchError) as caught:
            require_no_graphical_sessions(sessions)
        self.assertIn('"Id": "8"', str(caught.exception))
        self.assertNotIn('"Id": "2"', str(caught.exception))


if __name__ == "__main__":
    unittest.main()
