#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""The values the compositor and QML both use, written on each side and kept
equal here (docs/visual-coherence.md §6.4): the four durations and the curve
of compositor/src/motion.h and common/controls/Theme.qml, the polkit prompt's
wait for its fade-out, and the title bar buttons' hover (decoration.c), which
is Theme.subtleHover (§6.6)."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOTION_H = ROOT / "compositor/src/motion.h"
THEME_QML = ROOT / "common/controls/Theme.qml"
PROMPT_CPP = ROOT / "polkit/prompt/main.cpp"
DECORATION_C = ROOT / "compositor/src/decoration.c"
TOKENS = ("fast", "normal", "slow", "long")
NUMBER = r"-?\d+(?:\.\d+)?"


def compositor_values(text: str) -> tuple[dict[str, float], list[float]]:
    """The scale (VELA_MOTION_<TOKEN>_MS) and VELA_DECELERATE_CURVE."""
    scale = {}
    for token in TOKENS:
        m = re.search(rf"^#define\s+VELA_MOTION_{token.upper()}_MS\s+({NUMBER})\s*$", text, re.M)
        if not m:
            raise ValueError(f"motion.h: VELA_MOTION_{token.upper()}_MS not found")
        scale[token] = float(m[1])
    m = re.search(r"^#define\s+VELA_DECELERATE_CURVE\s+(.+?)\s*$", text, re.M)
    if not m:
        raise ValueError("motion.h: VELA_DECELERATE_CURVE not found")
    return scale, [float(v) for v in m[1].split(",")]


def qml_values(text: str) -> tuple[dict[str, float], list[float]]:
    """Theme.scale and Theme.decelerate (QML's bezierCurve: the two control
    points, then the end point, which must be (1, 1))."""
    m = re.search(r"readonly property var scale:\s*\(\{(.*?)\}\)", text)
    if not m:
        raise ValueError("Theme.qml: scale not found")
    scale = {key: float(value) for key, value in re.findall(rf"(\w+)\s*:\s*({NUMBER})", m[1])}
    if sorted(scale) != sorted(TOKENS):
        raise ValueError(f"Theme.qml: scale has {sorted(scale)}, expected {sorted(TOKENS)}")
    m = re.search(r"readonly property var decelerate:\s*\[(.*?)\]", text)
    if not m:
        raise ValueError("Theme.qml: decelerate not found")
    curve = [float(v) for v in m[1].split(",")]
    if len(curve) != 6 or curve[4:] != [1.0, 1.0]:
        raise ValueError(f"Theme.qml: decelerate {curve} is not one curve ending at (1, 1)")
    return scale, curve[:4]


def prompt_fade_out(text: str) -> float:
    """fadeOutMs: a sum of literals (the fade plus a margin)."""
    m = re.search(r"constexpr int fadeOutMs\s*=\s*([\d\s+]+);", text)
    if not m:
        raise ValueError("polkit/prompt/main.cpp: fadeOutMs not found")
    return float(sum(int(v) for v in m[1].split("+")))


def bar_hover(text: str) -> tuple[float, float]:
    """decoration.c's hover alphas: white in dark mode, black in light mode."""
    values = []
    for mode in ("DARK", "LIGHT"):
        m = re.search(rf"^#define\s+HOVER_{mode}_ALPHA\s+({NUMBER})f?\s*$", text, re.M)
        if not m:
            raise ValueError(f"decoration.c: HOVER_{mode}_ALPHA not found")
        values.append(float(m[1]))
    return values[0], values[1]


def subtle_hover(text: str) -> tuple[float, float]:
    """Theme.subtleHover: light ? black at a : white at a."""
    m = re.search(rf"property color subtleHover:\s*light\s*\?\s*Qt\.rgba\(0, 0, 0, ({NUMBER})\)"
                  rf"\s*:\s*Qt\.rgba\(1, 1, 1, ({NUMBER})\)", text)
    if not m:
        raise ValueError("Theme.qml: subtleHover not found (light ? black : white)")
    return float(m[2]), float(m[1])


def check(motion_h: str, theme_qml: str, prompt_cpp: str, decoration_c: str) -> list[str]:
    try:
        c_scale, c_curve = compositor_values(motion_h)
        q_scale, q_curve = qml_values(theme_qml)
        fade_out = prompt_fade_out(prompt_cpp)
        bar = bar_hover(decoration_c)
        subtle = subtle_hover(theme_qml)
    except ValueError as e:
        return [str(e)]
    failures = []
    for token in TOKENS:
        if c_scale[token] != q_scale[token]:
            failures.append(f"{token}: {c_scale[token]:g} ms in motion.h, {q_scale[token]:g} in Theme.qml")
    if c_curve != q_curve:
        failures.append(f"decelerate: {c_curve} in motion.h, {q_curve} in Theme.qml")
    if bar != subtle:
        failures.append(f"title bar hover {bar} (dark, light) in decoration.c, Theme.subtleHover {subtle}")
    if fade_out < q_scale["normal"]:
        failures.append(f"polkit prompt: fadeOutMs {fade_out:g} is shorter than Theme.normal "
                        f"({q_scale['normal']:g}): the process would exit during its fade")
    return failures


def main() -> int:
    failures = check(MOTION_H.read_text(), THEME_QML.read_text(), PROMPT_CPP.read_text(), DECORATION_C.read_text())
    for failure in failures:
        print(f"ERROR: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("Shared values agree: " + ", ".join(TOKENS) + ", decelerate, the polkit prompt's fade-out, "
          "the title bar's hover.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
