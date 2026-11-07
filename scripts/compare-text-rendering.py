#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Capture three text policies in fresh processes and build a local comparison."""

import argparse
import html
import os
from pathlib import Path
import subprocess


MODES = {"native": "Previous native renderer", "qt-default": "Qt rendering, default AA",
         "qt-gray": "Qt rendering, grayscale AA"}
SCALES = (1, 1.25, 1.5, 1.75, 2)
SIZES = (12, 14, 18)
STATES = ("rest", "pressed", "scrolled", "transparent", "tinted", "light-rest", "light-pressed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="path to vela-text-rendering-test")
    parser.add_argument("--output", type=Path, default=Path("/tmp/vela-text-comparison"))
    parser.add_argument("--font-family", help="font used in the Vela session; resolved family is logged")
    args = parser.parse_args()
    binary = args.binary.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Never leave an old successful report after a failed recapture.
    report = output / "index.html"
    report.unlink(missing_ok=True)
    environment = dict(os.environ)
    environment.setdefault("QT_QPA_PLATFORMTHEME", "generic")
    diagnostics = []
    for mode in MODES:
        target = output / mode
        target.mkdir(exist_ok=True)
        command = [str(binary), "--comparison-mode", mode, "--capture-dir", str(target)]
        if args.font_family:
            command.extend(["--font-family", args.font_family])
        result = subprocess.run(command, env=environment, text=True, capture_output=True, check=False)
        log = result.stdout + result.stderr
        (target / "run.log").write_text(log, encoding="utf-8")
        if result.returncode:
            parser.exit(result.returncode if result.returncode > 0 else 1,
                        f"{mode}: capture failed (exit {result.returncode}); see {target / 'run.log'}\n")
        expected = [target / f"{scale:g}-{size}-{state}.png"
                    for scale in SCALES for size in SIZES for state in STATES]
        if any(not path.is_file() for path in expected):
            parser.exit(1, f"{mode}: incomplete capture; see {target / 'run.log'}\n")
        diagnostics.append(log.splitlines()[0] if log else mode)

    parts = ["""<!doctype html><html lang="en"><meta charset="utf-8">
<title>Vela text rendering comparison</title>
<style>
body { font: 16px system-ui; margin: 24px; color: #222; background: #fafafa }
table { border-collapse: collapse } th, td { padding: 8px; border: 1px solid #bbb }
img { display: block; max-width: none; background: #888 }
details { margin: 16px 0; overflow-x: auto } summary { cursor: pointer }
pre { white-space: pre-wrap } th { text-align: left }
</style><h1>Vela text rendering comparison</h1>
<p>View at 100% browser zoom on an output at 100% scale for one image pixel per
physical pixel. All three columns use the same font, size, pose and background.
Each policy runs in a fresh process. Expand a size/scale to compare its frames.</p>
<p>These captures support visual review; they do not score legibility or measure
session frame pacing or GPU memory. The native column reproduces the previous
renderer. OpenGL ES may already choose grayscale for the default-AA column.</p>
<pre>""", html.escape("\n".join(diagnostics)), "</pre>"]
    for scale in SCALES:
        for size in SIZES:
            parts.append(f"<details><summary>{scale * 100:g}% scale, {size}px text</summary><table>")
            parts.append("<thead><tr><th>State</th>" + "".join(
                f"<th>{html.escape(label)}</th>" for label in MODES.values()) + "</tr></thead><tbody>")
            for state in STATES:
                parts.append(f"<tr><th>{state}</th>")
                for mode in MODES:
                    path = f"{mode}/{scale:g}-{size}-{state}.png"
                    parts.append(f'<td><img src="{path}" alt="{mode}, {state}" loading="lazy" '
                                 f'width="{round(320 * scale)}" height="{round(112 * scale)}"></td>')
                parts.append("</tr>")
            parts.append("</tbody></table></details>")
    parts.append("</html>")
    report.write_text("\n".join(parts), encoding="utf-8")
    print(f"Comparison captured: {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
