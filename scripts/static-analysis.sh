#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Static analysis of one CI lane (docs/ci-lanes.md) over every first-party
# translation unit of a built tree: the Clang Static Analyzer through
# clang-tidy and cppcheck in both lanes; Clazy and qmllint in the Qt lane.
# Each count is then held to its ceiling by ci-findings.py.
#
#   scripts/static-analysis.sh native|qt BUILD_DIR OUT_DIR
#
# BUILD_DIR must be configured with Clang and CMAKE_EXPORT_COMPILE_COMMANDS
# and fully built, so that generated headers and moc output exist.
set -euo pipefail

lane=${1:?lane: native or qt}
build=$(realpath "${2:?build directory}")
out=${3:?output directory}
case $lane in native|qt) ;; *) echo "unknown lane: $lane" >&2; exit 2 ;; esac
root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${JOBS:-$(nproc)}
mkdir -p "$out"
out=$(realpath "$out")

python3 "$root/scripts/ci-lanes.py" check "$build" --sources
python3 "$root/scripts/ci-lanes.py" compdb "$build" --lane "$lane" -o "$out/compile_commands.json"
python3 - "$out" <<'PY'
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
files = sorted({Path(e["directory"], e["file"]).resolve().as_posix()
                for e in json.loads((out / "compile_commands.json").read_text())})
(out / "files-cpp.txt").write_text("".join(f + "\n" for f in files if not f.endswith(".c")))
PY

# The analyzer checkers scan-build enables by default, plus one bugprone check.
checks='-*,clang-analyzer-core.*,clang-analyzer-cplusplus.*,clang-analyzer-deadcode.*'
checks+=',clang-analyzer-nullability.*,clang-analyzer-unix.*'
checks+=',clang-analyzer-security.insecureAPI.UncheckedReturn,clang-analyzer-security.insecureAPI.getpw'
checks+=',clang-analyzer-security.insecureAPI.gets,clang-analyzer-security.insecureAPI.mkstemp'
checks+=',clang-analyzer-security.insecureAPI.mktemp,clang-analyzer-security.insecureAPI.vfork'
checks+=',bugprone-sizeof-expression'
# A QObject's parent deletes it, which the analyzer cannot see: in Qt code
# every parented `new` would be a leak. Runtime leaks are LeakSanitizer's.
[[ $lane == qt ]] && checks+=',-clang-analyzer-cplusplus.NewDeleteLeaks'
echo "clang-tidy: $(clang-tidy --version | grep -m1 version)"
# run-clang-tidy fails when clang-tidy fails on a file (a compile error or a
# crash), not on findings: those are counted below.
run-clang-tidy -quiet -j "$jobs" -p "$out" -checks="$checks" \
    -header-filter="^$root/" -exclude-header-filter="^$build/" \
    > "$out/clang-tidy.log" 2>&1 || { tail -50 "$out/clang-tidy.log"; exit 1; }

# cppcheck reads each file with the definitions and project include paths of
# its compile command, but not system headers: scripts/cppcheck/vela.cfg
# spells out the system macros that change control flow.
cppcheck_args=(--project="$out/compile_commands.json" --enable=warning --inline-suppr --quiet -j "$jobs"
    --library=googletest --library="$root/scripts/cppcheck/vela.cfg"
    --suppress=missingIncludeSystem --suppress=unusedFunction
    # Tests #undef NDEBUG, so their assert() always runs, side effects and
    # all; and a test that cannot allocate may as well crash.
    --suppress='assertWithSideEffect:*/tests/*' --suppress='assignmentInAssert:*/tests/*'
    --suppress='nullPointerOutOfMemory:*/tests/*'
    --template='{file}:{line}: {severity}: {message} [{id}]')
if [[ $lane == qt ]]; then
    # moc output stops at an #error unless it sees Qt's moc revision, which
    # lives in a system header cppcheck does not read.
    revision=$(sed -n 's/^#define Q_MOC_OUTPUT_REVISION \([0-9]*\).*/\1/p' \
        "$(qmake6 -query QT_INSTALL_HEADERS 2>/dev/null || echo /usr/include/qt6)"/QtCore/*.h | head -1)
    [[ -n $revision ]] || { echo "cannot find Q_MOC_OUTPUT_REVISION" >&2; exit 1; }
    cppcheck_args+=(--library=qt -DQ_MOC_OUTPUT_REVISION="$revision")
fi
cppcheck --version
cppcheck "${cppcheck_args[@]}" 2> "$out/cppcheck.log"

logs=(--clang-tidy "$out/clang-tidy.log" --cppcheck "$out/cppcheck.log")
if [[ $lane == qt ]]; then
    echo "clazy: $(clazy-standalone --version | grep -m1 version)"
    # connect-not-normalized is off: clang-format puts back the spaces in
    # SLOT(f(A, B)), and Qt normalizes the signature when it connects.
    # One log per file: parallel writers to one file would interleave lines.
    rm -rf "$out/clazy" && mkdir "$out/clazy"
    awk '{ print NR, $0 }' "$out/files-cpp.txt" | CLAZY_OUT=$out xargs -P "$jobs" -L 1 sh -c \
        'clazy-standalone -p "$CLAZY_OUT" --checks=level0,level1,no-connect-not-normalized "$1" > "$CLAZY_OUT/clazy/$0.log" 2>&1' \
        || { cat "$out"/clazy/*.log | tail -50; exit 1; }
    cat "$out"/clazy/*.log > "$out/clazy.log"
    logs+=(--clazy "$out/clazy.log")

    # qmllint over every QML module with the build's import paths. It exits
    # non-zero on warnings, which are counted like the others.
    status=0
    LC_ALL=C cmake --build "$build" --target all_qmllint > "$out/qmllint.log" 2>&1 || status=$?
    if grep -Eq "(unknown target|No rule to make target).*all_qmllint" "$out/qmllint.log"; then
        echo "qmllint did not run: the all_qmllint target is missing (exit status $status)" >&2
        exit 1
    fi
    logs+=(--qmllint "$out/qmllint.log")
fi

python3 "$root/scripts/ci-findings.py" "$lane" "${logs[@]}" --source "$root" --list "$out/findings.txt" ${UPDATE:+--update}
