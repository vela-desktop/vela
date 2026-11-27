#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Count static-analysis findings per check and hold each count to its
ceiling (docs/ci-lanes.md).

  ci-findings.py LANE --clang-tidy LOG --cppcheck LOG [--clazy LOG] [--qmllint LOG]

A count above its ceiling fails: the findings of that check are printed.
--list writes every finding to standard output; the summary and the
failures go to standard error.
A count below its ceiling fails too, so that the ceiling comes down with the
fix; --update writes the new counts. Checks without a ceiling have a
ceiling of 0. Only findings in first-party files count: the source tree,
outside build directories.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CEILINGS_NAME = "scripts/ci-findings-ceilings.json"
CEILINGS = ROOT / CEILINGS_NAME
LANES = ("native", "qt")
TOOLS = ("clang-tidy", "cppcheck", "clazy", "qmllint")

# Each pattern matches the head of a line; the trailing " [check]" is split
# off the rest with rpartition, which keeps the matching linear.
# path:line:col: warning: message [check] (clang-tidy, clazy)
CLANG = re.compile(r"(?P<file>/[^:]+):(?P<line>\d+):\d+: (?:warning|error): (?P<rest>.*)")
# path:line: severity: message [id] (cppcheck with the template CI uses)
CPPCHECK = re.compile(r"(?P<file>[^:]+):(?P<line>\d+): \w+: (?P<rest>.*)")
# Warning: path:line:col: message [category] (qmllint)
QMLLINT = re.compile(r"Warning: (?P<file>/[^:]+):(?P<line>\d+):\d+: (?P<rest>.*)")
# What a check name may hold, per tool.
CHECK_NAME = {"clang-tidy": re.compile(r"[^\]]+"), "clazy": re.compile(r"[^\]]+"),
              "cppcheck": re.compile(r"\w+"), "qmllint": re.compile(r"[a-z.-]+")}
# cppcheck messages that mean a file was not analysed at all. They count
# wherever they point, generated files included.
CPPCHECK_FAILURES = {"syntaxError", "preprocessorErrorDirective", "unknownMacro", "internalAstError",
                     "internalError", "cppcheckError", "premium-internalError"}


@dataclass(frozen=True, order=True)
class Finding:
    tool: str
    check: str
    file: str
    line: int
    message: str

    def __str__(self) -> str:
        return f"{self.file}:{self.line}: {self.message} [{self.check}]"


def first_party(path: str, source: Path) -> str | None:
    resolved = (source / path).resolve() if not os.path.isabs(path) else Path(path).resolve()
    if not resolved.is_relative_to(source):
        return None
    relative = resolved.relative_to(source)
    if relative.parts and (relative.parts[0] == "build" or relative.parts[0].startswith("build-")):
        return None
    if any(part.endswith("_autogen") for part in relative.parts):
        return None
    return relative.as_posix()


def parse(tool: str, text: str, source: Path) -> set[Finding]:
    pattern = {"clang-tidy": CLANG, "clazy": CLANG, "cppcheck": CPPCHECK, "qmllint": QMLLINT}[tool]
    findings = set()
    for line in text.splitlines():
        match = pattern.fullmatch(line.strip())
        if not match:
            continue
        message, bracket, tail = match["rest"].rpartition(" [")
        checks = tail.removesuffix("]")
        if not bracket or checks == tail or not CHECK_NAME[tool].fullmatch(checks):
            continue
        file = first_party(match["file"], source)
        if file is None and tool == "cppcheck" and checks in CPPCHECK_FAILURES:
            file = match["file"]
        if file is None:
            continue
        for check in checks.split(","):
            if tool == "clazy":
                if not check.startswith("-Wclazy-"):
                    continue  # compiler warnings are the compilers' business
                check = check.removeprefix("-Wclazy-")
            findings.add(Finding(tool, check, file, int(match["line"]), message))
    return findings


def compare(lane: str, findings: dict[str, set[Finding]], ceilings: dict) -> tuple[list[str], list[str]]:
    """(failures, summary lines)."""
    failures, rows = [], []
    for tool, found in findings.items():
        limits = ceilings.get(lane, {}).get(tool, {})
        counts = Counter(finding.check for finding in found)
        for check in sorted(set(counts) | set(limits)):
            count, ceiling = counts.get(check, 0), limits.get(check, 0)
            if count == ceiling == 0:
                continue
            state = "ok"
            if count > ceiling:
                state = "over"
                listed = "\n".join(f"  {finding}" for finding in sorted(found) if finding.check == check)
                failures.append(f"{lane} {tool} {check}: {count} findings, ceiling {ceiling}\n{listed}")
            elif count < ceiling:
                state = "lower"
                failures.append(f"{lane} {tool} {check}: {count} findings, below the ceiling of {ceiling}; "
                                f"lower it in {CEILINGS_NAME} (or run with --update)")
            rows.append(f"| {tool} | `{check}` | {count} | {ceiling} | {state} |")
    return failures, rows


def update(lane: str, findings: dict[str, set[Finding]], ceilings: dict) -> dict:
    for tool, found in findings.items():
        counts = Counter(finding.check for finding in found)
        if counts:
            ceilings.setdefault(lane, {})[tool] = dict(sorted(counts.items()))
        else:
            ceilings.get(lane, {}).pop(tool, None)
    for lane_name in list(ceilings):
        if not ceilings[lane_name]:
            del ceilings[lane_name]
    return ceilings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("lane", choices=LANES)
    for tool in TOOLS:
        parser.add_argument(f"--{tool}", type=Path, action="append", default=[], metavar="LOG")
    parser.add_argument("--source", type=Path, default=ROOT, help="source tree the logs refer to")
    parser.add_argument("--list", action="store_true", help="write every finding to standard output")
    parser.add_argument("--update", action="store_true", help="set the ceilings to the current counts")
    args = parser.parse_args(argv)

    source = args.source.resolve()
    findings: dict[str, set[Finding]] = {}
    for tool in TOOLS:
        logs = getattr(args, tool.replace("-", "_"))
        if logs:
            findings[tool] = set().union(*(parse(tool, log.read_text(errors="replace"), source) for log in logs))
    if not findings:
        parser.error("give at least one log")

    ceilings = json.loads(CEILINGS.read_text(encoding="utf-8")) if CEILINGS.exists() else {}
    if args.list:
        sys.stdout.write("".join(f"{finding.tool}: {finding}\n" for tool in findings
                                 for finding in sorted(findings[tool])))
    if args.update:
        ceilings = update(args.lane, findings, ceilings)
        CEILINGS.write_text(json.dumps(ceilings, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"updated {CEILINGS_NAME}", file=sys.stderr)
        return 0

    failures, rows = compare(args.lane, findings, ceilings)
    total = sum(len(found) for found in findings.values())
    summary = [f"### Static analysis, {args.lane} lane", "",
               f"{total} findings in first-party files.", ""]
    if rows:
        summary += ["| Tool | Check | Findings | Ceiling | |", "|---|---|---:|---:|---|", *rows]
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as out:
            out.write("\n".join(summary) + "\n")
    print("\n".join(summary), file=sys.stderr)
    for failure in failures:
        print(f"\nerror: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
