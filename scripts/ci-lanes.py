#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Split a configured build's tests and translation units into CI lanes
(docs/ci-lanes.md).

A test is in the Qt lane when a program it runs from the build tree needs a
libQt6 library, directly or through a shared library of the build tree; in
the native lane when it runs build-tree programs and none needs Qt; in the
scripts lane when it runs none. Tests labelled gpu or rhi are in no CPU
lane. A first-party translation unit is Qt when it is compiled with Qt 6's
headers, native otherwise.

  ci-lanes.py tests BUILD --lane native|qt|scripts   test names, one per line
  ci-lanes.py compdb BUILD --lane native|qt -o FILE  a compile_commands.json
  ci-lanes.py check BUILD [--sources]                the invariants, and counts
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_LANES = ("native", "qt", "scripts")
SOURCE_LANES = ("native", "qt")
GPU_LABELS = {"gpu", "rhi"}
QT_LIBRARY = re.compile(r"^libQt6[A-Za-z0-9]*\.so")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}


def output_path(path: Path) -> Path:
    """A file to write, which must be inside the current directory or this
    checkout: an argument cannot send it anywhere else."""
    resolved = os.path.realpath(path)
    for base in (os.path.realpath(os.getcwd()), str(ROOT)):
        if os.path.commonpath([resolved, base]) == base:
            return Path(resolved)
    raise SystemExit(f"{path}: not inside the current directory or {ROOT}")


def needed(path: Path) -> list[str] | None:
    """DT_NEEDED of an ELF file, or None when the file is not ELF."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if data[:4] != b"\x7fELF" or len(data) < 64:
        return None
    wide = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if wide:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x3A)
        section = end + "IIQQQQIIQQ"
        dyn, dynsize = end + "qQ", 16
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)
        section = end + "IIIIIIIIII"
        dyn, dynsize = end + "iI", 8
    sections = []
    for i in range(shnum):
        offset = shoff + i * shentsize
        if offset + struct.calcsize(section) > len(data):
            return []
        _, kind, _, _, at, size, link, *_ = struct.unpack_from(section, data, offset)
        sections.append((kind, at, size, link))
    names = []
    for kind, at, size, link in sections:
        if kind != 6 or link >= len(sections):  # SHT_DYNAMIC
            continue
        strings_at = sections[link][1]
        for entry in range(at, min(at + size, len(data)) - dynsize + 1, dynsize):
            tag, value = struct.unpack_from(dyn, data, entry)
            if tag == 0:
                break
            if tag == 1:  # DT_NEEDED
                name_end = data.find(b"\0", strings_at + value)
                names.append(data[strings_at + value:name_end].decode(errors="replace"))
    return names


class Build:
    def __init__(self, build: Path):
        self.build = build.resolve()
        self.source = self._source_dir()
        self._libraries: dict[str, Path] | None = None
        self._qt: dict[Path, bool] = {}

    def _source_dir(self) -> Path:
        cache = self.build / "CMakeCache.txt"
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                return Path(line.split("=", 1)[1]).resolve()
        raise SystemExit(f"{cache}: no CMAKE_HOME_DIRECTORY")

    def inside_build(self, path: Path) -> bool:
        return path.is_relative_to(self.build)

    def libraries(self) -> dict[str, Path]:
        """Shared libraries of the build tree, by file name."""
        if self._libraries is None:
            self._libraries = {}
            for root, dirs, files in os.walk(self.build):
                dirs[:] = [d for d in dirs if d != "CMakeFiles" and not d.endswith("_autogen")]
                for name in files:
                    if ".so" in name:
                        self._libraries.setdefault(name, Path(root, name))
        return self._libraries

    def needs_qt(self, path: Path, seen: set[Path] | None = None) -> bool | None:
        """Whether a build-tree ELF file needs Qt; None when it is not ELF."""
        path = path.resolve()
        if path in self._qt:
            return self._qt[path]
        names = needed(path)
        if names is None:
            return None
        seen = seen or set()
        seen.add(path)
        result = False
        for name in names:
            if QT_LIBRARY.match(name):
                result = True
                break
            library = self.libraries().get(name)
            if library and library.resolve() not in seen and self.needs_qt(library, seen):
                result = True
                break
        self._qt[path] = result
        return result

    def tests(self) -> list[dict]:
        listing = subprocess.run(
            ["ctest", "--test-dir", str(self.build), "--show-only=json-v1"],
            check=True, capture_output=True, text=True).stdout
        return json.loads(listing)["tests"]

    def test_lane(self, test: dict) -> str:
        labels: set[str] = set()
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS":
                labels.update(prop.get("value") or [])
        if labels & GPU_LABELS:
            return "gpu"
        programs = []
        for argument in test.get("command") or []:
            path = Path(argument)
            if path.is_absolute() and path.is_file() and self.inside_build(path.resolve()):
                qt = self.needs_qt(path)
                if qt is not None:
                    programs.append(qt)
        if not programs:
            return "scripts"
        return "qt" if any(programs) else "native"

    def lanes(self) -> dict[str, list[str]]:
        lanes: dict[str, list[str]] = {lane: [] for lane in (*TEST_LANES, "gpu")}
        for test in self.tests():
            lanes[self.test_lane(test)].append(test["name"])
        return lanes

    def first_party(self, source: Path) -> bool:
        return (source.suffix in SOURCE_SUFFIXES and source.is_relative_to(self.source)
                and not self.inside_build(source)
                and not any(part.endswith("_autogen") for part in source.parts))

    def compile_commands(self) -> list[dict]:
        path = self.build / "compile_commands.json"
        if not path.is_file():
            raise SystemExit(f"{path} is missing: configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        return json.loads(path.read_text(encoding="utf-8"))

    def source_lanes(self) -> dict[str, list[dict]]:
        """First-party compile commands per lane, one per file and lane."""
        lanes: dict[str, list[dict]] = {lane: [] for lane in SOURCE_LANES}
        seen: set[tuple[str, Path]] = set()
        for entry in self.compile_commands():
            source = Path(entry["directory"], entry["file"]).resolve()
            if not self.first_party(source):
                continue
            command = entry.get("command") or " ".join(entry.get("arguments", []))
            lane = "qt" if re.search(r"/qt6/Qt[A-Z]|/QtCore\b|QtCore\.framework", command) else "native"
            if (lane, source) in seen:
                continue
            seen.add((lane, source))
            lanes[lane].append(entry)
        return lanes


def check(build: Build, with_sources: bool = False) -> list[str]:
    failures = []
    lanes = build.lanes()
    names = [name for lane in lanes.values() for name in lane]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        failures.append(f"tests in more than one lane: {', '.join(duplicates)}")
    for lane in TEST_LANES:
        if not lanes[lane]:
            failures.append(f"the {lane} test lane is empty")
    cpu = subprocess.run(
        ["ctest", "--test-dir", str(build.build), "--show-only=json-v1", "-LE", "|".join(sorted(GPU_LABELS))],
        check=True, capture_output=True, text=True).stdout
    expected = {test["name"] for test in json.loads(cpu)["tests"]}
    selected = {name for lane in TEST_LANES for name in lanes[lane]}
    for name in sorted(expected - selected):
        failures.append(f"CPU test in no CPU lane: {name}")
    for name in sorted(selected - expected):
        failures.append(f"test in a CPU lane that ctest does not select as CPU: {name}")
    counts = ", ".join(f"{lane} {len(lanes[lane])}" for lane in lanes)
    print(f"tests: {counts}")
    if with_sources:
        failures += check_sources(build)
    return failures


def tracked_sources(source: Path) -> list[Path]:
    listing = subprocess.run(["git", "-C", str(source), "ls-files", "-z", "--", "*.c", "*.cc", "*.cpp", "*.cxx"],
                             check=True, capture_output=True, text=True).stdout
    return [source / name for name in listing.split("\0") if name]


def check_sources(build: Build) -> list[str]:
    """Every tracked C and C++ file is compiled in this build, so the
    analysers see it."""
    failures = []
    sources = build.source_lanes()
    for lane in SOURCE_LANES:
        if not sources[lane]:
            failures.append(f"the {lane} source lane is empty")
    compiled = {Path(entry["directory"], entry["file"]).resolve()
                for entries in sources.values() for entry in entries}
    for path in tracked_sources(build.source):
        if path.resolve() not in compiled:
            failures.append(f"not compiled in this build, so not analysed: {path.relative_to(build.source)}")
    files = ", ".join(f"{lane} {len(sources[lane])}" for lane in SOURCE_LANES)
    print(f"translation units: {files}")
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    tests = commands.add_parser("tests", help="test names of one lane")
    tests.add_argument("build", type=Path)
    tests.add_argument("--lane", choices=TEST_LANES, required=True)
    tests.add_argument("-o", "--output", type=Path)
    compdb = commands.add_parser("compdb", help="compile_commands.json of one lane")
    compdb.add_argument("build", type=Path)
    compdb.add_argument("--lane", choices=SOURCE_LANES, required=True)
    compdb.add_argument("-o", "--output", type=Path, required=True)
    checks = commands.add_parser("check", help="every test in exactly one lane")
    checks.add_argument("build", type=Path)
    checks.add_argument("--sources", action="store_true",
                        help="also check that every tracked C and C++ file is in compile_commands.json")
    args = parser.parse_args(argv)

    build = Build(args.build)
    if args.command == "tests":
        names = build.lanes()[args.lane]
        if not names:
            print(f"the {args.lane} test lane is empty", file=sys.stderr)
            return 1
        text = "".join(f"{name}\n" for name in names)
        if args.output:
            output_path(args.output).write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0
    if args.command == "compdb":
        entries = build.source_lanes()[args.lane]
        if not entries:
            print(f"the {args.lane} source lane is empty", file=sys.stderr)
            return 1
        output = output_path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(entries, indent=1) + "\n", encoding="utf-8")
        print(f"{len(entries)} {args.lane} translation units in {args.output}")
        return 0
    failures = check(build, args.sources)
    for failure in failures:
        print(f"error: {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
