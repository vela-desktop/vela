#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Record repeatable, authentic Vela desktop demos for release trailers.

    cmake --build build --target vela-shell vela-screen-record
    python3 scripts/record-promo.py --list
    python3 scripts/record-promo.py --out ~/videos/vela-beta
    python3 scripts/record-promo.py start-menu window-motion --no-reel --out ~/videos/clips

Each scene runs the *real* compositor and Qt shell on a private headless
session, using the same timestamped capture path as record-before-after.py.
A scene produces MP4, WebM, PNG poster and machine-readable metadata.
When recording multiple scenes, the clips are also cut together as
vela-promo.mp4. No soundtrack, fake UI, or synthetic frame interpolation.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import recording_core as capture
from recording_core import Desktop, Scene, FPS, lossless, record


class DesktopOverview(Scene):
    name = "desktop"
    about = "Clean Vela desktop and wallpaper, without personal data"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        output = desktop.vela.state().output
        self.step(desktop, "show desktop", input_actions=("move", int(output["w"] * .52),
                  int(output["h"] * .60)), hold=1.6)


class StartMenu(Scene):
    name = "start-menu"
    poster = 0.85  # Show the open panel, not the desktop before the animation.
    about = "Start menu entering and leaving the desktop"
    namespace = "vela-start-menu"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        vela = desktop.vela
        self.step(desktop, "open Start", input_actions=("key", "super"),
                  ready=lambda s: Desktop.layer(s, self.namespace), hold=1.0)
        self.step(desktop, "search Files", input_actions=("type", "Files"), hold=1.0)
        self.step(desktop, "close Start", input_actions=("key", "Escape"),
                  ready=lambda s: not Desktop.layer(s, self.namespace), hold=0.5)


class QuickSettings(Scene):
    name = "quick-settings"
    poster = 0.85
    about = "Quick Settings panel, shown at full desktop resolution"
    namespace = "vela-quick-settings"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        vela = desktop.vela
        self.step(desktop, "open Quick Settings", input_actions=("key", "super+a"),
                  ready=lambda s: Desktop.layer(s, self.namespace), hold=1.5)
        self.step(desktop, "close Quick Settings", input_actions=("key", "Escape"),
                  ready=lambda s: not Desktop.layer(s, self.namespace), hold=0.6)


class WindowMotion(Scene):
    name = "window-motion"
    poster = 0.25
    about = "Real window maximize and restore animations"

    def prepare(self, desktop):
        vela = desktop.vela
        window = desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
        if window is None:
            raise RuntimeError("build vela-settings: promo footage must show real Vela windows")
        vela.wait_still(window)
        if vela.state().window(window)["maximized"]:
            vela.command(f"window {window} restore")
            vela.wait_still(window)
        return None, window

    def act(self, desktop, window):
        vela = desktop.vela
        self.step(desktop, "maximize window", command=f"window {window} maximize",
                  ready=lambda s: s.window(window)["maximized"])
        vela.wait_still(window)
        self.pause(0.9)
        self.step(desktop, "restore window", command=f"window {window} restore",
                  ready=lambda s: not s.window(window)["maximized"])
        vela.wait_still(window)
        self.pause(0.9)


class TaskView(Scene):
    name = "task-view"
    poster = 0.85
    about = "Task View with real live window previews"
    namespace = "vela-task-view"

    def prepare(self, desktop):
        vela = desktop.vela
        window = desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
        if window is None:
            raise RuntimeError("build vela-settings: Task View needs a real app preview")
        vela.wait_still(window)
        files = desktop.open_app("explorer/vela-files", str(desktop.demo_folder), user="Alex")
        if files is None:
            raise RuntimeError("build vela-files: Task View needs two authentic app previews")
        vela.wait_still(files)
        return None, (window, files)

    def act(self, desktop, _):
        self.step(desktop, "open Task View", input_actions=("key", "super+Tab"),
                  ready=lambda s: Desktop.layer(s, self.namespace), hold=1.6)
        self.step(desktop, "close Task View", input_actions=("key", "Escape"),
                  ready=lambda s: not Desktop.layer(s, self.namespace), hold=0.6)


class Settings(Scene):
    name = "settings"
    about = "Vela Settings with a synthetic public demo user and private devices"

    def prepare(self, desktop):
        window = desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
        if window is None:
            raise RuntimeError("the Vela Settings app must be built for the settings scene")
        desktop.vela.wait_still(window)
        return None, window

    def act(self, desktop, _):
        self.step(desktop, "show Settings", input_actions=("move", 1100, 680), hold=2.1)


class Files(Scene):
    name = "files"
    about = "Vela Files browsing a synthetic, isolated demo directory"
    poster = 0.7

    def prepare(self, desktop):
        window = desktop.open_app("explorer/vela-files", str(desktop.demo_folder), user="Alex")
        if window is None:
            raise RuntimeError("build vela-files for the Files promo scene")
        desktop.vela.wait_still(window)
        return None, window

    def act(self, desktop, window):
        self.step(desktop, "maximize Vela Files", command=f"window {window} maximize",
                  ready=lambda s: s.window(window)["maximized"], hold=1.5)
        desktop.vela.wait_still(window)


class SnapAssist(Scene):
    name = "snap-assist"
    about = "Snap real Settings and Files windows into two side-by-side zones"
    poster = 1.1
    namespace = "vela-snap-assist"

    def prepare(self, desktop):
        settings = desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
        if settings is None:
            raise RuntimeError("build vela-settings for the Snap Assist scene")
        files = desktop.open_app("explorer/vela-files", str(desktop.demo_folder), user="Alex")
        if files is None:
            raise RuntimeError("build vela-files for the Snap Assist scene")
        desktop.vela.wait_still(settings)
        desktop.vela.wait_still(files)
        return None, (settings, files)

    def act(self, desktop, windows):
        settings, files = windows
        self.step(desktop, "snap Settings to left", command=f"window {settings} snap 0 0 6 12",
                  ready=lambda s: s.window(settings)["snap"] == [0, 0, 6, 12], hold=0.7)
        desktop.vela.wait_for(lambda s: Desktop.layer(s, self.namespace),
                              what="Snap Assist offers remaining windows")
        output = desktop.vela.state().output
        # With one remaining app, its card is centered in the free right half.
        self.step(desktop, "choose Vela Files in Snap Assist",
                  input_actions=("move", int(output["w"] * .75), int(output["h"] * .5),
                                 "sleep", 150, "click", "left"),
                  ready=lambda s: s.window(files)["snap"] == [6, 0, 12, 12], hold=1.3)


class VirtualDesktops(Scene):
    name = "virtual-desktops"
    about = "Switch between two real Vela virtual desktops"
    poster = 0.8

    def prepare(self, desktop):
        window = desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
        if window is None:
            raise RuntimeError("build vela-settings for virtual desktops")
        desktop.vela.wait_still(window)
        desktop.vela.command("workspace new")
        desktop.vela.wait_for(lambda s: s["workspaces"] >= 2, what="second workspace")
        return None, window

    def act(self, desktop, _):
        self.step(desktop, "switch to second desktop", command="workspace switch 1",
                  ready=lambda s: s["workspace"] == 1, hold=1.1)
        self.step(desktop, "return to first desktop", command="workspace switch 0",
                  ready=lambda s: s["workspace"] == 0, hold=1.1)


class Colors(Scene):
    name = "colors"
    about = "Vela Settings: real light/dark mode and accent controls"
    poster = 0.7

    def prepare(self, desktop):
        window = desktop.open_app("settings/vela-settings", "--page", "colors", user="Alex")
        if window is None:
            raise RuntimeError("build vela-settings for colors")
        desktop.vela.wait_still(window)
        return None, window

    def act(self, desktop, _):
        self.step(desktop, "show color settings", input_actions=("move", 1120, 650), hold=2.0)


SCENES = {scene.name: scene for scene in (
    DesktopOverview(), StartMenu(), QuickSettings(), WindowMotion(), TaskView(), Settings(),
    Files(), SnapAssist(), VirtualDesktops(), Colors()
)}

PROFILES = {
    "beta-trailer": ("desktop", "start-menu", "snap-assist", "task-view", "files",
                     "virtual-desktops", "quick-settings", "colors"),
    "ui-overview": ("desktop", "start-menu", "quick-settings", "settings", "colors", "files"),
    "window-management": ("window-motion", "snap-assist", "task-view", "virtual-desktops"),
}


def export_clip(scene, take, output, scratch):
    """Write one scene from true presentation timestamps, not sampled screenshots."""
    duration = max((scene.lead + take.end - take.start + scene.tail) / scene.speed, 1 / FPS)
    mezzanine = scratch / "clip.mkv"
    lossless(take, take.frames(duration, scene.speed, scene.lead), mezzanine)
    vf = "pad=ceil(iw/2)*2:ceil(ih/2)*2,format=yuv420p"
    input_args = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(mezzanine),
                  "-vf", vf, "-an"]
    mp4 = output / f"{scene.name}.mp4"
    webm = output / f"{scene.name}.webm"
    subprocess.run([*input_args, "-c:v", "libx264", "-crf", "18", "-preset", "slow",
                    "-movflags", "+faststart", str(mp4)], check=True)
    subprocess.run([*input_args, "-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0", "-row-mt", "1",
                    str(webm)], check=True)
    poster_at = max(0, min((scene.lead + scene.poster) / scene.speed, duration - 1 / FPS))
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                    "-ss", f"{poster_at:.3f}", "-i", str(mp4), "-frames:v", "1",
                    str(output / f"{scene.name}.png")], check=True)
    return mp4


def assemble_reel(clips, output, staging):
    """Concatenate equal-resolution, constant-rate clips; no re-timing of a scene."""
    # Scenes use the same 1920x1080@60 output and encoder settings. Each input
    # is an internal, allowlisted scene name; avoid path interpolation from users.
    if any(clip.parent.resolve() != output.resolve() or clip.stem not in SCENES or
           clip.suffix != ".mp4" for clip in clips):
        raise ValueError("reel inputs must be generated MP4 clips for known scenes in the output directory")
    # Create the playlist exclusively: never overwrite a predictable name or symlink.
    with tempfile.NamedTemporaryFile("w", dir=output, prefix=".vela-promo-",
                                     suffix=".ffconcat", delete=False) as playlist:
        concat = Path(playlist.name)
        playlist.write("ffconcat version 1.0\n" +
                       "".join(f"file '{clip.name}'\n" for clip in clips))
    try:
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-safe", "1", "-f", "concat", "-i", str(concat),
                        "-an", "-c:v", "libx264", "-crf", "18", "-preset", "slow",
                        "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                        str(staging / "vela-promo.mp4")], check=True)
        install(staging / "vela-promo.mp4", output)
    finally:
        concat.unlink(missing_ok=True)


def install(path, output):
    """Move a finished output from the staging directory into place. A rename
    replaces whatever is at the destination, even a symlink planted after the
    checks, and never follows it."""
    target = output / path.name
    if path.is_dir() and target.is_dir() and not target.is_symlink():
        shutil.rmtree(target)  # --force: a previous take; rename only replaces an empty directory
    os.replace(path, target)
    return target


def output_paths(output, names, make_reel, keep):
    paths = []
    for name in names:
        paths.extend(output / f"{name}{suffix}"
                     for suffix in (".mp4", ".webm", ".png", "-manifest.json"))
        if keep:
            paths.append(output / f"{name}-take")
    if make_reel and len(names) > 1:
        paths.append(output / "vela-promo.mp4")
    return paths


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="*", help="scene names (default: all)")
    parser.add_argument("--list", action="store_true", help="show available scenes")
    parser.add_argument("--out", type=Path, default=Path.cwd() / "promo",
                        help="output directory (default: ./promo)")
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build",
                        help="Vela CMake build to record (default: ./build)")
    parser.add_argument("--no-reel", action="store_true", help="skip combined vela-promo.mp4")
    parser.add_argument("--keep", action="store_true", help="retain very large raw frame captures")
    parser.add_argument("--force", action="store_true", help="overwrite existing output files")
    args = parser.parse_args(argv)

    if args.list:
        for scene in SCENES.values():
            print(f"{scene.name:16} {scene.about}")
        return

    names = args.scenes or list(SCENES)
    unknown = [name for name in names if name not in SCENES]
    if unknown:
        parser.error(f"unknown scene(s): {', '.join(unknown)}")
    if len(names) != len(set(names)):
        parser.error("scene names must be unique")
    output = args.out.resolve()
    planned = output_paths(output, names, not args.no_reel, args.keep)
    symlinks = [str(path) for path in planned if path.is_symlink()]
    if symlinks:
        parser.error("refusing symlink output paths: " + ", ".join(symlinks))
    collisions = [str(path) for path in planned if path.exists()]
    if collisions and not args.force:
        parser.error("refusing to overwrite existing output; use --force: " + ", ".join(collisions))

    build = args.build.resolve()
    capture.RECORDER = build / "tools" / "vela-screen-record"
    required = [build / "compositor" / "vela-compositor",
                build / "shell" / "vela-shell", capture.RECORDER,
                build / "tools" / "vela-input"]
    if any(not path.is_file() for path in required):
        parser.error("build the compositor, shell and recorder first: "
                     "cmake --build build --target vela-compositor vela-shell vela-screen-record vela-input")
    with_settings = set(names) & {"window-motion", "task-view", "settings"}
    if with_settings and not (build / "settings" / "vela-settings").is_file():
        parser.error("build vela-settings for window-motion, task-view and settings scenes")
    if not capture.harness.gpu_available():
        parser.error("no GPU (/dev/dri/renderD*): Vela requires a Vulkan 1.4 GPU")
    for program in ("ffmpeg", "dbus-daemon", "pipewire", "wireplumber", "pw-cat", "pw-dump", "unshare"):
        if not shutil.which(program):
            parser.error(f"required program not found: {program}")
    try:
        capture.check_user_namespaces()
    except RuntimeError as error:
        parser.error(str(error))

    output.mkdir(parents=True, exist_ok=True)
    # Everything is written here first, private and unpredictable, then
    # renamed into place: no write follows a path someone else can plant.
    staging = Path(tempfile.mkdtemp(dir=output, prefix=".vela-promo-staging-"))
    try:
        record_scenes(names, build, output, staging, args)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def record_scenes(names, build, output, staging, args):
    clips = []
    for name in names:
        scene = SCENES[name]
        with tempfile.TemporaryDirectory(prefix=f"vela-promo-{name}-") as temporary:
            scratch = Path(temporary)
            take_dir = scratch / "capture"
            take_dir.mkdir()
            print(f"{name}: recording a Vela headless session", flush=True)
            with Desktop(build, scene) as desktop:
                take = record(desktop, scene, take_dir, align=False)
            if take.width != 1920 or take.height != 1080:
                raise RuntimeError(f"{name}: expected 1920x1080 capture, got {take.width}x{take.height}")
            mp4 = export_clip(scene, take, staging, scratch)
            manifest = {
                "scene": name,
                "size": scene.size,
                "scale": scene.scale,
                "speed": scene.speed,
                "frames_captured": len(take.times),
                "frames_presented": take.presented,
                "pixel_format": take.format,
                "output": take.output_info,
                "recording": "real Vela headless compositor and Qt shell",
                "source": "user-supplied build directory (commit not independently verified)",
                "cursor": "not included in ext-image-copy-capture",
            }
            (staging / f"{name}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            if args.keep:
                shutil.copytree(take_dir, staging / f"{name}-take")
            for suffix in (".webm", ".png", "-manifest.json", *(("-take",) if args.keep else ())):
                install(staging / f"{name}{suffix}", output)
            clips.append(install(mp4, output))
        print(f"{name}: MP4, WebM, PNG poster, and manifest in {output}", flush=True)
    if not args.no_reel and len(clips) > 1:
        assemble_reel(clips, output, staging)
        print(f"Reel: {output / 'vela-promo.mp4'}", flush=True)


if __name__ == "__main__":
    main()  # Errors exit through argparse or an exception.
