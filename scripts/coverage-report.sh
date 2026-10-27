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

# Run inside the instrumented build tree where gcovr can find the .gcda
# files; filter source paths, not the object files needed to calculate hits.
# Keep gcovr 8.7's internal-function workaround to avoid its KeyError.
cd "$build"
gcovr --root "$root" \
  --exclude '(^|/)tests/' \
  --exclude '(^|/)build-coverage/' \
  --exclude '(^|/)vela-gpu-build/' \
  --include-internal-functions \
  --xml-pretty --output "$out/coverage.xml"

test -s "$out/coverage.xml"
python3 "$root/scripts/coverage_summary.py" "$out/coverage.xml" \
  "$out/coverage-summary.md" "$out/coverage-summary.json"
cat "$out/coverage-summary.md"
