#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

if (( $# == 0 )); then
    echo "Usage: bash scripts/format-changed.sh <target-ref> [--diff]" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"
base=$(git merge-base -- HEAD "$1")
shift

# Exclude these paths before running the formatter. LLVM 18 can return empty
# output for an ignored file, which git-clang-format treats as formatted code.
exclusions=$(sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
    -e '/^#/d' -e '/^$/d' -e 's/^/:(top,exclude,glob)/' .clang-format-ignore)
paths=('*.c' '*.h' '*.cpp' '*.hpp')
if [[ -n $exclusions ]]; then
    mapfile -t ignored <<< "$exclusions"
    paths+=("${ignored[@]}")
fi

formatter=(git clang-format)
if command -v git-clang-format-18 >/dev/null && command -v clang-format-18 >/dev/null; then
    formatter=(git clang-format-18 --binary clang-format-18)
fi

exec "${formatter[@]}" "$@" "$base" -- "${paths[@]}"
