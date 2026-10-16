#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Motion stays reachable and state colors fade (docs/visual-coherence.md
§6.11), in the QML of the shell, Settings, Files, the polkit prompt and
Vela.Controls:

1. A `duration:` takes a Theme token or an expression of them, never a literal
   other than 0: otherwise the duration scale and the animation effects
   switch (§6.5) can't reach it.
2. A `color` or `border.color` driven by a state (containsMouse, pressed,
   hovered, down, checked, selected) belongs to a Fill, or to an object with
   its own `Behavior on` that property; on a plain Rectangle it changes at
   once and is reported. So is an `opacity` driven by a state on a Rectangle
   or a Fill (a hover shown by turning a fixed color on): the state belongs in
   the color, which fades.

A reported line passes with a comment on it, or on the line above, saying why:
`// motion: follows the drag`. Any other report fails."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[1]
DIRECTORIES = ("shell/qml", "settings/qml", "explorer/qml", "polkit/prompt/qml", "common/controls")
STATES = re.compile(r"\b(containsMouse|pressed|hovered|down|checked|selected)\b")
EXEMPT = re.compile(r"//\s*motion:")
OPENING = re.compile(r"(?:^|[\s:;{])([A-Z][\w.]*)\s*\{")
DURATION = re.compile(r"(?<![\w.])duration:\s*(.+?)\s*(?:;|$|\}|//)")
DECLARATION = re.compile(r"^\s*(?:readonly\s+|required\s+|default\s+)*property\s+\S+\s+\w+\s*:")
COLOR = re.compile(r"(?:^|[\s;{])((?:border\.)?color|opacity):\s*(.+?)\s*(?:;\s*\w|$|//)")
NUMBER = re.compile(r"(?<![\w.])(\d+(?:\.\d+)?)(?![\w.])")


class Report(NamedTuple):
    path: str
    line: int
    rule: str
    text: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.text}"


def strip_strings(line: str) -> str:
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)


def objects(lines: list[str]) -> list[tuple[str, int, int]]:
    """Each object's type and its first and last line (0-based)."""
    found = []
    stack: list[tuple[str, int]] = []
    for i, raw in enumerate(lines):
        line = strip_strings(raw.split("//")[0])
        pos = 0
        while pos < len(line):
            c = line[pos]
            if c == "{":
                m = None
                for m in OPENING.finditer(line[: pos + 1]):
                    pass
                kind = m[1] if m and m.end() == pos + 1 else ""
                stack.append((kind, i))
            elif c == "}" and stack:
                kind, start = stack.pop()
                if kind:
                    found.append((kind, start, i))
            pos += 1
    return found


def innermost(found: list[tuple[str, int, int]], line: int) -> tuple[str, int, int] | None:
    best = None
    for kind, start, end in found:
        if start <= line <= end and (best is None or start >= best[1]):
            best = (kind, start, end)
    return best


def literal_duration(expression: str) -> bool:
    if "Theme." in expression:
        return False
    return any(float(n) != 0 for n in NUMBER.findall(expression))


def expression_at(lines: list[str], i: int, first: str) -> str:
    """The expression starting on line i, with its continuation lines."""
    text = first
    j = i + 1
    while j < len(lines):
        previous = lines[j - 1].split("//")[0].rstrip()
        current = lines[j].strip()
        if previous.endswith(("?", ":", "||", "&&", "(", ",")) or current.startswith(("?", ":", "||", "&&", ")")):
            text += " " + current.split("//")[0]
            j += 1
        else:
            break
    return text


def check_file(path: Path, relative: str) -> list[Report]:
    lines = path.read_text().splitlines()
    found = objects(lines)
    reports = []
    for i, raw in enumerate(lines):
        exempt = EXEMPT.search(raw) or (i > 0 and EXEMPT.search(lines[i - 1]))
        if exempt:
            continue
        code = strip_strings(raw.split("//")[0])
        # A declared property named `duration` is not an animation's.
        for m in DURATION.finditer(DECLARATION.sub("", code)):
            if literal_duration(m[1]):
                reports.append(Report(relative, i + 1, "duration is not a Theme token", raw.strip()))
        for m in COLOR.finditer(code):
            prop, expression = m[1], expression_at(lines, i, m[2])
            if not STATES.search(strip_strings(expression)):
                continue
            owner = innermost(found, i)
            kinds = ("Rectangle", "Fill") if prop == "opacity" else ("Rectangle",)
            if owner is None or owner[0] not in kinds:
                continue
            block = "\n".join(lines[owner[1] : owner[2] + 1])
            if re.search(rf"Behavior on {re.escape(prop)}\b", block):
                continue
            rule = "state opacity on a background, not a faded color" if prop == "opacity" \
                else f"state {prop} on a Rectangle, not a Fill"
            reports.append(Report(relative, i + 1, rule, raw.strip()))
    return reports


def check(root: Path = ROOT) -> list[Report]:
    reports = []
    for directory in DIRECTORIES:
        for path in sorted((root / directory).rglob("*.qml")):
            reports += check_file(path, str(path.relative_to(root)))
    return reports


def main(root: Path = ROOT) -> int:
    reports = check(root)
    for report in reports:
        print(report)
    rules: dict[str, int] = {}
    for report in reports:
        rules[report.rule] = rules.get(report.rule, 0) + 1
    summary = ", ".join(f"{count} {rule}" for rule, count in sorted(rules.items())) or "nothing to report"
    print(f"QML motion: {summary}.")
    return 1 if reports else 0


if __name__ == "__main__":
    argparse.ArgumentParser(description=__doc__.split("\n\n")[0]).parse_args()
    sys.exit(main())
