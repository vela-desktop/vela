#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Before/after videos for the journal: the same scene on two builds of Vela,
side by side.

    scripts/record-before-after.py --before f72a182^ --after main --pick 3fb2a40
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
PNG poster. Scenes about frame pacing also get a strip: the same refreshes of
the monitor, one after the other, before above and after below.

Needs what the functional tests need (a GPU with Vulkan 1.4, dbus-daemon),
the shell built, ffmpeg, PipeWire with WirePlumber, and unprivileged user
namespaces (unshare). The videos are published, so nothing personal shows:
the text is in English, the sound devices are made up on a PipeWire of the
session's own, and Settings, shown without its home page, sees a user named
Alex. The scenes only move the pointer and press keys: no click on a Quick
Settings tile, since Wi-Fi, Bluetooth and the others act on the real system
bus."""

import argparse
import bisect
import hashlib
import json
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


# The capture engine is shared with record-promo.py.
sys.path.insert(0, str(ROOT / "scripts"))
from recording_core import Scene, Desktop, Take, record, lossless, has_drawtext  # noqa: E402

class Maximize(Scene):
    name = "maximize"
    about = ("maximize at 180 Hz while a window maximizes on a 60 Hz monitor too, without late latching, ten times "
             "slower, and a strip of 8 refreshes: before, 2 in 3 repeat the previous one")
    # Before #111, copying a 1080p output into shared memory took Vela
    # longer than a frame at 180 Hz: record with --pick and that fix.
    size = "1920x1080@180"
    # With late latching the two monitors' leads differ by about 1 ms and the
    # old clock hardly showed (docs/visual-coherence.md D3); without it, as
    # nested in KWin, they differ by a frame or more.
    env = {"VELA_LATCH": "0"}
    # The old shared clock moved the animation on at 60 Hz: each position
    # stays for three refreshes. Ten times slower, that's 1/20 s, which shows.
    speed = 0.1
    lead = 0.1
    tail = 0.2
    poster = 0.12
    strip = (0.06, 8) # in the middle of the morph

    def prepare(self, desktop):
        vela = desktop.vela
        # Its own rate on each: the old shared clock made the faster one
        # repeat frames. No rate given: 60 Hz on both builds.
        vela.command("test-output add 1280x720")
        vela.wait_for(lambda s: len(s["outputs"]) == 2, what="the second output")
        # A window on the 60 Hz monitor too, which maximizes with the other:
        # with nothing to draw, the old build without late latching planned
        # its frames at the event loop's speed and starved the 180 Hz one,
        # a different defect (fixed in #40 as well) that hides this one. New
        # windows open where the pointer is.
        second = next(o for o in vela.state()["outputs"] if o["name"] != vela.state().output["name"])
        vela.input("move", second["x"] + second["w"] // 2, second["y"] + second["h"] // 2)
        other = vela.open_window(640, 400, decorated=True)
        placed = vela.state().window(other)
        if not second["x"] <= placed["x"] < second["x"] + second["w"]:
            raise RuntimeError("the second window didn't open on the 60 Hz monitor")
        vela.input("move", 200, 200)
        # Not the home page: it shows the PC's name.
        window = (desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
                  or vela.open_window(800, 500, decorated=True))
        vela.wait_still(window)
        if vela.state().window(window)["maximized"]:
            vela.command(f"window {window} restore")
            vela.wait_for(lambda s: not s.window(window)["maximized"], what="the window restored")
            vela.wait_still(window)
        time.sleep(1.0) # its first frames, the bar's colors
        return None, (window, other)

    def act(self, desktop, windows):
        for window in windows:
            desktop.vela.command(f"window {window} maximize")
        time.sleep(0.35) # the morph is `slow`, 250 ms


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
        right = math.ceil((w["x"] + w["w"] + self.STEPS + 4) * self.scale)
        top = math.floor(w["y"] * self.scale) - 4
        bottom = math.ceil((w["y"] + BAR + 10) * self.scale) + 4
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
        width, height = 480, 540 # the sound page is the tallest
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

def git(*args, cwd=None):
    # Resolve ROOT at call time; regression tests temporarily replace it with
    # their own isolated Git repository.
    if cwd is None:
        cwd = ROOT
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def worktree_digest(tree):
    """The exact staged and unstaged tracked changes, including --pick changes."""
    diff = subprocess.run(["git", "diff", "--binary", "HEAD"], cwd=tree, check=True,
                          capture_output=True).stdout
    return hashlib.sha256(diff).hexdigest()


def cached_worktree_valid(tree, commit, missing):
    """Reject partial cherry-picks, local edits and caches for other refs."""
    try:
        marker = json.loads((tree / ".vela-before-after-ready.json").read_text())
        return (git("rev-parse", "HEAD", cwd=tree) == commit and
                marker == {"commit": commit, "picks": missing, "diff": worktree_digest(tree)})
    except (OSError, ValueError, subprocess.CalledProcessError):
        return False


def build(ref, jobs, picks=()):
    """Build a checked, reproducible worktree of ref and its missing picks."""
    commit = git("rev-parse", "--verify", f"{ref}^{{commit}}")
    missing = []
    for pick in picks:
        result = subprocess.run(["git", "merge-base", "--is-ancestor", pick, commit], cwd=ROOT)
        if result.returncode == 1:
            missing.append(pick)
        elif result.returncode != 0:
            raise RuntimeError(f"could not check ancestry of {pick} against {commit}")
    tree = CACHE / "+".join(c[:12] for c in (commit, *missing))
    CACHE.mkdir(parents=True, exist_ok=True)
    if tree.exists() and not cached_worktree_valid(tree, commit, missing):
        print(f"{ref}: discarding incomplete or modified cached worktree {tree}", flush=True)
        # This is a dedicated cache, not a user's checkout. Remove Git's
        # worktree registration before recreating it, including failed picks.
        subprocess.run(["git", "worktree", "remove", "--force", str(tree)], cwd=ROOT,
                       check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if tree.exists():
            shutil.rmtree(tree)
        git("worktree", "prune")
    if not tree.exists():
        print(f"{ref}: worktree in {tree}", flush=True)
        git("worktree", "add", "--detach", str(tree), commit)
        for pick in missing:
            print(f"{ref}: applying {pick[:12]}", flush=True)
            git("cherry-pick", "--no-commit", pick, cwd=tree)
        (tree / ".vela-before-after-ready.json").write_text(json.dumps({
            "commit": commit, "picks": missing, "diff": worktree_digest(tree)}, sort_keys=True) + "\n")
    out = tree / "build"
    if not (out / "build.ninja").exists():
        subprocess.run(["cmake", "-S", str(tree), "-B", str(out), "-G", "Ninja"], check=True)
    print(f"{ref}: building {commit[:12]}", flush=True)
    subprocess.run(["cmake", "--build", str(out), *(["-j", str(jobs)] if jobs else [])], check=True)
    return out


def assemble(scene, before, after, out, work, labels):
    duration = (scene.lead + max(before.end - before.start, after.end - after.start) + scene.tail) / scene.speed
    lossless(before, before.frames(duration, scene.speed, scene.lead), work / "before.mkv")
    lossless(after, after.frames(duration, scene.speed, scene.lead), work / "after.mkv")
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
    poster = min((scene.lead + scene.poster) / scene.speed, duration - 1 / FPS)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-ss", f"{poster:.3f}",
                    "-i", str(out / f"{scene.name}.mp4"), "-frames:v", "1", str(out / f"{scene.name}.png")],
                   check=True)


def strip(scene, before, after, out, labels):
    """Consecutive refreshes of the recorded monitor, from a moment of the
    scene: before in the first row, after in the second. Returns, for each
    take, how many refreshes show the same image as the one before."""
    offset, count = scene.strip
    hz = float(scene.size.split("@")[1])
    if (before.width, before.height, before.format) != (after.width, after.height, after.format):
        raise RuntimeError("the two takes have different sizes or formats: no strip")
    rows = [list(take.at(take.start + offset + k / hz for k in range(count))) for take in (before, after)]
    width = before.width * scene.downscale // count # the strip as wide as the output
    graph = f"scale={width}:-2:flags=area"
    if labels:
        for index, label in ((0, "Before"), (count, "After")):
            graph += (f",drawtext=text='{label}':x=10:y=8:fontsize=20:fontcolor=white:box=1"
                      f":boxcolor=black@0.55:boxborderw=6:enable='eq(n,{index})'")
    graph += f",tile={count}x2:padding=4:color=black"
    encoder = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                                "-f", "rawvideo", "-pix_fmt", before.format, "-s", f"{before.width}x{before.height}",
                                "-i", "-", "-vf", graph, "-frames:v", "1", str(out / f"{scene.name}-frames.png")],
                               stdin=subprocess.PIPE)
    for row in rows:
        for frame in row:
            encoder.stdin.write(frame)
    encoder.stdin.close()
    if encoder.wait() != 0:
        raise RuntimeError("ffmpeg failed")
    return [sum(a == b for a, b in zip(row, row[1:])) for row in rows]


# -------------------------------------------------------------------- main --

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="*", help="the scenes to record (default: all)")
    parser.add_argument("--before", help="the ref before the change, such as f72a182^")
    parser.add_argument("--after", default="HEAD", help="the ref after it (default: HEAD)")
    parser.add_argument("--out", type=Path, default=Path.cwd() / "before-after", help="where the videos go")
    parser.add_argument("--pick", action="append", default=[], metavar="COMMIT",
                        help="apply this commit to both builds if they don't have it, such as a fix the recording "
                             "needs (repeatable)")
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
    for program in ("ffmpeg", "dbus-daemon", "cmake", "ninja", "pipewire", "wireplumber", "pw-cat", "pw-dump",
                    "unshare"):
        if not shutil.which(program):
            sys.exit(f"{program} is missing.")
    if not RECORDER.exists():
        sys.exit(f"{RECORDER} is missing: cmake --build build --target vela-screen-record")
    labels = has_drawtext()
    if not labels:
        print("This ffmpeg has no drawtext: the videos won't say which side is which.", file=sys.stderr)

    picks = [git("rev-parse", "--verify", f"{pick}^{{commit}}") for pick in args.pick]
    source_refs = {side: git("rev-parse", "--verify", f"{ref}^{{commit}}")
                   for side, ref in (("before", args.before), ("after", args.after))}
    builds = {side: build(ref, args.jobs, picks) for side, ref in (("before", args.before), ("after", args.after))}
    for name in args.scenes or SCENES:
        scene = SCENES[name]
        work = Path(tempfile.mkdtemp(prefix=f"vela-{name}-"))
        try:
            takes = {}
            for side, build_dir in builds.items():
                directory = work / side
                directory.mkdir()
                print(f"{name}: recording {side}", flush=True)
                with Desktop(build_dir, scene) as desktop:
                    takes[side] = take = record(desktop, scene, directory)
                # Pointer-only presentations do not appear in cursor-free
                # captures; the other output's missing frames matter.
                print(f"{name}: {side}, {len(take.times)} frames captured, {take.presented} presented",
                      flush=True)
            assemble(scene, takes["before"], takes["after"], args.out, work, labels)
            repeated = None
            if scene.strip:
                repeated = strip(scene, takes["before"], takes["after"], args.out, labels)
                count = scene.strip[1]
                for side, n in zip(("before", "after"), repeated):
                    print(f"{name}: {side}, {n} of {count - 1} refreshes repeat the previous image",
                          flush=True)
                print(f"{name}: {args.out / name}-frames.png", flush=True)
            manifest = {
                "scene": name, "size": scene.size, "scale": scene.scale, "speed": scene.speed,
                "downscale": scene.downscale, "pick_commits": picks,
                "sides": {side: {
                    "commit": source_refs[side], "output": take.output_info,
                    "frames_captured": len(take.times), "frames_presented": take.presented,
                    "recorded_width": take.width, "recorded_height": take.height,
                    "pixel_format": take.format,
                } for side, take in takes.items()},
            }
            if repeated is not None:
                manifest["repeated_refreshes"] = dict(zip(("before", "after"), repeated))
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / f"{name}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            if args.keep:
                shutil.copytree(work, args.out / f"{name}-takes", dirs_exist_ok=True)
            print(f"{name}: {args.out / name}.webm, .mp4, .png", flush=True)
        finally:
            shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
