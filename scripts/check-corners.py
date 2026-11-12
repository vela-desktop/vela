#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Corners inside corners (docs/visual-coherence.md §6.12): lists every shape
that sits in the corner of a rounder one without following it.

It reads what Vela's Qt programs write when run with
VELA_ROUNDED_DUMP=<directory> (common/src/controls.cpp): every shown item
with corners in every window, where it is and its radii, twice a second, as
<directory>/<program>-<pid>.json. Open the surfaces to check, copy the files
(one directory a moment, if you want them told apart) and run

  scripts/check-corners.py <directory> [--pills]

A shape is reported when its corner is within the outer shape's radius of
that shape's corner and its own radius is not the outer one less the gap
(between the two gaps when they differ, one unit of tolerance). Also: a
Shadow whose corners are not its target's, and two shapes in the same box
with different corners. Pictures are shapes with square corners, unless a
RoundClip cuts them. Full pills and circles are shapes of their own and are
not reported (--pills lists them too). Exits 1 when something is reported.
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

TOLERANCE = 1.01
# Corner: its name, its index in an item's radii, and whether it is at the
# right and at the bottom.
CORNERS = (("top left", 0, False, False), ("top right", 1, True, False),
           ("bottom left", 2, False, True), ("bottom right", 3, True, True))
# Pictures that are drawings on a clear ground, not rectangles.
GLYPHS = ("Icon.qml", "FileIcon.qml", "StartIcon.qml")


def radii(item):
    """The corners as drawn: a Rectangle caps them at half its shorter side."""
    cap = min(item["w"], item["h"]) / 2
    return [min(max(radius, 0), cap) for radius in item["radii"]]


def drawn(item):
    return item["opacity"] > 0.02 and (item["alpha"] > 0.02 or (item["border"] > 0 and item["borderAlpha"] > 0.02))


def inside(inner, outer, slack=0.5):
    return (inner["x"] >= outer["x"] - slack and inner["y"] >= outer["y"] - slack
            and inner["x"] + inner["w"] <= outer["x"] + outer["w"] + slack
            and inner["y"] + inner["h"] <= outer["y"] + outer["h"] + slack)


def name(item):
    return f'{item["file"]}#{item["id"] or item["name"] or item["type"]}'


def clipped(items):
    """Pictures cut to the RoundClip that holds them: its box and, where a
    corner of the picture is the clip's, its radius. Without the clips."""
    clips = [item for item in items if item["type"].startswith("RoundClip")]
    rest = [item for item in items if not item["type"].startswith("RoundClip") and item["file"] != "RoundClip.qml"]
    for picture in rest:
        if not picture.get("picture"):
            continue
        for clip in clips:
            left, top = max(picture["x"], clip["x"]), max(picture["y"], clip["y"])
            right = min(picture["x"] + picture["w"], clip["x"] + clip["w"])
            bottom = min(picture["y"] + picture["h"], clip["y"] + clip["h"])
            if right - left < 4 or bottom - top < 4:
                continue
            clip_radii = radii(clip)
            cut = list(picture["radii"])
            for _, index, at_right, at_bottom in CORNERS:
                x, y = (right if at_right else left), (bottom if at_bottom else top)
                clip_x = clip["x"] + clip["w"] if at_right else clip["x"]
                clip_y = clip["y"] + clip["h"] if at_bottom else clip["y"]
                if abs(x - clip_x) < 0.6 and abs(y - clip_y) < 0.6:
                    cut[index] = clip_radii[index]
            picture.update(x=left, y=top, w=right - left, h=bottom - top, radii=cut)
    return rest


def findings(items, pills=False):
    """(kind, outer, inner, used by, outer radius, inner radius, gaps, corner) for one window."""
    out = []
    items = [dict(item) for item in items]
    items = [item for item in items if item["w"] >= 4 and item["h"] >= 4 and item["opacity"] > 0.02
             and item["file"] not in ("FocusRing.qml", "") and "ShaderEffect" not in item["type"]]
    items = clipped(items)
    shadows = [item for item in items if item["file"] == "Shadow.qml"]
    items = [item for item in items if item["file"] != "Shadow.qml" and not item["type"].startswith("Shadow")]
    for shadow in shadows:
        for target in items:
            if (drawn(target) and inside(shadow, target) and inside(target, shadow)
                    and max(abs(a - b) for a, b in zip(radii(shadow), radii(target))) > TOLERANCE):
                out.append(("shadow", name(target), "its Shadow", shadow["owner"],
                            tuple(round(r) for r in radii(target)), tuple(round(r) for r in radii(shadow)), (0, 0), "all"))
    containers = [item for item in items if drawn(item) and max(radii(item)) >= 2 and not item.get("picture")]
    order = {id(item): index for index, item in enumerate(items)}
    for inner in items:
        if inner.get("picture") and (min(inner["w"], inner["h"]) < 40 or inner["file"] in GLYPHS):
            continue
        inner_radii = radii(inner)
        for same in containers:
            # Once a pair: against the one drawn first.
            if order[id(same)] < order[id(inner)] and inside(inner, same) and inside(same, inner):
                outer_radii = radii(same)
                if any(abs(outer_radii[index] - inner_radii[index]) > TOLERANCE for _, index, _, _ in CORNERS):
                    out.append(("same box", name(same), name(inner), inner["owner"],
                                tuple(round(r) for r in outer_radii), tuple(round(r) for r in inner_radii), (0, 0), "all"))
        around = [outer for outer in containers if outer is not inner and inside(inner, outer)
                  and not (abs(outer["w"] - inner["w"]) < 1 and abs(outer["h"] - inner["h"]) < 1)]
        if not around:
            continue
        # The smallest drawn round shape around it.
        outer = min(around, key=lambda item: item["w"] * item["h"])
        outer_radii = radii(outer)
        pill = min(inner["w"], inner["h"]) / 2
        for corner, index, at_right, at_bottom in CORNERS:
            gap_x = (outer["x"] + outer["w"] - inner["x"] - inner["w"]) if at_right else inner["x"] - outer["x"]
            gap_y = (outer["y"] + outer["h"] - inner["y"] - inner["h"]) if at_bottom else inner["y"] - outer["y"]
            radius = outer_radii[index]
            if radius < 2 or gap_x >= radius or gap_y >= radius:
                continue # out of this corner's reach
            own = inner_radii[index]
            if radius - max(gap_x, gap_y) - TOLERANCE <= own <= radius - min(gap_x, gap_y) + TOLERANCE:
                continue
            if own < 0.5 and not drawn(inner):
                continue # draws nothing and has no shape
            if abs(own - pill) < 0.6 and not pills:
                continue
            out.append(("nested", name(outer), name(inner), inner["owner"], round(radius, 1), round(own, 1),
                        (round(gap_x, 1), round(gap_y, 1)), corner))
    return out


def check(directory, pills=False):
    """{finding without its corner: the moments (directories) it was seen in}."""
    seen = defaultdict(set)
    for path in sorted(Path(directory).rglob("*.json")):
        moment = str(path.parent.relative_to(directory))
        for window in json.loads(path.read_text()):
            for finding in findings(window["items"], pills):
                seen[finding[:7]].add(moment)
    return seen


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--pills", action="store_true", help="report full pills and circles too")
    args = parser.parse_args()
    seen = check(args.directory, args.pills)
    for (kind, outer, inner, owner, outer_radius, radius, gaps), moments in sorted(seen.items(), key=lambda kv: kv[0][1:3]):
        where = f" (used by {owner})" if owner else ""
        print(f"{kind}: {inner}{where} in {outer}: radius {radius}, the outer one {outer_radius}, gaps {gaps}"
              f" [{len(moments)}: {sorted(moments)[0]}]")
    print(f"{len(seen)} corner{'s' if len(seen) != 1 else ''} out of place")
    return 1 if seen else 0


if __name__ == "__main__":
    sys.exit(main())
