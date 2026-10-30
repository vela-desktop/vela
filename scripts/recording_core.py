#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Shared deterministic capture engine for Vela demos and before/after comparisons.

The real Vela shell and compositor run on a headless backend with a private
D-Bus session and private PipeWire. Recordings use presentation timestamps
from vela-screen-record, not wall-clock samples from a screen-recorder GUI.
"""

import bisect
import json
import mmap
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "functional"))
import harness  # noqa: E402

RECORDER = Path(os.environ.get("VELA_BUILD", ROOT / "build")) / "tools" / "vela-screen-record"
FPS = 60
LEAD = 0.3
TAIL = 0.4

# ------------------------------------------------------------------ scenes --

class Scene:
    """A scene: the output, how to show it, and what happens. prepare()
    readies the desktop and returns the area to record (output pixels, or
    None for all of it) plus anything act() needs; act() does the scene."""

    name = ""
    about = ""
    size = "1920x1080@60"
    scale = 1.0
    speed = 1.0 # 0.25: four times slower
    zoom = 1 # nearest neighbour: the pixels stay pixels
    downscale = 1 # vela-screen-record averages N×N blocks
    stacked = False # one above the other instead of side by side
    poster = 0.0 # seconds after the start: the poster's moment
    lead = LEAD
    tail = TAIL
    strip = None # (seconds after the start, refreshes): a strip of that many
    env = {} # more for the compositor

    def prepare(self, desktop):
        raise NotImplementedError

    def act(self, desktop, context):
        raise NotImplementedError



# ----------------------------------------------------------------- desktop --

ENGLISH = {"LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8", "LANGUAGE": "en_US"} # the journal is in English

# A PipeWire of its own, with made-up devices: the videos are published, and
# the real ones have names (wpctl status shows them).
PIPEWIRE_DEVICES = """context.objects = [
""" + "".join(f"""    {{ factory = adapter args = {{ factory.name = support.null-audio-sink node.name = "{name}"
        node.description = "{description}" media.class = "{kind}" audio.position = [ {channels} ] }} }}
""" for name, description, kind, channels in (("speakers", "Speakers", "Audio/Sink", "FL FR"),
                                             ("headphones", "Headphones", "Audio/Sink", "FL FR"),
                                             ("microphone", "Microphone", "Audio/Source/Virtual", "MONO"))) + "]\n"
WIREPLUMBER_NO_HARDWARE = """wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
  }
}
"""


class Desktop:
    """The real shell on a headless Vela from one build, on a private D-Bus
    session and PipeWire, with the default wallpaper and English text
    (RealShell in test_shell.py)."""

    def __init__(self, build_dir, scene):
        self.build_dir = build_dir
        self.scene = scene

    @staticmethod
    def bus_environment(address, scene):
        """Use the private bus for *both* D-Bus types: real network/BT/device
        names must not appear in a video, and tiles must not change host state."""
        return {"VELA_SYNC_FILE": os.environ.get("VELA_QT_SYNC_FILE", "0"),
                **ENGLISH, **scene.env,
                "DBUS_SESSION_BUS_ADDRESS": address,
                "DBUS_SYSTEM_BUS_ADDRESS": address}

    @staticmethod
    def isolated_environment(address, scene, home, runtime):
        """Override inherited user locations, even if a scene defines them."""
        return {**Desktop.bus_environment(address, scene),
                "HOME": str(home), "USER": "alex", "LOGNAME": "alex",
                "XDG_RUNTIME_DIR": str(runtime),
                "XDG_STATE_HOME": str(home / ".local" / "state")}

    def __enter__(self):
        harness.BUILD = str(self.build_dir) # tool() and Session read it
        self.vela = None
        self.bus = None
        self._private = tempfile.TemporaryDirectory(prefix="vela-record-")
        private = Path(self._private.name)
        self.private_home = private / "home"
        self.private_runtime = private / "runtime"
        self.private_home.mkdir(mode=0o700)
        self.private_runtime.mkdir(mode=0o700)
        try:
            self.bus = subprocess.Popen(
                ["dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            address = self.bus.stdout.readline().strip()
            if not address:
                raise RuntimeError("the private D-Bus session didn't start")
            self.vela = harness.Session(size=self.scene.size, scale=self.scene.scale,
                                        env=self.isolated_environment(address, self.scene,
                                                                      self.private_home, self.private_runtime))
            self.vela.start()
            os.makedirs(os.path.join(self.vela.env["XDG_CONFIG_HOME"], "Vela"), exist_ok=True)
            # An empty desktop, not the user's icons.
            desktop = os.path.join(self.vela.directory, "Desktop")
            os.makedirs(desktop)
            with open(os.path.join(self.vela.env["XDG_CONFIG_HOME"], "user-dirs.dirs"), "w") as file:
                file.write(f'XDG_DESKTOP_DIR="{desktop}"\n')
            pipewire = self.start_pipewire()
            self.qt_env = dict(self.vela.client_env, QT_QPA_PLATFORM="wayland", QT_WAYLAND_RECONNECT="0",
                               QT_FORCE_STDERR_LOGGING="1", QSG_RENDER_LOOP="threaded", QSG_RHI_BACKEND="opengl",
                               PIPEWIRE_RUNTIME_DIR=pipewire)
            shell = subprocess.Popen([str(self.build_dir / "shell" / "vela-shell")],
                                     env=dict(self.qt_env, QT_WAYLAND_SHELL_INTEGRATION="layer-shell"),
                                     stdout=self.vela.log, stderr=self.vela.log)
            self.vela.clients.append(shell)
            self.vela.wait_for(lambda s: self.layer(s, "vela-taskbar") and self.layer(s, "vela-wallpaper"),
                               timeout=20, what="the real Qt desktop")
            time.sleep(1.5) # the wallpaper drawn, the colors sent to the compositor
        except BaseException as error:
            self.__exit__(type(error), error, None)
            raise
        return self

    def __exit__(self, kind, value, traceback):
        try:
            try:
                if self.vela:
                    self.vela.stop(keep_log=kind is not None)
            finally:
                if self.bus is not None:
                    self.bus.terminate()
                    try:
                        self.bus.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        self.bus.kill()
                        self.bus.wait()
                    if self.bus.stdout:
                        self.bus.stdout.close()
        finally:
            self._private.cleanup()
        return False

    @staticmethod
    def layer(state, namespace):
        return next((layer for layer in state["layers"] if layer["namespace"] == namespace and layer["mapped"]), None)

    def start_pipewire(self):
        """PipeWire and WirePlumber for this session only, with Speakers,
        Headphones, a microphone and one app playing silence; returns the
        directory of its socket."""
        env = dict(self.vela.env)
        config = Path(env["XDG_CONFIG_HOME"])
        (config / "pipewire" / "pipewire.conf.d").mkdir(parents=True)
        (config / "pipewire" / "pipewire.conf.d" / "devices.conf").write_text(PIPEWIRE_DEVICES)
        (config / "wireplumber" / "wireplumber.conf.d").mkdir(parents=True)
        (config / "wireplumber" / "wireplumber.conf.d" / "devices.conf").write_text(WIREPLUMBER_NO_HARDWARE)
        # Short: a socket path has room for 107 bytes.
        runtime = os.path.join(self.vela.directory, "pw")
        os.mkdir(runtime, 0o700)
        env.update(PIPEWIRE_RUNTIME_DIR=runtime, XDG_STATE_HOME=os.path.join(self.vela.directory, "state"))
        self.vela.clients.append(subprocess.Popen(["pipewire"], env=env, stdout=self.vela.log, stderr=self.vela.log))
        deadline = time.monotonic() + 5
        while not os.path.exists(os.path.join(runtime, "pipewire-0")):
            if time.monotonic() > deadline:
                raise RuntimeError("the private PipeWire didn't start")
            time.sleep(0.05)
        wireplumber = subprocess.Popen(["wireplumber"], env=env, stdout=self.vela.log, stderr=self.vela.log)
        self.vela.clients.append(wireplumber)
        with open("/dev/zero", "rb") as silence: # an app in the volume mixer
            music = subprocess.Popen(
                ["pw-cat", "--playback", "--raw", "--format", "s16", "--rate", "48000", "--channels", "2",
                 "--properties", '{ application.name = "Music" }', "-"],
                env=env, stdin=silence, stdout=self.vela.log, stderr=self.vela.log)
            self.vela.clients.append(music)
        # The socket is not enough: the shell must find the outputs and the app.
        deadline = time.monotonic() + 10
        while True:
            for process, what in ((wireplumber, "WirePlumber"), (music, "the Music stream (pw-cat)")):
                if process.poll() is not None:
                    raise RuntimeError(f"{what} exited on the private PipeWire")
            dump = subprocess.run(["pw-dump"], env=env, capture_output=True, text=True, timeout=5)
            if dump.returncode == 0 and self.audio_ready(json.loads(dump.stdout or "[]")):
                return runtime
            if time.monotonic() > deadline:
                raise RuntimeError("the private PipeWire has no default output or no Music stream")
            time.sleep(0.1)

    @staticmethod
    def audio_ready(dump):
        """In pw-dump's objects: the made-up outputs, WirePlumber's choice of
        default, and the Music app playing."""
        nodes = [(o.get("info") or {}).get("props") or {} for o in dump if o.get("type") == "PipeWire:Interface:Node"]
        defaults = {entry.get("key") for o in dump if o.get("type") == "PipeWire:Interface:Metadata"
                    and (o.get("props") or {}).get("metadata.name") == "default"
                    for entry in o.get("metadata") or ()}
        return ({"speakers", "headphones"} <= {props.get("node.name") for props in nodes}
                and "default.audio.sink" in defaults
                and any(props.get("application.name") == "Music" and props.get("media.class") == "Stream/Output/Audio"
                        for props in nodes))

    def open_app(self, path, *args, user=None):
        """One of Vela's Qt apps, if built; returns its window. user: the name
        it sees in /etc/passwd (Settings shows it), through two user
        namespaces: the outer one may mount, the inner one is us again."""
        program = self.build_dir / path
        if not program.exists():
            return None
        command = [str(program), *args]
        if user:
            passwd = Path(self.vela.directory) / "passwd"
            passwd.write_text(f"{user.lower()}:x:{os.getuid()}:{os.getgid()}:{user}:{self.private_home}:/bin/sh\n")
            command = ["unshare", "--user", "--map-root-user", "--mount", "sh", "-c",
                       'mount --bind "$0" /etc/passwd && uid=$1 gid=$2 && shift 2 && '
                       'exec unshare --user --map-user="$uid" --map-group="$gid" -- "$@"',
                       str(passwd), str(os.getuid()), str(os.getgid()), *command]
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen(command, env=self.qt_env,
                                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        state = self.vela.wait_for(lambda s: {w["id"] for w in s.windows} - before, timeout=15,
                                   what=f"{program.name}'s window")
        return next(iter({w["id"] for w in state.windows} - before))


# ------------------------------------------------------------------- takes --

class Take:
    """One recording: frames.raw and frames.txt from vela-screen-record, and
    when the scene started and ended (CLOCK_MONOTONIC, like the frames)."""

    def __init__(self, directory, start, end, presented):
        self.directory = directory
        self.start, self.end = start, end
        lines = (directory / "frames.txt").read_text().split()
        self.width, self.height, self.format = int(lines[0]), int(lines[1]), lines[2]
        self.times = [float(t) for t in lines[3:]]
        self.presented = presented
        self.frame_size = self.width * self.height * 4

    def align(self):
        """Starts the take at its first new image since the scene started:
        the two builds don't react to the same command in the same
        millisecond."""
        with open(self.directory / "frames.raw", "rb") as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            first = bisect.bisect_right(self.times, self.start)
            for index in range(max(first, 1), len(self.times)):
                previous = data[(index - 1) * self.frame_size:index * self.frame_size]
                if data[index * self.frame_size:(index + 1) * self.frame_size] != previous:
                    self.start = self.times[index]
                    return

    def at(self, moments):
        """The frame on screen at each of these moments: the last one
        presented by then (the first one, before it)."""
        with open(self.directory / "frames.raw", "rb") as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for t in moments:
                index = max(0, bisect.bisect_right(self.times, t) - 1)
                yield data[index * self.frame_size:(index + 1) * self.frame_size]

    def frames(self, duration, speed, lead=LEAD):
        """The frame on screen at each tick of a FPS video, from lead before
        the start, slowed by speed, for duration seconds of video."""
        return self.at(self.start - lead + tick / FPS * speed for tick in range(round(duration * FPS)))


def record(desktop, scene, directory, *, align=True):
    """Runs a scene with the correctly identified output; always reap the recorder."""
    area, context = scene.prepare(desktop)
    vela = desktop.vela
    selected = vela.state().output
    name = selected.get("name")
    if not name:
        raise RuntimeError("the selected output has no stable wl_output name")
    env = dict(vela.client_env, VELA_SCREEN_RECORD_OUTPUT=name,
               VELA_SCREEN_RECORD_DOWNSCALE=str(scene.downscale))
    recorder = subprocess.Popen([str(RECORDER), str(directory), *map(str, area or ())], env=env,
                                stdout=subprocess.PIPE, text=True)
    try:
        started = threading.Event()
        threading.Thread(target=lambda: recorder.stdout.readline() and started.set(), daemon=True).start()
        deadline = time.monotonic() + 5
        while not started.wait(0.1):
            if recorder.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("vela-screen-record didn't start recording")
            vela.input("move", 2, 2 + int(time.monotonic() * 10) % 2)
        frames_before = vela.state().output["frames"]
        time.sleep(max(scene.lead, 0.3))
        start = time.clock_gettime(time.CLOCK_MONOTONIC)
        scene.act(desktop, context)
        end = time.clock_gettime(time.CLOCK_MONOTONIC)
        time.sleep(scene.tail)
        presented = vela.state().output["frames"] - frames_before
        recorder.send_signal(signal.SIGINT)
        if recorder.wait(timeout=30) != 0:
            raise RuntimeError("vela-screen-record failed")
        take = Take(directory, start, end, presented)
        if align:
            take.align()  # Before/after needs a shared visual change; promo keeps the opening.
        # Record only display metadata, never the user or their real devices.
        take.output_info = {key: selected[key] for key in ("name", "w", "h", "refresh")
                            if key in selected}
        return take
    finally:
        if recorder.poll() is None:
            recorder.kill()
            recorder.wait()
        if recorder.stdout:
            recorder.stdout.close()


# ------------------------------------------------------------------ ffmpeg --

def has_drawtext():
    filters = subprocess.run(["ffmpeg", "-hide_banner", "-filters"], capture_output=True, text=True).stdout
    return any(line.split()[1:2] == ["drawtext"] for line in filters.splitlines())


def lossless(take, frames, path):
    """These frames of the take as a constant-rate video (FFV1)."""
    encoder = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                                "-f", "rawvideo", "-pix_fmt", take.format, "-s", f"{take.width}x{take.height}",
                                "-r", str(FPS), "-i", "-", "-c:v", "ffv1", str(path)], stdin=subprocess.PIPE)
    try:
        for frame in frames:
            encoder.stdin.write(frame)
        encoder.stdin.close()
        if encoder.wait() != 0:
            raise RuntimeError("ffmpeg failed")
    except BaseException:
        if encoder.poll() is None:
            encoder.kill()
        encoder.wait()
        if not encoder.stdin.closed:
            encoder.stdin.close()
        raise


