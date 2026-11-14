# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Trust boundaries of a running compositor: the command socket and the
screen lock (docs/security-model.md)."""

import os
import socket
import stat
import subprocess
import time
import unittest

from harness import Session, tool


class CommandSocket(unittest.TestCase):
    def test_socket_is_private_to_the_user(self):
        with Session() as vela:
            info = os.stat(vela.socket_path)
            self.assertTrue(stat.S_ISSOCK(info.st_mode))
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
            self.assertEqual(info.st_uid, os.geteuid())

    def test_test_commands_need_an_explicit_opt_in(self):
        with Session(env={"VELA_TEST_COMMANDS": "0"}) as vela:
            outputs = len(vela.state()["outputs"])
            vela.command("test-output add 1280x720")
            vela.command("test-power off")
            vela.wait_for_log("Refused a test-only command")
            time.sleep(0.2)
            state = vela.state()
            self.assertEqual(len(state["outputs"]), outputs)
            self.assertTrue(all(output["powered"] for output in state["outputs"]))

    def test_test_commands_work_when_enabled(self):
        with Session() as vela:
            outputs = len(vela.state()["outputs"])
            vela.command("test-output add 1280x720")
            vela.wait_for(lambda s: len(s["outputs"]) == outputs + 1, what="the test output")

    def test_connections_are_bounded(self):
        with Session() as vela:
            idle = []
            try:
                for _ in range(32):
                    connection = socket.socket(socket.AF_UNIX)
                    connection.connect(vela.socket_path)
                    idle.append(connection)
                # One more is accepted by the kernel, then closed unanswered.
                with socket.socket(socket.AF_UNIX) as extra:
                    extra.connect(vela.socket_path)  # blocking: waits for backlog room
                    extra.settimeout(5)
                    try:
                        extra.sendall(b"state\n")
                        answer = extra.recv(65536)
                    except (BrokenPipeError, ConnectionResetError):
                        answer = b""  # closed with the line still unread
                    self.assertEqual(answer, b"")
                vela.wait_for_log("connections already open")
            finally:
                for connection in idle:
                    connection.close()
            # Once they are gone, new connections are served again.
            time.sleep(0.3)
            vela.wait_for(lambda s: "outputs" in s, what="a connection after the others closed")


class Lock(unittest.TestCase):
    def test_only_the_launched_locker_can_take_a_vela_lock(self):
        # Locked by Vela, with no locker registered (it exits at once): a
        # client Vela did not launch must not get the lock, or it could
        # unlock the desktop.
        with Session(env={"VELA_LOCK": "exit 1"}) as vela:
            window = vela.open_window()
            vela.wait_for(lambda s: s["focused"] == window)
            vela.command("lock")
            vela.wait_for(lambda s: s["locked"], what="locked without a locker")
            intruder = subprocess.run([tool("vela-testlock"), "100"], env=vela.client_env,
                                      capture_output=True, text=True, timeout=10)
            self.assertIn("the compositor refused the lock", intruder.stderr)
            self.assertNotIn("unlocked", intruder.stderr)
            vela.wait_for_log("refusing a lock from a client Vela did not launch")
            state = vela.state()
            self.assertTrue(state["locked"])
            self.assertIsNone(state["focused"])
            image = vela.pixels()
            self.assertEqual(image.at(image.width // 2, image.height // 2), (0, 0, 0))

    def test_unlocked_session_still_accepts_a_client_lock(self):
        # Nothing to bypass while unlocked: a client (swaylock, say) may lock,
        # and only unlocks what it locked itself.
        with Session() as vela:
            locker = subprocess.run([tool("vela-testlock"), "300"], env=vela.client_env,
                                    capture_output=True, text=True, timeout=10)
            self.assertIn("vela-testlock: locked", locker.stderr)
            vela.wait_for(lambda s: not s["locked"], what="unlocked by the same client")


if __name__ == "__main__":
    unittest.main()
