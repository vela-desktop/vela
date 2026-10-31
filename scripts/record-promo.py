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
import hashlib
import json
import os
import re
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
        self.move_pointer(desktop, int(output["w"] * .52), int(output["h"] * .60), duration=0.75)
        self.step(desktop, "show desktop", input_actions=("sleep", "100"), hold=1.2)


class StartMenu(Scene):
    name = "start-menu"
    poster = 0.85  # Show the open panel, not the desktop before the animation.
    about = "Start menu entering and leaving the desktop"
    namespace = "vela-start-menu"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        before = {window["id"] for window in desktop.vela.state().windows}
        self.step(desktop, "open Start", input_actions=("key", "super"),
                  ready=lambda s: Desktop.layer(s, self.namespace), hold=1.0)
        self.step(desktop, "search Files", input_actions=("type", "Files"), hold=1.0)
        self.step(desktop, "launch Vela Files from Start", input_actions=("key", "Return"),
                  ready=lambda s: (not Desktop.layer(s, self.namespace)
                                   and any(window["id"] not in before for window in s.windows)),
                  timeout=15, hold=1.2)


class QuickSettings(Scene):
    name = "quick-settings"
    poster = 0.85
    about = "Quick Settings panel, shown at full desktop resolution"
    namespace = "vela-quick-settings"

    def prepare(self, desktop):
        return None, None

    def act(self, desktop, _):
        # Keep one virtual keyboard alive across the whole sequence. A new
        # vela-input instance can steal focus from an open Quick Settings page.
        # Use the same sound shortcut exercised by the before/after recorder.
        self.step(desktop, "Quick Settings and sound controls",
                  input_actions=("key", "super+a", "sleep", "1000",
                                 "key", "super+ctrl+v", "sleep", "1200",
                                 "key", "Escape", "sleep", "600",
                                 "key", "Escape", "sleep", "500"),
                  ready=lambda s: not Desktop.layer(s, self.namespace), hold=0.5)


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
        self.pause(0.6)
        self.step(desktop, "minimize window", command=f"window {window} minimize",
                  ready=lambda s: s.window(window)["minimized"], hold=0.7)
        self.step(desktop, "restore minimized window", command=f"window {window} restore",
                  ready=lambda s: not s.window(window)["minimized"], hold=0.8)


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
        output = desktop.vela.state().output
        self.move_pointer(desktop, int(output["w"] * 0.62), int(output["h"] * 0.58), duration=0.6)
        self.step(desktop, "show Settings", input_actions=("sleep", "100"), hold=1.5)


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
                  ready=lambda s: s.window(window)["maximized"], hold=0.8)
        desktop.vela.wait_still(window)
        self.step(desktop, "browse demo Pictures",
                  input_actions=("key", "ctrl+l", "type", str(desktop.demo_folder / "Pictures"),
                                 "key", "Return"), hold=1.3)
        self.step(desktop, "navigate back",
                  input_actions=("key", "alt+Left"), hold=1.0)


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
        self.move_pointer(desktop, int(output["w"] * .75), int(output["h"] * .5), duration=0.55)
        self.pause(0.15)  # let the real selection hover become visible
        self.step(desktop, "choose Vela Files in Snap Assist",
                  input_actions=("click", "left"),
                  ready=lambda s: s.window(files)["snap"] == [6, 0, 12, 12], hold=1.3)


class AltTab(Scene):
    name = "alt-tab"
    about = "Show the real application switcher with Settings and Files previews"
    poster = 0.6

    def prepare(self, desktop):
        settings = desktop.open_app("settings/vela-settings", "--page", "personalization", user="Alex")
        if settings is None:
            raise RuntimeError("build vela-settings for Alt+Tab")
        files = desktop.open_app("explorer/vela-files", str(desktop.demo_folder), user="Alex")
        if files is None:
            raise RuntimeError("build vela-files for Alt+Tab")
        desktop.vela.wait_still(files)
        return None, (settings, files)

    def act(self, desktop, _):
        # One continuous virtual-keyboard lifetime preserves held Alt while
        # the compositor shows its live switcher. It is filmed before release.
        self.step(desktop, "switch applications with Alt+Tab",
                  input_actions=("keydown", "alt", "key", "Tab", "sleep", "1100", "keyup", "alt"),
                  hold=0.9)


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
        output = desktop.vela.state().output
        self.move_pointer(desktop, int(output["w"] * 0.58), int(output["h"] * 0.58), duration=0.6)
        self.step(desktop, "show color settings", input_actions=("sleep", "100"), hold=1.5)


SCENES = {scene.name: scene for scene in (
    DesktopOverview(), StartMenu(), QuickSettings(), WindowMotion(), TaskView(), Settings(),
    Files(), SnapAssist(), AltTab(), VirtualDesktops(), Colors()
)}

PROFILES = {
    "beta-trailer": ("desktop", "start-menu", "snap-assist", "task-view", "files",
                     "virtual-desktops", "quick-settings", "colors"),
    "ui-overview": ("desktop", "start-menu", "quick-settings", "settings", "colors", "files"),
    "window-management": ("window-motion", "snap-assist", "alt-tab", "task-view", "virtual-desktops"),
}


def export_clip(scene, take, output, scratch, fps=FPS):
    """Write one scene from true presentation timestamps, not sampled screenshots."""
    duration = max((scene.lead + take.end - take.start + scene.tail) / scene.speed, 1 / fps)
    mezzanine = scratch / "clip.mkv"
    lossless(take, take.frames(duration, scene.speed, scene.lead, fps=fps), mezzanine, fps=fps)
    vf = "pad=ceil(iw/2)*2:ceil(ih/2)*2,format=yuv420p"
    input_args = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(mezzanine),
                  "-vf", vf, "-an"]
    mp4 = output / f"{scene.name}.mp4"
    webm = output / f"{scene.name}.webm"
    subprocess.run([*input_args, "-c:v", "libx264", "-crf", "18", "-preset", "slow",
                    "-movflags", "+faststart", str(mp4)], check=True)
    subprocess.run([*input_args, "-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0", "-row-mt", "1",
                    str(webm)], check=True)
    poster_at = max(0, min((scene.lead + scene.poster) / scene.speed, duration - 1 / fps))
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                    "-ss", f"{poster_at:.3f}", "-i", str(mp4), "-frames:v", "1",
                    str(output / f"{scene.name}.png")], check=True)
    return mp4


def assemble_reel(clips, output, staging, config=None):
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
        signature = reel_signature(clips, config)
        (staging / "vela-promo-manifest.json").write_text(
            json.dumps({"signature": signature, "reel": artifact_digest(staging / "vela-promo.mp4")},
                       indent=2) + "\n")
        install(staging / "vela-promo.mp4", output)
        install(staging / "vela-promo-manifest.json", output)
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


def artifact_digest(path):
    """Stable digest for a completed output (not just existence or file size)."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"sha256": digest.hexdigest(), "size": path.stat().st_size}


def capture_artifacts(name, output, keep):
    """Hash the finished exports and, if kept, each raw capture file."""
    paths = [output / f"{name}{suffix}" for suffix in (".mp4", ".webm", ".png")]
    if keep:
        paths.extend(path for path in sorted((output / f"{name}-take").rglob("*")) if path.is_file())
    return {str(path.relative_to(output)): artifact_digest(path) for path in paths}


def recipe_fingerprint():
    """Invalidate reused footage when the actual scene/recording code changes."""
    return build_fingerprint({
        "record-promo.py": Path(__file__).resolve(),
        "recording_core.py": Path(capture.__file__).resolve(),
    })


def reel_signature(clips, config):
    """Both scene ORDER and exact MP4 bytes define a reel."""
    return {"scenes": [clip.stem for clip in clips],
            "capture_config": config,
            "clips": {clip.name: artifact_digest(clip) for clip in clips}}


def reel_matches(clips, output, config):
    reel = output / "vela-promo.mp4"
    manifest = output / "vela-promo-manifest.json"
    if not reel.is_file() or not manifest.is_file() or reel.is_symlink() or manifest.is_symlink():
        return False
    try:
        previous = json.loads(manifest.read_text())
        return (previous.get("signature") == reel_signature(clips, config)
                and previous.get("reel") == artifact_digest(reel))
    except (OSError, ValueError, TypeError):
        return False


def build_fingerprint(binaries):
    """SHA-256 of the executables that actually produced the demo.

    The source commit is not proof of which compiled binaries were executed.
    """
    result = hashlib.sha256()
    for name, path in sorted(binaries.items()):
        result.update(name.encode() + b"\0")
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                result.update(chunk)
        result.update(b"\0")
    return result.hexdigest()


def capture_config(args, fingerprint):
    return {"resolution": getattr(args, "resolution", "1920x1080"),
            "scale": getattr(args, "scale", 1.0),
            "fps": getattr(args, "fps", FPS),
            "speed": getattr(args, "speed", 1.0),
            "hold_scale": getattr(args, "hold_scale", 1.0),
            "cursor": getattr(args, "cursor", False),
            "build_fingerprint": fingerprint,
            "recipe_fingerprint": recipe_fingerprint()}


def completed_scene(name, output, config, keep):
    """A resumable scene needs *all* files and matching capture settings."""
    paths = output_paths(output, [name], False, keep)
    present = [path.exists() for path in paths]
    if not any(present):
        return False
    if not all(present):
        raise ValueError(f"{name}: partial outputs from an interrupted run; use a clean directory or --force")
    try:
        manifest = json.loads((output / f"{name}-manifest.json").read_text())
    except (OSError, ValueError) as error:
        raise ValueError(f"{name}: invalid manifest; cannot resume") from error
    if manifest.get("capture_config") != config or manifest.get("scene") != name:
        raise ValueError(f"{name}: capture settings or recording fingerprint changed; cannot resume")
    artifacts = manifest.get("artifacts")
    required = {f"{name}{suffix}" for suffix in (".mp4", ".webm", ".png")}
    if keep:
        required.update({f"{name}-take/frames.raw", f"{name}-take/frames.txt"})
    if not isinstance(artifacts, dict) or not required.issubset(artifacts):
        raise ValueError(f"{name}: manifest lacks complete output checksums; use --force to re-record")
    for relative, expected in artifacts.items():
        if not isinstance(relative, str) or not isinstance(expected, dict):
            raise ValueError(f"{name}: invalid output digest; use --force")
        path = output / relative
        if (Path(relative).is_absolute() or ".." in Path(relative).parts
                or not path.is_file() or path.is_symlink()
                or artifact_digest(path) != expected):
            raise ValueError(f"{name}: output integrity check failed for {relative}; use --force")
    return True


def output_paths(output, names, make_reel, keep):
    paths = []
    for name in names:
        paths.extend(output / f"{name}{suffix}"
                     for suffix in (".mp4", ".webm", ".png", "-manifest.json"))
        if keep:
            paths.append(output / f"{name}-take")
    if make_reel and len(names) > 1:
        paths.extend((output / "vela-promo.mp4", output / "vela-promo-manifest.json"))
    return paths


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scenes", nargs="*", help="explicit scene names override --profile")
    parser.add_argument("--list", action="store_true", help="show available scenes")
    parser.add_argument("--list-profiles", action="store_true", help="show curated recording profiles")
    parser.add_argument("--profile", choices=tuple(PROFILES), default="beta-trailer",
                        help="curated scenes (default: beta-trailer)")
    parser.add_argument("--resolution", default="1920x1080", metavar="WIDTHxHEIGHT",
                        help="physical recording dimensions (default: 1920x1080)")
    parser.add_argument("--scale", type=float, default=1.0, help="headless compositor scale")
    parser.add_argument("--fps", type=int, choices=(30, 60), default=60,
                        help="encoded FPS (capture runs at 60 Hz; default: 60)")
    parser.add_argument("--speed", type=float, default=1.0, help="playback speed multiplier")
    parser.add_argument("--hold-scale", type=float, default=1.0,
                        help="multiplier for editorial scene pauses")
    parser.add_argument("--cursor", action=argparse.BooleanOptionalAction, default=True,
                        help="capture the real compositor cursor (default: on)")
    parser.add_argument("--out", type=Path, default=Path.cwd() / "promo",
                        help="output directory (default: ./promo)")
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build",
                        help="Vela CMake build to record (default: ./build)")
    parser.add_argument("--no-reel", action="store_true", help="skip combined vela-promo.mp4")
    parser.add_argument("--keep", action="store_true", help="retain very large raw frame captures")
    parser.add_argument("--force", action="store_true", help="overwrite existing output files")
    parser.add_argument("--resume", action="store_true",
                        help="reuse complete scenes with matching binary fingerprint and settings")
    args = parser.parse_args(argv)

    if args.list:
        for scene in SCENES.values():
            print(f"{scene.name:18} {scene.about}")
        return
    if args.list_profiles:
        for name, scenes in PROFILES.items():
            print(f"{name}: {', '.join(scenes)}")
        return
    names = args.scenes or list(PROFILES[args.profile])
    unknown = [name for name in names if name not in SCENES]
    if unknown:
        parser.error(f"unknown scene(s): {', '.join(unknown)}")
    if len(names) != len(set(names)):
        parser.error("scene names must be unique")
    if args.resume and args.force:
        parser.error("--resume and --force cannot be combined")
    match = re.fullmatch(r"(\d+)x(\d+)", args.resolution)
    if not match:
        parser.error("--resolution must be WIDTHxHEIGHT")
    width, height = map(int, match.groups())
    if not (640 <= width <= 4096 and 480 <= height <= 2160 and width % 2 == 0 and height % 2 == 0):
        parser.error("resolution must be even dimensions between 640x480 and 4096x2160")
    if not (0.5 <= args.scale <= 3.0 and 0.25 <= args.speed <= 4.0 and 0.25 <= args.hold_scale <= 4.0):
        parser.error("scale must be 0.5–3, speed and hold-scale must be 0.25–4")
    output = args.out.resolve()
    planned = output_paths(output, names, not args.no_reel, args.keep)
    symlinks = [str(path) for path in planned if path.is_symlink()]
    if symlinks:
        parser.error("refusing symlink output paths: " + ", ".join(symlinks))
    collisions = [str(path) for path in planned if path.exists()]
    if collisions and not (args.force or args.resume):
        parser.error("refusing to overwrite existing output; use --force: " + ", ".join(collisions))

    build = args.build.resolve()
    capture.RECORDER = build / "tools" / "vela-screen-record"
    binaries = {"compositor": build / "compositor" / "vela-compositor",
                "shell": build / "shell" / "vela-shell",
                "recorder": capture.RECORDER,
                "input": build / "tools" / "vela-input"}
    if set(names) & {"window-motion", "task-view", "settings", "snap-assist", "alt-tab", "virtual-desktops", "colors"}:
        binaries["settings"] = build / "settings" / "vela-settings"
    if set(names) & {"files", "task-view", "snap-assist", "alt-tab", "start-menu"}:
        binaries["files"] = build / "explorer" / "vela-files"
    missing = [name for name, path in binaries.items() if not path.is_file()]
    if missing:
        parser.error("missing build target(s): " + ", ".join(missing) +
                     "; cmake --build build --target vela-compositor vela-shell "
                     "vela-screen-record vela-input vela-settings vela-files")
    fingerprint = build_fingerprint(binaries)
    config = capture_config(args, fingerprint)
    completed = set()
    if args.resume:
        try:
            completed = {name for name in names if completed_scene(name, output, config, args.keep)}
        except ValueError as error:
            parser.error(str(error))
        reel = output / "vela-promo.mp4"
        # The reel is regenerated only if new clips are recorded, or it is missing.
        if reel.exists() and not args.no_reel and len(names) > 1 and not completed == set(names):
            pass  # --resume explicitly authorizes replacing this generated reel.
    for name in names:
        scene = SCENES[name]
        scene.size = f"{width}x{height}@60"
        scene.scale = args.scale
        scene.speed = args.speed
        scene.hold_scale = args.hold_scale

    if (args.resume and not args.no_reel and len(names) > 1
            and completed == set(names)
            and not reel_matches([output / f"{name}.mp4" for name in names], output, config)
            and not shutil.which("ffmpeg")):
        parser.error("ffmpeg is required to regenerate the requested reel")
    if not completed == set(names):
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
    staging = Path(tempfile.mkdtemp(dir=output, prefix=".vela-promo-staging-"))
    try:
        record_scenes(names, build, output, staging, args, config=config, completed=completed)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def record_scenes(names, build, output, staging, args, *, config=None, completed=()):
    clips = []
    config = config or capture_config(args, "unverified")
    for name in names:
        if name in completed:
            clips.append(output / f"{name}.mp4")
            print(f"{name}: reused matching complete recording", flush=True)
            continue
        scene = SCENES[name]
        with tempfile.TemporaryDirectory(prefix=f"vela-promo-{name}-") as temporary:
            scratch = Path(temporary)
            take_dir = scratch / "capture"
            take_dir.mkdir()
            print(f"{name}: recording a Vela headless session", flush=True)
            with Desktop(build, scene) as desktop:
                take = record(desktop, scene, take_dir, align=False,
                              cursor=getattr(args, "cursor", False), one_keyboard=True)
            resolution = getattr(args, "resolution", "1920x1080")
            width, height = map(int, resolution.split("x"))
            if (take.width, take.height) != (width, height):
                raise RuntimeError(f"{name}: expected {resolution} capture, got {take.width}x{take.height}")
            mp4 = export_clip(scene, take, staging, scratch, fps=getattr(args, "fps", FPS))
            manifest = {
                "scene": name,
                "capture_config": config,
                "size": scene.size,
                "scale": scene.scale,
                "speed": scene.speed,
                "duration": round((take.end - take.start) / scene.speed, 3),
                "timeline": getattr(take, "timeline", []),
                "frames_captured": len(take.times),
                "frames_presented": take.presented,
                "pixel_format": take.format,
                "output": take.output_info,
                "recording": "real Vela headless compositor and Qt shell",
                "source": "binary SHA-256 fingerprint (source commit not independently verified)",
                "cursor": "compositor cursor" if getattr(args, "cursor", False) else "off",
            }
            if args.keep:
                shutil.copytree(take_dir, staging / f"{name}-take")
            manifest["artifacts"] = capture_artifacts(name, staging, args.keep)
            (staging / f"{name}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            # Commit the manifest last: an interrupted output set cannot pass --resume.
            for suffix in (".webm", ".png", ".mp4", *(("-take",) if args.keep else ()),
                           "-manifest.json"):
                install(staging / f"{name}{suffix}", output)
            clips.append(output / f"{name}.mp4")
        print(f"{name}: MP4, WebM, PNG poster, and manifest in {output}", flush=True)
    if not args.no_reel and len(clips) > 1 and (
            len(completed) != len(names) or not reel_matches(clips, output, config)):
        assemble_reel(clips, output, staging, config)
        print(f"Reel: {output / 'vela-promo.mp4'}", flush=True)


if __name__ == "__main__":
    main()  # Errors exit through argparse or an exception.
