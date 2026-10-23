#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Before/after videos for the journal: the same scene on two builds of Vela,
side by side.

    scripts/record-before-after.py --before f72a182^ --after main
    scripts/record-before-after.py --before f72a182^ titlebar --out ~/videos
    scripts/record-before-after.py --list

Each ref gets a worktree under ~/.cache/vela-before-after, built there with
CMake and Ninja (once; later runs only rebuild what changed). Each scene runs
on the headless backend with the real shell, as tests/functional/test_shell.py
does, driven by vela-input and the command socket. vela-screen-record, from
this checkout's build (it speaks only ext-image-copy-capture, so it records
older compositors too), writes every frame with its presentation time. The
two takes are then lined up on the moment the scene starts, played at the
scene's speed and put side by side, or one above the other, by FFmpeg: WebM
(VP9) and MP4 (H.264) for src/components/mdx/Video.astro on the website, and a
PNG poster.

Needs what the functional tests need (a GPU with Vulkan 1.4, dbus-daemon),
the shell built, and ffmpeg. The scenes only move the pointer and press keys:
no click on a Quick Settings tile, since Wi-Fi, Bluetooth and the others act
on the real system bus."""

import argparse
import bisect
import math
import mmap
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "functional"))
import harness # noqa: E402

CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "vela-before-after"
RECORDER = Path(os.environ.get("VELA_BUILD", ROOT / "build")) / "tools" / "vela-screen-record"
FPS = 60 # of the videos
LEAD = 0.3 # seconds shown before the scene starts
TAIL = 0.4 # and after it ends

BAR = 32 # the title bar, logical units (compositor/src/decoration.h)
BUTTON = 46
PANEL = 360 # Quick Settings' panel width (shell/qml/QuickSettings.qml)
TILE_W, TILE_H, TILE_GAP_X, TILE_GAP_Y, PANEL_PADDING = 104, 82, 8, 4, 16


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

    def prepare(self, desktop):
        raise NotImplementedError

    def act(self, desktop, context):
        raise NotImplementedError


class Maximize(Scene):
    name = "maximize"
    about = "maximize and restore at 180 Hz, next to a 60 Hz monitor, four times slower"
    size = "1920x1080@180"
    speed = 0.25
    downscale = 2
    poster = 0.12

    def prepare(self, desktop):
        # Its own rate on each: the old shared clock made the faster one
        # repeat frames. No rate given: 60 Hz on both builds.
        desktop.vela.command("test-output add 1280x720")
        desktop.vela.wait_for(lambda s: len(s["outputs"]) == 2, what="the second output")
        window = desktop.open_app("settings/vela-settings") or desktop.vela.open_window(960, 600, decorated=True)
        desktop.vela.wait_still(window)
        if desktop.vela.state().window(window)["maximized"]:
            desktop.vela.command(f"window {window} restore")
            desktop.vela.wait_for(lambda s: not s.window(window)["maximized"], what="the window restored")
            desktop.vela.wait_still(window)
        time.sleep(1.0) # its first frames, the bar's colors
        return None, window

    def act(self, desktop, window):
        desktop.vela.command(f"window {window} maximize")
        time.sleep(0.8)
        desktop.vela.command(f"window {window} restore")
        time.sleep(0.8)


class TitleBar(Scene):
    name = "titlebar"
    about = "the title bar at 125%, zoomed in, while the window moves one unit at a time, then hover"
    scale = 1.25
    speed = 0.5
    zoom = 4
    stacked = True
    poster = 2.4
    STEPS = 12 # units the window moves

    def prepare(self, desktop):
        vela = desktop.vela
        window = vela.open_window(520, 300, decorated=True)
        w = vela.wait_for(lambda s: s["focused"] == window).window(window)
        time.sleep(0.5)
        left = math.floor((w["x"] + w["w"] - 3 * BUTTON - 100) * self.scale)
        right = math.ceil((w["x"] + w["w"] + self.STEPS) * self.scale)
        top = math.floor(w["y"] * self.scale) - 4
        bottom = math.ceil((w["y"] + BAR) * self.scale) + 4
        return (left, top, right - left, bottom - top), (window, w)

    def act(self, desktop, context):
        window, w = context
        vela = desktop.vela
        # The drag of test_decorations.py, then one unit at a time: where the
        # bar lands on half pixels, the old images went through the filter.
        x, y = w["x"] + 200, w["y"] + BAR // 2
        steps = []
        for i in range(1, self.STEPS + 1):
            steps += ["move", x + i, y + 10, "sleep", "150"]
        vela.input("move", x, y, "sleep", "50", "down", "sleep", "50", "move", x, y + 10, "sleep", "150",
                   *steps, "up", "sleep", "400")
        w = vela.state().window(window)
        middle = w["y"] + BAR // 2
        vela.input("move", w["x"] + w["w"] - BUTTON - BUTTON // 2, middle, "sleep", "600",
                   "move", w["x"] + w["w"] - BUTTON // 2, middle, "sleep", "600",
                   "move", w["x"] + 100, w["y"] + 150, "sleep", "600")


class QuickSettings(Scene):
    name = "quick-settings"
    about = "Quick Settings: open, hover over the tiles, the sound page and back, close"
    speed = 0.5
    poster = 0.15
    NAMESPACE = "vela-quick-settings"

    def prepare(self, desktop):
        vela = desktop.vela
        vela.input("key", "super+a")
        vela.wait_for(lambda s: desktop.layer(s, self.NAMESPACE), what="Quick Settings open")
        time.sleep(1.0)
        layer = desktop.layer(vela.state(), self.NAMESPACE)
        vela.input("key", "Escape")
        vela.wait_for(lambda s: not desktop.layer(s, self.NAMESPACE), what="Quick Settings closed")
        time.sleep(0.5)
        # The panel is 360 wide with 12 units of window right of it; left of
        # it the shadow's room: 12 before PanelMotion, 46 since. The same
        # above.
        room = layer["w"] - PANEL - 12
        left, top = layer["x"] + room, layer["y"] + room
        tiles = [(left + PANEL_PADDING + column * (TILE_W + TILE_GAP_X) + TILE_W // 2,
                  top + PANEL_PADDING + row * (TILE_H + TILE_GAP_Y) + TILE_H // 2)
                 for row in range(2) for column in range(3)]
        output = vela.state().output
        width, height = 560, 820
        area = (output["w"] - width, output["h"] - height, width, height)
        return area, (tiles, (left + 6, top + 8))

    def act(self, desktop, context):
        tiles, margin = context
        # One vela-input for the whole scene: a new virtual keyboard would
        # take the focus from the panel.
        actions = ["key", "super+a", "sleep", "900"]
        for x, y in tiles:
            actions += ["move", x, y, "sleep", "350"]
        actions += ["move", *margin, "sleep", "300",
                    "key", "super+ctrl+v", "sleep", "1200", # the sound page
                    "key", "Escape", "sleep", "1000", # back
                    "key", "Escape", "sleep", "900"] # closed
        desktop.vela.input(*actions)


SCENES = {scene.name: scene for scene in (Maximize(), TitleBar(), QuickSettings())}


# ------------------------------------------------------------------ builds --

def git(*args, cwd=ROOT):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def build(ref, jobs):
    """A worktree of ref under CACHE, built; returns its build directory."""
    commit = git("rev-parse", "--verify", f"{ref}^{{commit}}")
    tree = CACHE / commit[:12]
    if not tree.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        print(f"{ref}: worktree in {tree}", flush=True)
        git("worktree", "add", "--detach", str(tree), commit)
    out = tree / "build"
    if not (out / "build.ninja").exists():
        subprocess.run(["cmake", "-S", str(tree), "-B", str(out), "-G", "Ninja"], check=True)
    print(f"{ref}: building {commit[:12]}", flush=True)
    subprocess.run(["cmake", "--build", str(out), *(["-j", str(jobs)] if jobs else [])], check=True)
    return out


# ----------------------------------------------------------------- desktop --

class Desktop:
    """The real shell on a headless Vela from one build, on a private D-Bus
    session, with the default wallpaper (RealShell in test_shell.py)."""

    def __init__(self, build_dir, scene):
        self.build_dir = build_dir
        self.scene = scene

    def __enter__(self):
        harness.BUILD = str(self.build_dir) # tool() and Session read it
        self.vela = None
        self.bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        try:
            address = self.bus.stdout.readline().strip()
            if not address:
                raise RuntimeError("the private D-Bus session didn't start")
            self.vela = harness.Session(size=self.scene.size, scale=self.scene.scale,
                                        env={"VELA_SYNC_FILE": os.environ.get("VELA_QT_SYNC_FILE", "0"),
                                             "DBUS_SESSION_BUS_ADDRESS": address})
            self.vela.start()
            os.makedirs(os.path.join(self.vela.env["XDG_CONFIG_HOME"], "Vela"), exist_ok=True)
            self.qt_env = dict(self.vela.client_env, QT_QPA_PLATFORM="wayland", QT_WAYLAND_RECONNECT="0",
                               QT_FORCE_STDERR_LOGGING="1", QSG_RENDER_LOOP="threaded", QSG_RHI_BACKEND="opengl")
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
        if self.vela:
            self.vela.stop(keep_log=kind is not None)
        self.bus.terminate()
        try:
            self.bus.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.bus.kill()
            self.bus.wait()
        self.bus.stdout.close()
        return False

    @staticmethod
    def layer(state, namespace):
        return next((layer for layer in state["layers"] if layer["namespace"] == namespace and layer["mapped"]), None)

    def open_app(self, path):
        """One of Vela's Qt apps, if built; returns its window."""
        program = self.build_dir / path
        if not program.exists():
            return None
        before = {w["id"] for w in self.vela.state().windows}
        self.vela.clients.append(subprocess.Popen([str(program)], env=self.qt_env,
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

    def frames(self, duration, speed):
        """The frame on screen at each tick of a FPS video, from LEAD before
        the start, slowed by speed, for duration seconds of video."""
        with open(self.directory / "frames.raw", "rb") as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            for tick in range(round(duration * FPS)):
                t = self.start - LEAD + tick / FPS * speed
                index = max(0, bisect.bisect_right(self.times, t) - 1)
                yield data[index * self.frame_size:(index + 1) * self.frame_size]


def record(desktop, scene, directory):
    """Runs the scene while vela-screen-record records it."""
    area, context = scene.prepare(desktop)
    vela = desktop.vela
    env = dict(vela.client_env, VELA_SCREEN_RECORD_OUTPUT="0", VELA_SCREEN_RECORD_DOWNSCALE=str(scene.downscale))
    recorder = subprocess.Popen([str(RECORDER), str(directory), *map(str, area or ())], env=env,
                                stdout=subprocess.PIPE, text=True)
    started = threading.Event()
    threading.Thread(target=lambda: recorder.stdout.readline() and started.set(), daemon=True).start()
    # The first frame comes with the next change on screen: move the pointer
    # where it doesn't matter until it does.
    deadline = time.monotonic() + 5
    while not started.wait(0.1):
        if recorder.poll() is not None or time.monotonic() > deadline:
            recorder.kill()
            raise RuntimeError("vela-screen-record didn't start recording")
        vela.input("move", 2, 2 + int(time.monotonic() * 10) % 2)
    frames_before = vela.state().output["frames"]
    time.sleep(LEAD)
    start = time.clock_gettime(time.CLOCK_MONOTONIC)
    scene.act(desktop, context)
    end = time.clock_gettime(time.CLOCK_MONOTONIC)
    time.sleep(TAIL)
    presented = vela.state().output["frames"] - frames_before
    recorder.send_signal(signal.SIGINT)
    if recorder.wait(timeout=30) != 0:
        raise RuntimeError("vela-screen-record failed")
    return Take(directory, start, end, presented)


# ------------------------------------------------------------------ ffmpeg --

def has_drawtext():
    filters = subprocess.run(["ffmpeg", "-hide_banner", "-filters"], capture_output=True, text=True).stdout
    return any(line.split()[1:2] == ["drawtext"] for line in filters.splitlines())


def lossless(take, duration, speed, path):
    """The take as a constant-rate video at the scene's speed (FFV1)."""
    encoder = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                                "-f", "rawvideo", "-pix_fmt", take.format, "-s", f"{take.width}x{take.height}",
                                "-r", str(FPS), "-i", "-", "-c:v", "ffv1", str(path)], stdin=subprocess.PIPE)
    for frame in take.frames(duration, speed):
        encoder.stdin.write(frame)
    encoder.stdin.close()
    if encoder.wait() != 0:
        raise RuntimeError("ffmpeg failed")


def assemble(scene, before, after, out, work, labels):
    duration = (LEAD + max(before.end - before.start, after.end - after.start) + TAIL) / scene.speed
    lossless(before, duration, scene.speed, work / "before.mkv")
    lossless(after, duration, scene.speed, work / "after.mkv")
    speed = "" if scene.speed == 1 else f" · {scene.speed:g}×"
    chains = []
    for i, label in enumerate(("Before", "After")):
        chain = f"[{i}:v]scale=iw*{scene.zoom}:ih*{scene.zoom}:flags=neighbor"
        if labels:
            chain += (f",drawtext=text='{label}{speed}':x=16:y=12:fontsize=26:fontcolor=white"
                      ":box=1:boxcolor=black@0.55:boxborderw=8")
        chains.append(chain + f"[v{i}]")
    stack = "vstack" if scene.stacked else "hstack"
    graph = ";".join(chains) + f";[v0][v1]{stack},pad=ceil(iw/2)*2:ceil(ih/2)*2,format=yuv420p[v]"
    inputs = ["-i", str(work / "before.mkv"), "-i", str(work / "after.mkv")]
    common = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *inputs, "-filter_complex", graph, "-map", "[v]",
              "-an"]
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([*common, "-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0", "-row-mt", "1",
                    str(out / f"{scene.name}.webm")], check=True)
    subprocess.run([*common, "-c:v", "libx264", "-crf", "20", "-preset", "slow", "-movflags", "+faststart",
                    str(out / f"{scene.name}.mp4")], check=True)
    poster = min((LEAD + scene.poster) / scene.speed, duration - 1 / FPS)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", f"{poster:.3f}",
                    "-i", str(out / f"{scene.name}.mp4"), "-frames:v", "1", str(out / f"{scene.name}.png")],
                   check=True)


# -------------------------------------------------------------------- main --

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="*", help="the scenes to record (default: all)")
    parser.add_argument("--before", help="the ref before the change, such as f72a182^")
    parser.add_argument("--after", default="HEAD", help="the ref after it (default: HEAD)")
    parser.add_argument("--out", type=Path, default=Path.cwd() / "before-after", help="where the videos go")
    parser.add_argument("--jobs", type=int, help="build jobs (default: Ninja's)")
    parser.add_argument("--keep", action="store_true", help="keep the raw takes (in the output directory)")
    parser.add_argument("--list", action="store_true", help="list the scenes")
    args = parser.parse_args()

    if args.list:
        for scene in SCENES.values():
            print(f"{scene.name:16} {scene.about}")
        return 0
    unknown = [name for name in args.scenes if name not in SCENES]
    if unknown or not args.before:
        parser.error(f"unknown scene: {', '.join(unknown)}" if unknown else "--before is required")
    if not harness.gpu_available():
        sys.exit("No GPU (/dev/dri/renderD*): Vela's renderer needs one.")
    for program in ("ffmpeg", "dbus-daemon", "cmake", "ninja"):
        if not shutil.which(program):
            sys.exit(f"{program} is missing.")
    if not RECORDER.exists():
        sys.exit(f"{RECORDER} is missing: cmake --build build --target vela-screen-record")
    labels = has_drawtext()
    if not labels:
        print("This ffmpeg has no drawtext: the videos won't say which side is which.", file=sys.stderr)

    builds = {"before": build(args.before, args.jobs), "after": build(args.after, args.jobs)}
    for name in args.scenes or SCENES:
        scene = SCENES[name]
        work = Path(tempfile.mkdtemp(prefix=f"vela-{name}-"))
        takes = {}
        for side, build_dir in builds.items():
            directory = work / side
            directory.mkdir()
            print(f"{name}: recording {side}", flush=True)
            with Desktop(build_dir, scene) as desktop:
                takes[side] = take = record(desktop, scene, directory)
            # Fewer frames than presented means the recording missed some:
            # what's on screen at those moments isn't in the video.
            print(f"{name}: {side}, {len(take.times)} frames captured, {take.presented} presented", flush=True)
        assemble(scene, takes["before"], takes["after"], args.out, work, labels)
        if args.keep:
            shutil.copytree(work, args.out / f"{name}-takes", dirs_exist_ok=True)
        shutil.rmtree(work)
        print(f"{name}: {args.out / name}.webm, .mp4, .png", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
