# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Who captures the screen (docs/screen-sharing.md §3, §4): sessions held open
and screencopy streams are reported on `watch capture` and in `state`;
screenshots, the shell's own captures and VELA_CAPTURE_UNREPORTED never are."""

import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest

from harness import TIME_SCALE, Image, Session, tool
import test_shell # its classes not imported by name: unittest would run them here too


class Watcher:
    """A `watch capture` connection: every line it got, with its time."""

    def __init__(self, vela):
        self.lines = []
        self.lock = threading.Lock()
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.connect(vela.socket_path)
        self.connection.sendall(b"watch capture\n")
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        buffer = b""
        while True:
            try:
                chunk = self.connection.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                with self.lock:
                    self.lines.append((time.monotonic(), json.loads(line)))

    def states(self):
        with self.lock:
            return list(self.lines)

    def wait_for(self, condition, timeout=5.0, what="the capture state"):
        deadline = time.monotonic() + timeout * TIME_SCALE
        while time.monotonic() < deadline:
            for when, state in self.states():
                if condition(state):
                    return when, state
            time.sleep(0.02)
        raise AssertionError(f"timed out waiting for {what}; got {[s for _, s in self.states()]}")

    def close(self):
        self.connection.close()


def sharing(state):
    return bool(state["clients"])


class ScreenSharing(unittest.TestCase):
    def share(self, vela, *arguments, env=None, first_frame=True):
        """vela-share with a pipe on stdin: closing it ends the share."""
        process = subprocess.Popen([tool("vela-share"), *arguments], env=dict(vela.client_env, **(env or {})),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        vela.clients.append(process)
        if first_frame:
            self.assertEqual(process.stdout.readline().strip(), "sharing",
                             process.stderr.read() if process.poll() else "")
        return process

    @staticmethod
    def stop(process):
        process.stdin.close()
        process.wait(5)

    def test_session_on_an_output(self):
        with Session() as vela:
            watcher = Watcher(vela)
            started = time.monotonic()
            share = self.share(vela, "ext-output", "0")
            when, state = watcher.wait_for(sharing, what="the session to be reported")
            # A session counts once open for 1 s: a screenshot never does.
            self.assertGreaterEqual(when - started, 0.9)
            [client] = state["clients"]
            self.assertEqual(client["pid"], share.pid)
            self.assertEqual(client["binary"], "vela-share")
            self.assertEqual((client["app_id"], client["window"]), ("", "")) # it has no window
            self.assertFalse(client["portal"])
            [target] = client["targets"]
            self.assertEqual(target["type"], "output")
            self.assertEqual(target["name"], vela.state().output["name"])
            self.assertEqual(target["description"], "Headless output 1")
            self.assertEqual((target["make"], target["model"]), ("", "")) # headless outputs have none
            self.assertEqual(vela.state()["capture"], state) # the same in `state`
            self.stop(share)
            watcher.wait_for(lambda s: not sharing(s) and watcher.states()[-1][1] is s, what="the end of the share")
            self.assertEqual(vela.state()["capture"], {"clients": []})
            watcher.close()

    def test_session_on_a_window(self):
        with Session() as vela:
            window = vela.open_window(app_id="org.vela.shared")
            watcher = Watcher(vela)
            share = self.share(vela, "ext-window", window)
            _, state = watcher.wait_for(sharing, what="the window to be reported")
            [client] = state["clients"]
            title = vela.state().window(window)["title"]
            self.assertEqual(client["targets"], [{"type": "window", "id": window, "app_id": "org.vela.shared",
                                                  "title": title}])
            self.stop(share)
            watcher.wait_for(lambda s: not sharing(s) and watcher.states()[-1][1] is s, what="the end of the share")
            watcher.close()

    def test_its_window_names_it(self):
        with Session() as vela:
            watcher = Watcher(vela)
            share = self.share(vela, "ext-output", "0")
            watcher.wait_for(lambda s: sharing(s) and s["clients"][0]["app_id"] == "", what="the share, nameless")
            # Its window opens while it shares (nothing else happens to the
            # capture): the report names it at once...
            share.stdin.write("window\n")
            share.stdin.flush()
            _, state = watcher.wait_for(lambda s: sharing(s) and s["clients"][0]["app_id"] == "org.vela.Share",
                                        what="the report to name its window")
            self.assertTrue(state["clients"][0]["window"])
            # ...and forgets it when it closes.
            share.stdin.write("close\n")
            share.stdin.flush()
            watcher.wait_for(lambda s: sharing(s) and s["clients"][0]["window"] == "", what="the window to be forgotten")
            self.stop(share)
            watcher.close()

    def test_a_screenshot_is_never_reported(self):
        with Session() as vela:
            vela.command("test-output add 1280x720")
            vela.wait_for(lambda s: len(s["outputs"]) == 2, what="two outputs")
            watcher = Watcher(vela)
            path = os.path.join(vela.directory, "shot.png")
            for _ in range(3):
                vela.screenshot(path)
            # Like grim: one screencopy frame of every output, at once.
            subprocess.run([tool("vela-share"), "screencopy-once"], env=vela.client_env, check=True, timeout=10)
            subprocess.run([tool("vela-share"), "screencopy-once"], env=vela.client_env, check=True, timeout=10)
            time.sleep(2.5 * TIME_SCALE) # past every threshold
            self.assertTrue(watcher.states(), "the watcher got the state")
            self.assertFalse([s for _, s in watcher.states() if sharing(s)])
            watcher.close()

    def test_screencopy_stream(self):
        with Session() as vela:
            watcher = Watcher(vela)
            # Like wf-recorder; then it stops asking for frames, still connected.
            share = self.share(vela, "screencopy", "0", env={"VELA_SHARE_INTERVAL_MS": "50", "VELA_SHARE_FRAMES": "20"})
            when, state = watcher.wait_for(sharing, what="the screencopy stream to be reported")
            [client] = state["clients"]
            self.assertEqual(client["pid"], share.pid)
            self.assertEqual([t["name"] for t in client["targets"]], [vela.state().output["name"]])
            ended, _ = watcher.wait_for(lambda s: not sharing(s) and watcher.states()[-1][1] is s, timeout=8,
                                        what="the stream to run out")
            # 20 frames 50 ms apart, then 2 s without one.
            self.assertGreaterEqual(ended - when, 2.0)
            self.assertIsNone(share.poll(), "the client is still connected")
            self.stop(share)
            watcher.close()

    def test_screencopy_stream_of_a_lazy_client(self):
        with Session() as vela:
            watcher = Watcher(vela)
            # Its finished frames stay: they are not pending, so the stream
            # still runs out.
            share = self.share(vela, "screencopy", "0",
                               env={"VELA_SHARE_INTERVAL_MS": "50", "VELA_SHARE_FRAMES": "10", "VELA_SHARE_KEEP": "1"})
            watcher.wait_for(sharing, what="the stream to be reported")
            watcher.wait_for(lambda s: not sharing(s) and watcher.states()[-1][1] is s, timeout=8,
                             what="the stream to run out")
            self.assertIsNone(share.poll(), "the client is still connected")
            self.stop(share)
            watcher.close()

    def test_screencopy_frame_waiting_for_damage(self):
        with Session() as vela:
            vela.wait_for_log("virtual vblank") # the output is up and still
            watcher = Watcher(vela)
            # copy_with_damage: the first frame comes at once, the second
            # waits for the still screen to change.
            share = self.share(vela, "screencopy", "0", env={"VELA_SHARE_DAMAGE": "1"})
            _, state = watcher.wait_for(sharing, what="the stream to be reported")
            self.assertEqual(state["clients"][0]["pid"], share.pid)
            # Pending past the 2 s a stream gets after its last frame: still
            # reported (wf-recorder on a still screen).
            time.sleep(3.5 * TIME_SCALE)
            self.assertTrue(sharing(watcher.states()[-1][1]), "reported while its frame waits")
            self.assertEqual(sharing(vela.state()["capture"]), True)
            self.stop(share)
            watcher.wait_for(lambda s: not sharing(s) and watcher.states()[-1][1] is s, what="the end of the share")
            watcher.close()

    def test_a_client_that_dies(self):
        with Session() as vela:
            watcher = Watcher(vela)
            share = self.share(vela, "ext-output", "0")
            watcher.wait_for(sharing)
            share.send_signal(signal.SIGKILL)
            share.wait(5)
            watcher.wait_for(lambda s: not sharing(s) and watcher.states()[-1][1] is s, what="the client's report to go")
            watcher.close()

    def test_unreported(self):
        with Session(env={"VELA_CAPTURE_UNREPORTED": "1"}) as vela:
            vela.wait_for_log("VELA_CAPTURE_UNREPORTED=1")
            watcher = Watcher(vela)
            share = self.share(vela, "ext-output", "0")
            vela.command(f"test-shell {share.pid}") # as recording_core.py does: no crash
            time.sleep(1.5 * TIME_SCALE)
            self.assertFalse([s for _, s in watcher.states() if sharing(s)])
            self.assertEqual(vela.state()["capture"], {"clients": []})
            self.stop(share)
            watcher.close()


    def test_test_shell(self):
        """test-shell <pid>: the tests' real shell is the shell (§4.2)."""
        with Session() as vela:
            watcher = Watcher(vela)
            shell = self.share(vela, "ext-output", "0")
            other = self.share(vela, "ext-output", "0")
            watcher.wait_for(lambda s: len(s["clients"]) == 2, what="both clients reported")
            # Already capturing: its report goes at once.
            vela.command(f"test-shell {shell.pid}")
            _, state = watcher.wait_for(lambda s: [c["pid"] for c in s["clients"]] == [other.pid],
                                        what="only the other client")
            vela.command("test-shell nonsense") # ignored
            time.sleep(1.2 * TIME_SCALE)
            self.assertEqual([c["pid"] for c in vela.state()["capture"]["clients"]], [other.pid])
            self.stop(shell)
            self.stop(other)
            watcher.close()


class TheShell(unittest.TestCase):
    """The startup command is the shell: its captures stay its own (§4.2)."""

    def held(self, startup, copy_as=None):
        directory = None
        if copy_as:
            # A copy, not a link: /proc/<pid>/exe names the file it runs.
            directory = tempfile.mkdtemp(prefix="vela-share-")
            shutil.copy(tool("vela-share"), os.path.join(directory, copy_as))
            startup = startup.replace("@BINARY@", os.path.join(directory, copy_as))
        else:
            startup = startup.replace("@BINARY@", tool("vela-share"))
        try:
            with Session(startup=startup) as vela:
                watcher = Watcher(vela)
                time.sleep(2.0 * TIME_SCALE)
                return [s for _, s in watcher.states() if sharing(s)]
        finally:
            if directory:
                shutil.rmtree(directory, ignore_errors=True)

    def test_the_shell_itself(self):
        # /bin/sh execs a lone command: the shell is the compositor's child.
        self.assertEqual(self.held("exec @BINARY@ --forever ext-output 0"), [])

    def test_the_shell_under_sh(self):
        # sh stays, and its child is vela-shell.
        self.assertEqual(self.held("@BINARY@ --forever ext-output 0; true", copy_as="vela-shell"), [])

    def test_another_program_under_sh(self):
        # Its child that isn't vela-shell is an app like any other.
        reported = self.held("@BINARY@ --forever ext-output 0; true")
        self.assertTrue(reported)
        self.assertEqual(reported[-1]["clients"][0]["binary"], "vela-share")


def red_pixels(image, output_width, output_height):
    """The indicator's red pixels, near the taskbar's clock: (x, y) each."""
    return [(x, y) for y in range(output_height - 48, output_height) for x in range(output_width - 500, output_width)
            if (lambda p: p[0] > 200 and p[1] < 60 and p[2] < 70)(image.at(x, y))]


class TheIndicator(test_shell.RealShell):
    """The taskbar's icon, tooltip and menu (§6), with the real shell."""

    send_notification = test_shell.QtShell.send_notification

    def red(self):
        path = os.path.join(self.vela.directory, "taskbar.png")
        self.vela.screenshot(path)
        return red_pixels(Image.read(path), 1920, 1080)

    def test_shows_who_shares(self):
        self.assertEqual(self.red(), [])
        share = subprocess.Popen([tool("vela-share"), "ext-output", "0"], env=self.vela.client_env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.vela.clients.append(share)
        self.assertEqual(share.stdout.readline().strip(), "sharing")
        self.vela.wait_for(lambda _: self.red(), timeout=5, what="the red dot", read_state=False)
        # The icon: just left of and above its dot.
        dot = self.red()
        x, y = str(min(p[0] for p in dot) - 6), str(min(p[1] for p in dot) - 4)
        self.vela.input("move", x, y, "sleep", "300", "move", str(int(x) + 1), y, "sleep", "900")
        self.vela.wait_for(lambda s: self.mapped(s, "vela-taskbar-tooltip"), what="the tooltip")
        self.vela.input("click", "left")
        self.vela.wait_for(lambda s: self.mapped(s, "vela-context-menu"), what="the menu")
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-context-menu"), what="the menu to close")
        share.stdin.close()
        share.wait(5)
        # A second of grace, then gone.
        time.sleep(0.5)
        self.assertTrue(self.red(), "still shown half a second after")
        self.vela.wait_for(lambda _: not self.red(), timeout=5, what="the red dot to go", read_state=False)

    def test_notifications_wait_while_sharing(self):
        share = subprocess.Popen([tool("vela-share"), "ext-output", "0"], env=self.vela.client_env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.vela.clients.append(share)
        self.assertEqual(share.stdout.readline().strip(), "sharing")
        self.vela.wait_for(lambda _: self.red(), timeout=5, what="the red dot", read_state=False)
        # Held back: no popup, it waits in the notification center (§7).
        self.send_notification("While sharing")
        time.sleep(1.5 * TIME_SCALE)
        self.assertFalse(self.mapped(self.vela.state(), "vela-notifications"))
        # Settings > Notifications turns the rule off: popups again.
        conf = os.path.join(self.vela.env["XDG_CONFIG_HOME"], "Vela", "vela-shell.conf")
        with open(conf + ".new", "w") as file:
            file.write("[notifications]\nsilenceWhileSharing=false\n")
        os.replace(conf + ".new", conf)
        time.sleep(0.5 * TIME_SCALE)
        self.send_notification("Rule off")
        self.vela.wait_for(lambda s: self.mapped(s, "vela-notifications"), timeout=5, what="a popup")
        share.stdin.close()
        share.wait(5)

    def test_the_shells_own_previews_are_not_sharing(self):
        # One short session per preview update, never held for a second.
        self.vela.open_window()
        self.vela.input("key", "super+Tab") # Task View: live previews of the windows
        self.vela.wait_for(lambda s: self.mapped(s, "vela-task-view"), what="Task View")
        time.sleep(2.0 * TIME_SCALE)
        self.assertEqual(self.vela.state()["capture"], {"clients": []})
        self.vela.input("key", "Escape")
        self.vela.wait_for(lambda s: not self.mapped(s, "vela-task-view"), what="Task View to close")
        self.assertEqual(self.red(), [])


if __name__ == "__main__":
    unittest.main()
