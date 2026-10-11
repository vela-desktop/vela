#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Starts Vela without a window (the wlroots "headless" backend): nothing
# appears on screen, but apps open and everything can be watched and driven
# with the tools in tools/ (vela-screen-dump, vela-windows, vela-input). For automated
# tests, even while you use the computer.
#
# Usage: scripts/run-headless.sh [build-directory] The log says which
# WAYLAND_DISPLAY it runs on; close with Ctrl+C or SIGTERM.

set -eu

BUILD_DIR="${1:-build}"
COMPOSITOR="$BUILD_DIR/compositor/vela-compositor"
SHELL_BIN="$BUILD_DIR/shell/vela-shell"

if [ ! -x "$COMPOSITOR" ]; then
    echo "$COMPOSITOR not found: build first (see README)." >&2
    exit 1
fi

export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export VELA_OUTPUT_SIZE="${VELA_OUTPUT_SIZE:-1920x1080}"
# Virtual mouse and keyboard for vela-input.
export VELA_DEBUG_INPUT=1
# Apps started from here must not try to reconnect elsewhere.
unset QT_WAYLAND_RECONNECT

if [ -x "$SHELL_BIN" ]; then
    SHELL_PATH="$(cd "$(dirname "$SHELL_BIN")" && pwd)/$(basename "$SHELL_BIN")"
    exec "$COMPOSITOR" -s "$SHELL_PATH"
else
    exec "$COMPOSITOR"
fi
