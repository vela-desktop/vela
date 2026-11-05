#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Starts Vela inside a window of your current session (such as KDE), with the
# shell already loaded. Close with Alt+Shift+Esc or by closing the window.
#
# Usage: scripts/run-nested.sh [build-directory]

set -eu

BUILD_DIR="${1:-build}"
COMPOSITOR="$BUILD_DIR/compositor/vela-compositor"
SHELL_BIN="$BUILD_DIR/shell/vela-shell"

if [ ! -x "$COMPOSITOR" ]; then
    echo "$COMPOSITOR not found: build first (see README)." >&2
    exit 1
fi

# Keyboard layout: Italian unless you chose another one.
export XKB_DEFAULT_LAYOUT="${XKB_DEFAULT_LAYOUT:-it}"

. "$(dirname "$0")/private-session.sh"

if [ -x "$SHELL_BIN" ]; then
    SHELL_PATH="$(cd "$(dirname "$SHELL_BIN")" && pwd)/$(basename "$SHELL_BIN")"
    vela_run_private_session "$COMPOSITOR" -s "$SHELL_PATH"
else
    echo "Shell not built: starting only the compositor." >&2
    vela_run_private_session "$COMPOSITOR"
fi
