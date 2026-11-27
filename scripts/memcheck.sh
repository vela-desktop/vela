#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One CPU lane's tests (docs/ci-lanes.md) under Valgrind Memcheck, from a
# configured, built tree: every test of the lane whose command is a program
# of the build tree. A test started through dbus-run-session or Python would
# put the wrapper, not Vela, under Memcheck; those wait for a harness of
# their own. Fails when a test fails or Memcheck reports a defect.
#
#   memcheck.sh native|qt BUILD
#
# Memcheck's logs stay in BUILD/Testing/Temporary/MemoryChecker.*.log.
set -euo pipefail

if [[ $# -ne 2 || ( $1 != native && $1 != qt ) ]]; then
    echo "usage: $0 native|qt BUILD" >&2
    exit 2
fi
lane=$1
build=$(realpath "$2")
here=$(dirname "$(realpath "$0")")

regex=$(python3 "$here/ci-lanes.py" tests "$build" --lane "$lane" |
    python3 -c '
import json, re, subprocess, sys
from pathlib import Path

build, programs = Path(sys.argv[1]), Path(sys.argv[2])
lane = set(sys.stdin.read().split())
listing = json.loads(subprocess.check_output(
    ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
names, paths = [], set()
for test in listing["tests"]:
    name, command = test["name"], test.get("command") or []
    if name not in lane or not command:
        continue
    program = Path(command[0]).resolve()
    if program.is_relative_to(build) and program.is_file():
        with program.open("rb") as binary:
            if binary.read(4) == b"\x7fELF":
                names.append(name)
                paths.add(str(program))
                continue
    print(f"not under Memcheck, started through {command[0]}: {name}", file=sys.stderr)
if not names:
    sys.exit("no test of the lane runs a program of the build tree")
print(f"{len(names)} tests under Memcheck", file=sys.stderr)
programs.write_text("".join(f"{path}\n" for path in sorted(paths)))
print("^(" + "|".join(re.escape(name) for name in names) + ")$")
' "$build" "$build/memcheck-$lane.programs")

# Memcheck reads each library's debug information through debuginfod, and
# every test process would download the same files at once: fetch them first.
if [[ -n ${DEBUGINFOD_URLS:-} ]]; then
    xargs -r ldd < "$build/memcheck-$lane.programs" | awk '$3 ~ /^\// { print $3 }' | sort -u |
        xargs -r -P 8 -n 1 debuginfod-find debuginfo > /dev/null 2>&1 || true
fi

# Code generated at run time has no symbols, and PCRE2's reads whole vectors
# past the end of a string: QRegularExpression and QML interpret instead.
export QT_ENABLE_REGEXP_JIT=0 QV4_FORCE_INTERPRETER=1

script="$build/memcheck-$lane.cmake"
cat > "$script" <<CMAKE
set(CTEST_SOURCE_DIRECTORY "$(dirname "$here")")
set(CTEST_BINARY_DIRECTORY "$build")
set(CTEST_MEMORYCHECK_TYPE Valgrind)
set(CTEST_MEMORYCHECK_COMMAND "$(command -v valgrind)")
set(CTEST_MEMORYCHECK_SUPPRESSIONS_FILE "$here/valgrind.supp")
set(CTEST_MEMORYCHECK_COMMAND_OPTIONS "--tool=memcheck --leak-check=full --show-leak-kinds=definite,possible --errors-for-leak-kinds=definite,possible --track-origins=yes --num-callers=40 --error-exitcode=99")
# Tests without a TIMEOUT of their own; the declared ones are scaled at
# configure time (VELA_TEST_TIMEOUT_SCALE).
set(CTEST_TEST_TIMEOUT 900)
ctest_start(Experimental QUIET)
ctest_memcheck(INCLUDE "\$ENV{VELA_MEMCHECK_REGEX}" PARALLEL_LEVEL $(nproc)
    RETURN_VALUE result DEFECT_COUNT defects)
message(STATUS "Memcheck ($lane): test result \${result}, defects \${defects}")
if(NOT result EQUAL 0 OR NOT defects EQUAL 0)
    message(FATAL_ERROR "Tests failed or Memcheck reported defects")
endif()
CMAKE
VELA_MEMCHECK_REGEX=$regex ctest -S "$script" -V
