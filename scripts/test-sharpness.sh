#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Sharpness test (docs/renderer.md §3.10): at every scale, the vela-pattern
# window must reach the screen bit for bit, just opened, snapped left and
# right, maximized and restored.
#
# Usage: scripts/test-sharpness.sh [SCALES...]   (default: 1 1.25 1.5 1.75 2)
# Needs Python 3 with numpy and Pillow.

set -u
cd "$(dirname "$0")/.."
BUILD="${VELA_BUILD:-build}"
# CTest forwards its selected Python; standalone runs keep the PATH default.
PYTHON="${VELA_TEST_PYTHON:-python3}"
for binary in "$BUILD/compositor/vela-compositor" "$BUILD/tools/vela-pattern" \
        "$BUILD/tools/vela-screen-dump" "$BUILD/tools/vela-input"; do
    if [ ! -x "$binary" ]; then
        echo "$binary not found: build the compositor and test tools first." >&2
        exit 1
    fi
done
SCALES="${*:-1 1.25 1.5 1.75 2}"
TMP=$(mktemp -d)
FAILED=0
# Empty configuration: the night light or a color filter of whoever runs the
# test would change every pixel.
export XDG_CONFIG_HOME="$TMP/config" XDG_DATA_HOME="$TMP/data" XDG_CACHE_HOME="$TMP/cache"

for scale in $SCALES; do
    echo "== scale $scale"
    # Rounded corners (radius 8 logical) touch only those squares.
    CORNER=$("$PYTHON" -c "import math; print(math.ceil(8 * $scale) + 1)")
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 VELA_DEBUG_INPUT=1 VELA_SCALE=$scale \
        VELA_OUTPUT_SIZE=2560x1440@60 "$BUILD/compositor/vela-compositor" > "$TMP/log" 2>&1 &
    PID=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.2
        DISPLAY_NAME=$(grep -o "WAYLAND_DISPLAY=[a-z0-9-]*" "$TMP/log" | head -1 | cut -d= -f2)
        [ -n "$DISPLAY_NAME" ] && break
    done
    export WAYLAND_DISPLAY=$DISPLAY_NAME
    # In real-GPU CI, inspect the renderer Vela actually selected, not just vulkaninfo.
    if [ -n "${VELA_GPU_EXPECT_VENDOR:-}" ]; then
        if ! "$PYTHON" tests/functional/renderer_identity.py "$TMP/log" "$VELA_GPU_EXPECT_VENDOR"; then
            kill "$PID" 2> /dev/null
            wait "$PID" 2> /dev/null
            rm -rf "$TMP"
            exit 1
        fi
    fi
    "$BUILD/tools/vela-pattern" 401 301 2> "$TMP/pattern.log" &
    CLIENT=$!
    sleep 1

    check() {
        sleep 0.8
        "$BUILD/tools/vela-screen-dump" "$TMP/shot.png" || { echo "capture failed"; FAILED=1; return; }
        "$PYTHON" scripts/sharpness-check.py "$TMP/shot.png" "$1" "$CORNER" || FAILED=1
    }
    check opened
    "$BUILD/tools/vela-input" key super+Left; check "snapped left"
    "$BUILD/tools/vela-input" key super+Right key super+Right; check "snapped right"
    "$BUILD/tools/vela-input" key super+Up; check maximized
    "$BUILD/tools/vela-input" key super+Down; check restored

    kill $CLIENT 2> /dev/null
    kill $PID
    wait $PID 2> /dev/null
done
rm -rf "$TMP"
[ $FAILED -eq 0 ] && echo "Everything bit for bit." || echo "Some pixels differ."
exit $FAILED
