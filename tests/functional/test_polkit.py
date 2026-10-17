# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""The polkit agent (docs/polkit-agent.md §10). No test uses the real polkitd
or PAM: a wrong password sent to PAM can lock the account (faillock).

Prompt: vela-polkit-prompt started by the test, which plays the agent on its
stdin/stdout. Veil and dialog, exclusive keyboard, shortcuts off, answers,
"try again", cancellations, choosing the identity.

Agent: vela-polkit-agent started by the compositor (VELA_POLKIT_AGENT), which
registers with a fake polkitd (fake_polkitd.py) on a private D-Bus bus and uses
a test session instead of PAM (VELA_POLKIT_TEST_PASSWORD).
"""

import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from harness import BUILD, Session, TimeoutError

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPT = os.path.join(BUILD, "polkit", "vela-polkit-prompt")
AGENT = os.path.join(BUILD, "polkit", "vela-polkit-agent")
PASSWORD = "velatest"

try:
    import dbus
except ImportError:
    dbus = None


def layer(state, namespace):
    for item in state["layers"]:
        if item["namespace"] == namespace and item["mapped"]:
            return item
    return None


def dialog_shown(state):
    return layer(state, "vela-polkit") is not None and state["focusedLayer"] == "vela-polkit"


def no_prompt(state):
    return all(not item["namespace"].startswith("vela-polkit") for item in state["layers"])


class Prompt:
    """vela-polkit-prompt, with the test in the agent's place."""

    def __init__(self, vela):
        self.vela = vela
        self.process = subprocess.Popen([PROMPT], env=vela.client_env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        vela.clients.append(self.process)
        self.pending = b""

    def send(self, *lines):
        self.process.stdin.write("".join(line + "\n" for line in lines).encode())
        self.process.stdin.flush()

    def show(self, identities=("1000 me Me",), extra=()):
        self.send("action org.vela.test.action", "message Authentication is required to test Vela",
                  "icon dialog-password", "app Test", "app-icon preferences-system",
                  "detail polkit.subject-pid=1", *("identity " + i for i in identities), *extra, "show",
                  "request 0 Password: ")
        return self.vela.wait_for(dialog_shown, what="the dialog with the keyboard")

    def read_line(self, timeout=3.0):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.process.stdout], [], [], left)[0]:
                raise TimeoutError(f"nothing from the prompt (so far: {self.pending!r})")
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                raise TimeoutError(f"the prompt closed its output (so far: {self.pending!r})")
            self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return line.decode()

    def assert_silent(self, seconds=0.7):
        if select.select([self.process.stdout], [], [], seconds)[0]:
            data = os.read(self.process.stdout.fileno(), 4096)
            if data:
                raise AssertionError(f"the prompt said {data!r}")

    def wait_exit(self, timeout=3.0):
        return self.process.wait(timeout)


def type_and_enter(vela, text):
    # A single vela-input invocation, with a pause first: its new virtual
    # keyboard sends its keymap, and the client must have it.
    vela.input("sleep", "250", "type", text, "sleep", "80", "key", "Return", "sleep", "80")


class PromptTests(unittest.TestCase):
    def setUp(self):
        self.vela = Session()
        self.vela.start()

    def tearDown(self):
        self.vela.stop()

    def test_answer_is_sent_and_done_closes(self):
        prompt = Prompt(self.vela)
        state = prompt.show()
        self.assertEqual(layer(state, "vela-polkit")["keyboard"], 1)  # exclusive
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(prompt.read_line(), "response " + PASSWORD)
        prompt.send("done")
        self.assertEqual(prompt.wait_exit(), 0)
        self.vela.wait_for(no_prompt, what="the prompt gone")

    def test_wrong_answer_then_retry(self):
        prompt = Prompt(self.vela)
        prompt.show()
        type_and_enter(self.vela, "wrong")
        self.assertEqual(prompt.read_line(), "response wrong")
        # The agent: wrong, and a new question.
        prompt.send("retry", "request 0 Password: ")
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(prompt.read_line(), "response " + PASSWORD)  # the field had been emptied
        prompt.send("done")
        self.assertEqual(prompt.wait_exit(), 0)

    def test_no_answer_while_pam_is_checking(self):
        prompt = Prompt(self.vela)
        prompt.show()
        type_and_enter(self.vela, "first")
        self.assertEqual(prompt.read_line(), "response first")
        type_and_enter(self.vela, "second")  # PAM hasn't answered yet
        prompt.assert_silent()

    def test_escape_says_no(self):
        prompt = Prompt(self.vela)
        prompt.show()
        self.vela.input("sleep", "250", "key", "Escape", "sleep", "80")
        self.assertEqual(prompt.read_line(), "cancel")
        self.assertEqual(prompt.wait_exit(), 0)
        self.vela.wait_for(no_prompt, what="the prompt gone")

    def test_agent_cancel_and_agent_gone_close_it(self):
        prompt = Prompt(self.vela)
        prompt.show()
        prompt.send("cancel")
        self.assertEqual(prompt.wait_exit(), 0)
        # The agent dying (stdin closed) counts as a cancellation.
        prompt = Prompt(self.vela)
        prompt.show()
        prompt.process.stdin.close()
        self.assertEqual(prompt.wait_exit(), 0)
        self.vela.wait_for(no_prompt, what="the prompt gone")

    def test_unavailable_authentication_does_not_answer(self):
        prompt = Prompt(self.vela)
        prompt.show()
        prompt.send("failed")
        type_and_enter(self.vela, PASSWORD)
        prompt.assert_silent()
        self.vela.input("sleep", "250", "key", "Escape", "sleep", "80")
        self.assertEqual(prompt.read_line(), "cancel")

    def test_choosing_another_identity(self):
        # Neither is the current user: there is a choice. From the field,
        # Shift+Tab goes to the last identity (root).
        me = os.getuid()
        prompt = Prompt(self.vela)
        prompt.show(identities=(f"{me + 1} admin Admin", "0 root root"))
        self.vela.input("sleep", "250", "key", "shift+Tab", "sleep", "80", "key", "space", "sleep", "80")
        self.assertEqual(prompt.read_line(), "identity 0")
        prompt.send("request 0 Password: ")
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(prompt.read_line(), "response " + PASSWORD)

    def test_clicks_outside_go_nowhere(self):
        window = self.vela.open_window(400, 300)
        self.vela.wait_for(lambda s: s["focused"] == window)
        x, y = self.vela.state().window(window)["x"], self.vela.state().window(window)["y"]
        prompt = Prompt(self.vela)
        prompt.show()
        self.vela.input("move", x + 20, y + 20, "click", "sleep", "100")
        prompt.assert_silent(0.3)
        state = self.vela.state()
        self.assertEqual(state["focusedLayer"], "vela-polkit")
        self.assertIsNone(state["focused"])

    def test_keyboard_stays_and_shortcuts_are_off(self):
        window = self.vela.open_window(400, 300)
        self.vela.wait_for(lambda s: s["focused"] == window)
        prompt = Prompt(self.vela)
        prompt.show()
        # A window opening now doesn't take the keyboard.
        other = self.vela.open_window(300, 200)
        state = self.vela.wait_for(lambda s: s.has(other))
        self.assertEqual(state["focusedLayer"], "vela-polkit")
        # Snap, Alt+F4, Alt+Tab: nothing.
        self.vela.keys("super+Left", "alt+F4", "alt+Tab")
        time.sleep(0.4)
        state = self.vela.state()
        self.assertTrue(state.has(window) and state.has(other))
        self.assertIsNone(state.window(window)["snap"])
        self.assertIsNone(state.window(other)["snap"])
        self.assertEqual(state["focusedLayer"], "vela-polkit")
        # The keyboard still belongs to the dialog.
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(prompt.read_line(), "response " + PASSWORD)
        prompt.send("done")
        prompt.wait_exit()
        # Dialog closed: the keyboard goes back to the windows, and the shortcuts too.
        state = self.vela.wait_for(lambda s: s["focused"] in (window, other) and s["focusedLayer"] is None,
                                   what="focus back to a window")
        self.vela.keys("super+Left")
        self.vela.wait_for(lambda s: s.window(state["focused"])["snap"] is not None, what="snap works again")


    def test_keyboard_move_ends_when_the_dialog_appears(self):
        window = self.vela.open_window(300, 200, decorated=True)
        x = self.vela.state().window(window)["x"]
        self.vela.command("window active move")
        self.vela.keys("Right", "Right")
        self.vela.wait_for(lambda s: s.window(window)["x"] == x + 20, what="moving by keyboard")
        # vela-input's virtual keyboard has just gone: the seat loses its only
        # keyboard and gets one back with the next vela-input. If that happens
        # while the dialog's window opens, Qt can leave it inactive (a Qt race
        # on wl_keyboard replacement, not ours): let the seat settle first.
        time.sleep(0.5)
        # The dialog appears mid-move: the move ends as with Esc, and the keys
        # go to the dialog.
        prompt = Prompt(self.vela)
        prompt.show()
        self.vela.wait_for(lambda s: s.window(window)["x"] == x, what="the window back in place")
        # The same race the other way round: typing replaces the keyboard
        # (the seat still has the one vela.keys made), so not while the dialog
        # opens.
        time.sleep(0.5)
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(prompt.read_line(), "response " + PASSWORD)
        # Under the dialog, the window menu can't start another one.
        prompt.send("request 0 Password: ")
        self.vela.command("window active move")
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(prompt.read_line(), "response " + PASSWORD)


class PromptOnTwoOutputs(unittest.TestCase):
    def test_dialog_under_the_pointer_and_veils_elsewhere(self):
        with Session(scale="HEADLESS-1=1,HEADLESS-2=1") as vela:
            vela.command("test-output add 1920x1080")
            state = vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two outputs")
            second = next(o for o in state["outputs"] if o["name"] == "HEADLESS-2")
            vela.input("move", second["x"] + 100, second["y"] + 100)
            prompt = Prompt(vela)
            state = prompt.show()
            self.assertEqual(layer(state, "vela-polkit")["output"], "HEADLESS-2")
            # The prompt knows where the dialog is when the surface enters
            # the output: the veils settle right after.
            def veils(s):
                return sorted(item["output"] for item in s["layers"]
                              if item["namespace"] == "vela-polkit-veil" and item["mapped"])
            vela.wait_for(lambda s: veils(s) == ["HEADLESS-1"], what="a veil only on the other output")
            prompt.send("done")
            prompt.wait_exit()
            vela.wait_for(no_prompt, what="dialog and veils gone")


@unittest.skipUnless(dbus and shutil.which("dbus-daemon"), "needs dbus-python and dbus-daemon")
class AgentTests(unittest.TestCase):
    """The real agent, started by the compositor, with a fake polkitd."""

    def start(self, reject=False, language=None, backend=None):
        self.bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address=1"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.address = self.bus.stdout.readline().decode().strip()
        self.fake = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_polkitd.py"), self.address]
                                     + (["--reject"] if reject else [])
                                     + (["--backend", backend] if backend else []), stdout=subprocess.PIPE)
        self.assertEqual(self.fake.stdout.readline().strip(), b"ready")
        self.config = tempfile.mkdtemp(prefix="vela-polkit-test-")
        if language:
            os.makedirs(os.path.join(self.config, "vela"))
            with open(os.path.join(self.config, "vela", "vela.conf"), "w") as conf:
                conf.write(f"language={language}\n")
        self.vela = Session(env={
            "VELA_POLKIT_AGENT": AGENT,
            "VELA_POLKIT_TEST_PASSWORD": PASSWORD,
            "DBUS_SYSTEM_BUS_ADDRESS": self.address,
            "XDG_SESSION_ID": "vela-test",
            "XDG_CONFIG_HOME": self.config,
        })
        self.vela.start()
        connection = dbus.bus.BusConnection(self.address)
        self.control = dbus.Interface(connection.get_object("org.vela.Test", "/org/vela/Test"), "org.vela.Test")

    def tearDown(self):
        if hasattr(self, "vela"):
            self.vela.stop()
        for process in (getattr(self, "fake", None), getattr(self, "bus", None)):
            if process:
                process.terminate()
                process.wait(3)
        if hasattr(self, "config"):
            shutil.rmtree(self.config, ignore_errors=True)

    def registrations(self, count=1, timeout=8.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = [tuple(str(x) for x in r) for r in self.control.Registrations()]
            if len(found) >= count:
                return found
            time.sleep(0.05)
        raise TimeoutError(f"{count} registrations expected\n{self.vela.log_text()[-2500:]}")

    def begin(self, cookie):
        self.control.Begin(cookie, "org.vela.test.action", "Authentication is required to test Vela",
                           "dialog-password", {"polkit.subject-pid": str(os.getpid())},
                           dbus.Array([dbus.UInt32(os.getuid())], signature="u"))

    def result(self, cookie, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = str(self.control.Result(cookie))
            if value != "pending":
                return value
            time.sleep(0.05)
        return "pending"

    def child_named(self, prefix, parent):
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open(f"/proc/{pid}/comm") as comm:
                    name = comm.read().strip()
                with open(f"/proc/{pid}/stat") as stat:
                    ppid = int(stat.read().rsplit(")", 1)[1].split()[1])
            except OSError:
                continue
            if name.startswith(prefix) and ppid == parent:
                return int(pid)
        return None

    def agent_pid(self):
        return self.child_named("vela-polkit-age", self.vela.process.pid)

    def test_registers_for_the_session_in_vela_language(self):
        self.start(language="it")
        kind, session, locale, path = self.registrations()[0]
        self.assertEqual(kind, "unix-session")
        self.assertTrue(session)
        self.assertEqual(locale, "it_IT.UTF-8")
        self.assertEqual(path, "/org/vela/PolkitAgent")

    def test_authorize_after_a_wrong_password(self):
        self.start()
        self.registrations()
        self.begin("c1")
        self.vela.wait_for(dialog_shown, timeout=8, what="the dialog")
        type_and_enter(self.vela, "wrong")
        time.sleep(0.6)  # the test session answers after 300 ms
        self.assertEqual(self.result("c1", timeout=0.1), "pending")
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(self.result("c1"), "ok")
        self.vela.wait_for(no_prompt, what="the prompt gone")

    def test_user_says_no(self):
        self.start()
        self.registrations()
        self.begin("c1")
        self.vela.wait_for(dialog_shown, timeout=8, what="the dialog")
        self.vela.input("sleep", "250", "key", "Escape", "sleep", "80")
        self.assertIn("Cancelled", self.result("c1"))
        self.vela.wait_for(no_prompt, what="the prompt gone")

    def test_polkit_withdraws_the_request(self):
        self.start()
        self.registrations()
        self.begin("c1")
        self.vela.wait_for(dialog_shown, timeout=8, what="the dialog")
        self.control.Cancel("c1")
        # polkitd doesn't look at the answer to a request it withdrew: it just
        # mustn't be an authorization.
        self.assertTrue(self.result("c1").startswith("error:"))
        self.vela.wait_for(no_prompt, what="the prompt gone")

    def test_requests_are_queued(self):
        self.start()
        self.registrations()
        self.begin("c1")
        self.begin("c2")
        self.vela.wait_for(dialog_shown, timeout=8, what="the first dialog")
        prompts = [item for item in self.vela.state()["layers"] if item["namespace"] == "vela-polkit"]
        self.assertEqual(len(prompts), 1)
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(self.result("c1"), "ok")
        self.assertEqual(self.result("c2", timeout=0.1), "pending")
        # The second dialog: the first must have finished fading.
        self.vela.wait_for(lambda s: dialog_shown(s) and len(
            [i for i in s["layers"] if i["namespace"] == "vela-polkit"]) == 1, timeout=8, what="the second dialog")
        time.sleep(0.4)
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(self.result("c2"), "ok")

    def test_crashed_dialog_counts_as_no(self):
        self.start()
        self.registrations()
        agent = self.agent_pid()
        self.begin("c1")
        self.vela.wait_for(dialog_shown, timeout=8, what="the dialog")
        prompt = self.child_named("vela-polkit-pro", agent)
        self.assertIsNotNone(prompt)
        os.kill(prompt, signal.SIGKILL)
        self.assertIn("Cancelled", self.result("c1"))
        # The agent is still there, and the next request works.
        self.assertEqual(self.agent_pid(), agent)
        self.begin("c2")
        self.vela.wait_for(dialog_shown, timeout=8, what="the next dialog")
        type_and_enter(self.vela, PASSWORD)
        self.assertEqual(self.result("c2"), "ok")

    def test_test_mode_refuses_a_real_polkitd(self):
        # A private bus, but an authority that isn't the fake one: the test
        # password must not take the place of the session's agent.
        self.start(backend="js")
        self.vela.wait_for_log("works only with the tests' fake polkitd")
        time.sleep(0.5)
        self.assertEqual(self.control.Registrations(), [])

    def test_restarted_after_a_crash(self):
        self.start()
        self.registrations()
        pid = self.agent_pid()
        self.assertIsNotNone(pid)
        os.kill(pid, signal.SIGSEGV)
        self.registrations(count=2)
        self.assertIn("restarting it", self.vela.log_text())

    def test_not_restarted_when_another_agent_is_registered(self):
        self.start(reject=True)
        self.registrations()
        self.vela.wait_for_log("exited (code 2)")
        time.sleep(1.0)
        self.assertEqual(len(self.registrations()), 1)


if __name__ == "__main__":
    unittest.main()
