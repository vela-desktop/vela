#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Record repeatable, authentic Vela desktop demos for release trailers.

    cmake --build build --target vela-shell vela-screen-record
    scripts/record-promo.py --list
    scripts/record-promo.py --out ~/videos/vela-beta
    scripts/record-promo.py start-menu window-motion --no-reel --out ~/videos/clips

Each scene runs the *real* compositor and Qt shell on a private headless
session, using the same timestamped capture path as record-before-after.py.
A scene produces MP4, WebM, PNG poster and machine-readable metadata.
When recording multiple scenes, the clips are also cut together as
vela-promo.mp4. No soundtrack, fake UI, or synthetic frame interpolation.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
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
        # Pointer motion is deliberately not in ext-image-copy-capture output.
        desktop.vela.input("move", 980, 640, "sleep", "1600")


class StartMenu(Scene):
    name = "start-menu"
    about = "Start menu entering and leaving the desktop"
    namespace = "vela-start-menu"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        vela = desktop.vela
        vela.input("move", 1020, 650, "key", "super")
        vela.wait_for(lambda s: Desktop.layer(s, self.namespace), what="Start opening")
        time.sleep(1.3)
        vela.input("key", "Escape")
        vela.wait_for(lambda s: not Desktop.layer(s, self.namespace), what="Start closing")
        time.sleep(0.7)


class QuickSettings(Scene):
    name = "quick-settings"
    about = "Quick Settings panel, shown at full desktop resolution"
    namespace = "vela-quick-settings"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        vela = desktop.vela
        vela.input("move", 980, 640, "key", "super+a")
        vela.wait_for(lambda s: Desktop.layer(s, self.namespace), what="Quick Settings opening")
        time.sleep(1.5)
        vela.input("key", "Escape")
        vela.wait_for(lambda s: not Desktop.layer(s, self.namespace), what="Quick Settings closing")
        time.sleep(0.6)


class WindowMotion(Scene):
    name = "window-motion"
    about = "Real window maximize and restore animations"

    def prepare(self, desktop):
        vela = desktop.vela
        window = (desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
                  or vela.open_window(860, 580, decorated=True))
        vela.wait_still(window)
        if vela.state().window(window)["maximized"]:
            vela.command(f"window {window} restore")
            vela.wait_still(window)
        return None, window

    def act(self, desktop, window):
        vela = desktop.vela
        vela.command(f"window {window} maximize")
        vela.wait_still(window)
        time.sleep(0.9)
        vela.command(f"window {window} restore")
        vela.wait_still(window)
        time.sleep(0.9)


class TaskView(Scene):
    name = "task-view"
    about = "Task View with real live window previews"
    namespace = "vela-task-view"

    def prepare(self, desktop):
        vela = desktop.vela
        window = (desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
                  or vela.open_window(860, 580, decorated=True))
        vela.wait_still(window)
        return None, window

    def act(self, desktop, _):
        vela = desktop.vela
        vela.input("move", 980, 630, "key", "super+Tab")
        vela.wait_for(lambda s: Desktop.layer(s, self.namespace), what="Task View opening")
        time.sleep(1.6)
        vela.input("key", "Escape")
        vela.wait_for(lambda s: not Desktop.layer(s, self.namespace), what="Task View closing")
        time.sleep(0.6)


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
        desktop.vela.input("move", 1350, 680, "sleep", "2100")


SCENES = {scene.name: scene for scene in (
    DesktopOverview(), StartMenu(), QuickSettings(), WindowMotion(), TaskView(), Settings()
)}


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


def assemble_reel(clips, output):
    """Concatenate equal-resolution, constant-rate clips; no re-timing of a scene."""
    # Scenes use the same 1920x1080@60 output and encoder settings. Each input
    # is an internal, allowlisted scene name; avoid path interpolation from users.
    concat = output / ".promo-concat.txt"
    try:
        concat.write_text("ffconcat version 1.0\n" +
                          "".join(f"file '{clip.name}'\n" for clip in clips))
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-safe", "0", "-f", "concat", "-i", str(concat),
                        "-an", "-c:v", "libx264", "-crf", "18", "-preset", "slow",
                        "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                        str(output / "vela-promo.mp4")], check=True)
    finally:
        concat.unlink(missing_ok=True)


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
        return 0

    names = args.scenes or list(SCENES)
    unknown = [name for name in names if name not in SCENES]
    if unknown:
        parser.error(f"unknown scene(s): {', '.join(unknown)}")
    if len(names) != len(set(names)):
        parser.error("scene names must be unique")
    output = args.out.resolve()
    collisions = [str(path) for path in output_paths(output, names, not args.no_reel, args.keep)
                  if path.exists()]
    if collisions and not args.force:
        parser.error("refusing to overwrite existing output; use --force: " + ", ".join(collisions))

    build = args.build.resolve()
    capture.RECORDER = build / "tools" / "vela-screen-record"
    if not (build / "shell" / "vela-shell").is_file() or not capture.RECORDER.is_file():
        parser.error("build the shell and recorder first: cmake --build build --target vela-shell vela-screen-record")
    if not capture.harness.gpu_available():
        parser.error("no GPU (/dev/dri/renderD*): Vela requires a Vulkan 1.4 GPU")
    for program in ("ffmpeg", "dbus-daemon", "pipewire", "wireplumber", "pw-cat", "unshare"):
        if not shutil.which(program):
            parser.error(f"required program not found: {program}")

    output.mkdir(parents=True, exist_ok=True)
    clips = []
    for name in names:
        scene = SCENES[name]
        with tempfile.TemporaryDirectory(prefix=f"vela-promo-{name}-") as temporary:
            scratch = Path(temporary)
            take_dir = scratch / "capture"
            take_dir.mkdir()
            print(f"{name}: recording a Vela headless session", flush=True)
            with Desktop(build, scene) as desktop:
                take = record(desktop, scene, take_dir)
            if take.width != 1920 or take.height != 1080:
                raise RuntimeError(f"{name}: expected 1920x1080 capture, got {take.width}x{take.height}")
            clips.append(export_clip(scene, take, output, scratch))
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
            (output / f"{name}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            if args.keep:
                shutil.copytree(take_dir, output / f"{name}-take", dirs_exist_ok=args.force)
        print(f"{name}: MP4, WebM, PNG poster, and manifest in {output}", flush=True)
    if not args.no_reel and len(clips) > 1:
        assemble_reel(clips, output)
        print(f"Reel: {output / 'vela-promo.mp4'}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
