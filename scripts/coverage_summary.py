#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize C/C++ Cobertura line hits per Vela subsystem.

Each line is counted once even if GCC emits duplicate C++ instantiations.
Only line hits are considered; this is not Codecov's branch/partial coverage.
"""

import json
from pathlib import Path, PurePosixPath
import sys
import xml.etree.ElementTree as ET


def read_coverage(root):
    files = {}
    for cls in root.iter("class"):
        name = (cls.get("filename") or "").replace("\\", "/")
        path = PurePosixPath(name)
        if not name or path.is_absolute() or ".." in path.parts:
            raise ValueError(f"Invalid source path: {name!r}")
        if ("tests" in path.parts or "vela-gpu-build" in path.parts
                or any(part.startswith("build-") for part in path.parts)):
            raise ValueError(f"Excluded test or build source in report: {name}")
        if len(path.parts) < 2:
            continue
        lines = files.setdefault(name, {})
        for item in cls.findall("./lines/line"):
            line, hits = int(item.attrib["number"]), int(item.attrib["hits"])
            if line < 1 or hits < 0:
                raise ValueError(f"Invalid line or hit count in {name}:{line}")
            lines[line] = max(lines.get(line, 0), hits)
    if not any(files.values()):
        raise ValueError("No instrumented first-party source lines in Cobertura report")

    groups = {}
    for name, lines in files.items():
        area = name.split("/", 1)[0]
        entry = groups.setdefault(area, {"files": 0, "tracked": 0, "covered": 0})
        entry["files"] += 1
        entry["tracked"] += len(lines)
        entry["covered"] += sum(hits > 0 for hits in lines.values())
    total = {
        "files": sum(group["files"] for group in groups.values()),
        "tracked": sum(group["tracked"] for group in groups.values()),
        "covered": sum(group["covered"] for group in groups.values()),
    }
    for data in [*groups.values(), total]:
        data["percent"] = round(100 * data["covered"] / data["tracked"], 2) if data["tracked"] else 0
    return {"total": total, "subsystems": dict(sorted(groups.items()))}


def to_markdown(report):
    total = report["total"]
    result = [
        "## Vela first-party C/C++ line coverage",
        "",
        f"**{total['covered']:,} / {total['tracked']:,} executed lines "
        f"({total['percent']:.2f}%)** across {total['files']} files.",
        "",
        "| Subsystem | Covered / Tracked | Line coverage |",
        "| --- | ---: | ---: |",
    ]
    for name, item in report["subsystems"].items():
        result.append(f"| {name} | {item['covered']:,} / {item['tracked']:,} | {item['percent']:.2f}% |")
    result += [
        "",
        "_GCC-instrumented C/C++ only; test sources and generated code excluded. "
        "Hardware coverage exists only for an instrumented real-GPU run. "
        "Codecov may present a different percentage for partially covered branches._",
        "",
    ]
    return "\n".join(result)


def main(argv):
    if len(argv) != 3:
        raise SystemExit("Usage: coverage_summary.py REPORT.xml SUMMARY.md SUMMARY.json")
    report = read_coverage(ET.parse(argv[0]).getroot())
    Path(argv[1]).write_text(to_markdown(report), encoding="utf-8")
    Path(argv[2]).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (ET.ParseError, ValueError) as err:
        print(f"::error::Coverage summary failed: {err}", file=sys.stderr)
        raise SystemExit(1) from err
