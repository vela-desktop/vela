#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shared CPU/GPU Cobertura collector.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 BUILD_DIRECTORY OUTPUT_DIRECTORY" >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$(cd "$1" && pwd)"
mkdir -p "$2"
out="$(cd "$2" && pwd)"

command -v gcovr >/dev/null || { echo "::error::Install gcovr to collect coverage" >&2; exit 1; }
command -v gcov >/dev/null || { echo "::error::Install GCC gcov to collect coverage" >&2; exit 1; }

# The repository contains sources, whereas instrumented .gcda files may
# live in $RUNNER_TEMP outside the repository on GPU runners. Pass $build
# explicitly: gcovr defaults its search path to --root, not the current dir.
# Relative --exclude filters are resolved against the *current directory*,
# not --root. Run from the source root so build-coverage/ and nested tests/
# are excluded correctly, even though .gcda files are searched under $build.
# Keep gcovr 8.7's internal-function workaround to avoid its KeyError.
cd "$root"
gcovr --root "$root" \
  --exclude '(.*/)?tests/.*' \
  --exclude '(.*/)?(build-coverage|vela-gpu-build)/.*' \
  --include-internal-functions \
  --xml-pretty --output "$out/coverage.xml" \
  "$build"

[[ -s "$out/coverage.xml" ]]
( cd "$out" && python3 "$root/scripts/coverage_summary.py" )
cat "$out/coverage-summary.md"
