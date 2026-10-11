# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""A private PipeWire and WirePlumber for the audio tests (docs/audio.md §11).

Both run in a temporary directory that holds their runtime, state and
configuration: the user's audio, WirePlumber state and D-Bus services are
never touched. The devices are null sinks with fixed names (pipewire.conf):

    vela-test-speakers    stereo output, the default one
    vela-test-headphones  stereo output
    vela-test-surround    5.1 output
    vela-test-microphone  mono input, the default one

As a module:

    with AudioFixture() as audio:
        stream = audio.stream("--amplitude", "0.5", "--prop", "application.name=Player")
        node = stream.wait_ready()
        ...

As a program, it runs a command inside the fixture and returns its status
(77, "skipped", when PipeWire or WirePlumber is missing):

    fixture.py -- wpctl status
"""

import ctypes
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKIP = 77

SPEAKERS = "vela-test-speakers"
HEADPHONES = "vela-test-headphones"
SURROUND = "vela-test-surround"
MICROPHONE = "vela-test-microphone"

# WirePlumber writes its state (defaults, volumes, profiles) about 1 s after
# a change, and doesn't write it when it exits. Restarting it sooner loses
# the last changes (docs/audio.md §8).
WIREPLUMBER_SAVE_DELAY = 1.5

# Variables that could lead a test process to the user's audio, D-Bus or
# configuration.
_DROPPED = (
    "PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_CONFIG_DIR", "PIPEWIRE_CONFIG_NAME",
    "PIPEWIRE_CONFIG_PREFIX", "PIPEWIRE_PROPS", "PIPEWIRE_NODE", "PIPEWIRE_LATENCY",
    "WIREPLUMBER_CONFIG_DIR", "WIREPLUMBER_DATA_DIR", "WIREPLUMBER_MODULE_DIR",
    "PULSE_SERVER", "PULSE_RUNTIME_PATH", "PULSE_SINK", "PULSE_SOURCE",
    "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS",
    "XDG_DATA_HOME", "XDG_CACHE_HOME",
)

_PR_SET_PDEATHSIG = 1


def missing_tools():
    """The programs the fixture needs that aren't installed."""
    return [tool for tool in ("pipewire", "wireplumber", "pw-dump", "pw-metadata", "wpctl")
            if not shutil.which(tool)]


def fake_stream_path():
    """vela-audio-fake-stream, from VELA_AUDIO_FAKE_STREAM or the build."""
    path = os.environ.get("VELA_AUDIO_FAKE_STREAM")
    if path:
        return path
    build = Path(os.environ.get("VELA_BUILD", HERE.parent.parent / "build"))
    return str(build / "tests" / "audio" / "vela-audio-fake-stream")


def _die_with_parent():
    # A killed test run must not leave daemons behind.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl(_PR_SET_PDEATHSIG, signal.SIGTERM)
    os.setsid()


def _short_temp_root():
    # The socket's path has to fit in sun_path (108 bytes).
    root = tempfile.gettempdir()
    return root if len(root) <= 40 else "/tmp"


class FakeStream:
    """A vela-audio-fake-stream process: lines out, commands in."""

    def __init__(self, process):
        self.process = process
        self.node = None
        self.state = None
        self._lines = []
        self._peaks = []
        self._condition = threading.Condition()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        for raw in self.process.stdout:
            line = raw.strip()
            words = line.split()
            with self._condition:
                if words[:1] == ["ready"]:
                    self.node = int(words[1])
                elif words[:1] == ["state"]:
                    self.state = words[1]
                elif words[:1] == ["peaks"]:
                    self._peaks.append([float(w) for w in words[1:]])
                self._lines.append(line)
                self._condition.notify_all()
        with self._condition:
            self._condition.notify_all()

    def _wait(self, predicate, timeout, what):
        with self._condition:
            if not self._condition.wait_for(lambda: predicate() or self.process.poll() is not None, timeout):
                raise TimeoutError(f"fake stream: no {what} after {timeout} s; output: {self._lines}")
            if not predicate():
                raise RuntimeError(f"fake stream exited ({self.process.returncode}) before {what}; "
                                   f"output: {self._lines}")

    def wait_ready(self, timeout=5.0):
        """The stream's node id."""
        self._wait(lambda: self.node is not None, timeout, "node id")
        return self.node

    def wait_state(self, state, timeout=5.0):
        self._wait(lambda: self.state == state, timeout, f"state {state}")

    def peaks(self, count=3, timeout=5.0):
        """Recording: the next `count` reports (lists of per-channel peaks)."""
        with self._condition:
            self._peaks.clear()
        self._wait(lambda: len(self._peaks) >= count, timeout, f"{count} peak reports")
        with self._condition:
            return self._peaks[-count:]

    def send(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def close(self, timeout=5.0):
        """Ends it (end of input); its exit status. Not 0 is a failure: a
        crash, or a sanitizer's report."""
        if self.process.poll() is None:
            try:
                self.process.stdin.close()  # end of input: it exits
            except BrokenPipeError:
                pass
            try:
                self.process.wait(timeout)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self._reader.join(timeout)
        if self.process.stdout:
            self.process.stdout.close()
        return self.process.returncode


class AudioFixture:
    def __init__(self, fake_stream=None):
        self.fake_stream = fake_stream or fake_stream_path()
        self.root = None
        self.env = None
        self._pipewire = None
        self._wireplumber = None
        self._streams = []

    # ------------------------------------------------------------ lifetime --

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()

    def start(self):
        self.root = Path(tempfile.mkdtemp(prefix="vela-audio-", dir=_short_temp_root()))
        for name in ("run", "state", "config", "home"):
            (self.root / name).mkdir(mode=0o700)
        dropins = self.root / "config" / "wireplumber" / "wireplumber.conf.d"
        dropins.mkdir(parents=True)
        shutil.copy(HERE / "wireplumber-test.conf", dropins / "10-vela-test.conf")

        env = {k: v for k, v in os.environ.items() if k not in _DROPPED}
        env.update({
            "HOME": str(self.root / "home"),
            "XDG_RUNTIME_DIR": str(self.root / "run"),
            "XDG_STATE_HOME": str(self.root / "state"),
            "XDG_CONFIG_HOME": str(self.root / "config"),
            # Nobody is listening there: rtkit, BlueZ and logind stay out of
            # reach. A test that needs a bus starts its own.
            "DBUS_SYSTEM_BUS_ADDRESS": f"unix:path={self.root / 'no-system-bus'}",
        })
        self.env = env
        try:
            self._start_pipewire()
            self._start_wireplumber()
        except BaseException:
            self.stop()
            raise

    def stop(self):
        failed = [(s.process.args, code) for s in self._streams if (code := s.close()) != 0]
        self._streams.clear()
        self._stop("_wireplumber")
        self._stop("_pipewire")
        if self.root:
            shutil.rmtree(self.root, ignore_errors=True)
            self.root = None
        if failed:
            raise RuntimeError(f"fake streams failed: {failed}")

    def restart_wireplumber(self):
        """As Vela does after writing 90-vela.conf (docs/audio.md §8)."""
        self._stop("_wireplumber")
        self._start_wireplumber()

    def restart_pipewire(self):
        """PipeWire going away and coming back; WirePlumber follows."""
        self._stop("_wireplumber")
        self._stop("_pipewire")
        self._start_pipewire()
        self._start_wireplumber()

    def _spawn(self, name, *args):
        log = open(self.root / f"{name}.log", "ab")
        try:
            return subprocess.Popen(args, env=self.env, stdin=subprocess.DEVNULL, stdout=log,
                                    stderr=subprocess.STDOUT, preexec_fn=_die_with_parent)
        finally:
            log.close()

    def _stop(self, attribute):
        process = getattr(self, attribute)
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        setattr(self, attribute, None)

    def _start_pipewire(self):
        socket = self.root / "run" / "pipewire-0"
        self._pipewire = self._spawn("pipewire", "pipewire", "-c", str(HERE / "pipewire.conf"))
        self._wait(socket.exists, "the PipeWire socket", self._pipewire)

    def _start_wireplumber(self):
        self._wireplumber = self._spawn("wireplumber", "wireplumber", "-p", "vela-test")
        # Ready when it has chosen the defaults.
        self._wait(lambda: self.default("sink") and self.default("source"),
                   "WirePlumber's defaults", self._wireplumber)

    def _wait(self, predicate, what, process, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if process.poll() is not None:
                break
            time.sleep(0.05)
        raise RuntimeError(f"no {what} after {timeout} s; logs: {self.logs()}")

    def logs(self):
        out = {}
        for name in ("pipewire", "wireplumber"):
            path = self.root / f"{name}.log" if self.root else None
            if path and path.exists():
                out[name] = path.read_text(errors="replace")[-4000:]
        return out

    # -------------------------------------------------------------- clients --

    def run(self, *args, check=True, timeout=10):
        """A command inside the fixture (wpctl, pw-cli...); its stdout."""
        result = subprocess.run(args, env=self.env, capture_output=True, text=True, timeout=timeout)
        if check and result.returncode != 0:
            raise RuntimeError(f"{args} failed ({result.returncode}): {result.stderr}")
        return result.stdout

    def stream(self, *args):
        """A fake app (fake-stream.c), closed with the fixture."""
        process = subprocess.Popen([self.fake_stream, *args], env=self.env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True, bufsize=1,
                                   preexec_fn=_die_with_parent)
        stream = FakeStream(process)
        self._streams.append(stream)
        return stream

    # -------------------------------------------------------------- queries --

    def dump(self):
        """pw-dump's objects."""
        return json.loads(self.run("pw-dump"))

    def node(self, name):
        """The node called `name` (node.name), or None."""
        for obj in self.dump():
            props = obj.get("info", {}).get("props", {})
            if obj.get("type") == "PipeWire:Interface:Node" and props.get("node.name") == name:
                return obj
        return None

    def links(self):
        """(output node id, input node id) for every link."""
        return {(o["info"]["output-node-id"], o["info"]["input-node-id"])
                for o in self.dump() if o.get("type") == "PipeWire:Interface:Link"}

    def default(self, kind):
        """The default sink or source's node.name, from the metadata; None
        while there is none (WirePlumber not ready or restarting)."""
        result = subprocess.run(["pw-metadata", "-n", "default", "0", f"default.audio.{kind}"],
                                env=self.env, capture_output=True, text=True, timeout=5)
        for line in result.stdout.splitlines():
            if f"key:'default.audio.{kind}'" in line and "value:'" in line:
                value = line.split("value:'", 1)[1].split("'", 1)[0]
                try:
                    return json.loads(value).get("name")
                except ValueError:
                    return None
        return None

    def wait_for(self, predicate, what, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        raise TimeoutError(f"no {what} after {timeout} s")


def main(argv):
    if argv[:1] == ["--"]:
        argv = argv[1:]
    if not argv:
        print(__doc__)
        return 2
    missing = missing_tools()
    if missing:
        print(f"skipped: {', '.join(missing)} not installed", file=sys.stderr)
        return SKIP
    with AudioFixture() as audio:
        return subprocess.run(argv, env=audio.env).returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
