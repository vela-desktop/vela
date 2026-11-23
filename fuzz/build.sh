#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the libFuzzer targets straight from their sources, without the
# project's dependencies (wlroots, Qt): what .github/workflows/fuzz.yml uses.
#   sh fuzz/build.sh [output directory]      (default: build-fuzz)
# Then, for example: build-fuzz/vela-fuzz-config -max_total_time=60 fuzz/corpus/config
set -eu
root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
out=${1:-build-fuzz}
cc=${CC:-clang}
case "$out" in
    /*) ;;
    *) out=./$out ;;
esac
mkdir -p -- "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
flags="-std=c17 -D_GNU_SOURCE -g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
pure="compositor/src/config.c compositor/src/desktop_conf.c compositor/src/legacy_names.c compositor/src/path_security.c compositor/src/util.c common/alloc/alloc.c"
cd "$root"
# shellcheck disable=SC2086 # the flag and source lists are meant to split
$cc $flags -Ifuzz -Icommon -Icompositor/src fuzz/fuzz_config.c fuzz/fuzz_files.c $pure -o "$out/vela-fuzz-config"
# shellcheck disable=SC2086
$cc $flags -Ifuzz -Icommon -Icompositor/src fuzz/fuzz_ini.c fuzz/fuzz_files.c $pure -o "$out/vela-fuzz-ini"
# shellcheck disable=SC2086
$cc $flags -Ifuzz -Icommon -Ibrightness/src fuzz/fuzz_ddc.c brightness/src/ddc.c -o "$out/vela-fuzz-ddc"
echo "Built: $out/vela-fuzz-config $out/vela-fuzz-ini $out/vela-fuzz-ddc"
