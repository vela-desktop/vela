#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Verify recorded Vela promo footage on a real-GPU runner.

Requires ffprobe and ffmpeg, but no Python video packages:
    python3 scripts/verify-promo.py --out /tmp/vela-promo desktop snap-assist
"""

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    return {"sha256": sha.hexdigest(), "size": path.stat().st_size}


def media_info(path):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,r_frame_rate,nb_frames:format=duration",
         "-of", "json", str(path)],
        check=True, capture_output=True, text=True)
    parsed = json.loads(result.stdout)
    if not parsed.get("streams"):
        raise ValueError(f"{path.name}: video stream missing")
    return parsed["streams"][0], parsed.get("format", {})


def consecutive_black_samples(raw, sample_pixels, *, threshold=10, ratio=0.995):
    """Reject long fully-black stretches, not legitimate short transition frames."""
    if not raw or len(raw) % sample_pixels:
        raise ValueError("missing or truncated sampled frames")
    streak = 0
    for offset in range(0, len(raw), sample_pixels):
        sample = raw[offset:offset + sample_pixels]
        is_black = sum(pixel <= threshold for pixel in sample) >= ratio * sample_pixels
        streak = streak + 1 if is_black else 0
        if streak >= 2:
            return True
    return False


def inspect_clip(name, output):
    manifest_path = output / f"{name}-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("scene") != name:
        raise ValueError(f"{name}: manifest scene mismatch")
    settings = manifest.get("capture_config", {})
    dimensions = settings["resolution"].split("x")
    expected_width, expected_height = map(int, dimensions)
    expected_fps = settings["fps"]

    artifacts = manifest.get("artifacts", {})
    for suffix in (".mp4", ".webm", ".png"):
        key = f"{name}{suffix}"
        path = output / key
        if path.is_symlink() or not path.is_file() or artifacts.get(key) != digest(path):
            raise ValueError(f"{name}: missing or corrupt {key}")

    if manifest.get("frames_captured", 0) <= 0 or manifest.get("frames_presented", 0) <= 0:
        raise ValueError(f"{name}: no captured/presented compositor frames")
    missed = manifest.get("frames_missed")
    repeated = manifest.get("repeated_frames")
    if not isinstance(missed, int) or missed < 0 or not isinstance(repeated, list):
        raise ValueError(f"{name}: frame timing missing from the manifest")
    events = manifest.get("timeline")
    if not isinstance(events, list) or not events:
        raise ValueError(f"{name}: action timeline missing")
    for event in events:
        if not (0 <= event["at"] <= event["ready_at"] <=
                manifest["duration"] * settings["speed"] + 0.5):
            raise ValueError(f"{name}: action checkpoints outside the capture timeline")

    png = (output / f"{name}.png").read_bytes()[:24]
    if (len(png) < 24 or png[:8] != b"\x89PNG\r\n\x1a\n"
            or struct.unpack(">II", png[16:24]) != (expected_width, expected_height)):
        raise ValueError(f"{name}: poster dimensions invalid")

    for ext in ("mp4", "webm"):
        clip = output / f"{name}.{ext}"
        video, container = media_info(clip)
        if ((video["width"], video["height"]) != (expected_width, expected_height)
                or Fraction(video["r_frame_rate"]) != expected_fps
                or float(container.get("duration", 0)) <= 0):
            raise ValueError(f"{name}: invalid {ext} geometry, frame rate or duration")

    # Downsample decoded MP4 frames to 64x36 gray at 4 fps, checking for black
    # stretches over ~0.5 seconds. Do not confuse a single transitional frame
    # with a compositor failure.
    sampled = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(output / f"{name}.mp4"),
         "-vf", "fps=4,scale=64:36,format=gray", "-f", "rawvideo", "-an", "-"],
        capture_output=True, check=True).stdout
    if consecutive_black_samples(sampled, 64 * 36):
        raise ValueError(f"{name}: consecutive black frames in rendered video")
    return {"scene": name, "duration": manifest["duration"],
            "captured": manifest["frames_captured"], "presented": manifest["frames_presented"],
            "missed": missed, "repeated": repeated}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, required=True, help="completed recording directory")
    parser.add_argument("scenes", nargs="+", help="scene names to verify")
    args = parser.parse_args(argv)
    for name in args.scenes:
        if not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in name):
            parser.error(f"invalid scene name: {name}")
        report = inspect_clip(name, args.out)
        print(f"{name}: verified, {report['captured']} captured / {report['presented']} presented frames")
        # For the reviewer, not a failure: the first frame after a still
        # moment can be late once, and typing shows a frame twice at its own
        # pace. Many of either, though, is a stutter worth looking at.
        if report["missed"]:
            print(f"  {name}: the compositor presented {report['missed']} vblank(s) late")
        if report["repeated"]:
            print(f"  {name}: a frame shown twice mid-motion at "
                  + ", ".join(f"{t:.3f}" for t in report["repeated"]) + " s")


if __name__ == "__main__":
    main()
