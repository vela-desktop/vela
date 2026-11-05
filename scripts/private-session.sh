#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Shared by the manual nested/headless launchers. They are isolated test
# sessions, not logins: keep the host runtime directory and session bus so
# nested Wayland clients can reach the host compositor. These are NOT a
# security sandbox. Persistent user paths (including HOME) are private.

vela_private_cleanup() {
    status=$1
    trap - 0 HUP INT TERM
    if [ -n "${VELA_PRIVATE_PID:-}" ] && kill -0 "$VELA_PRIVATE_PID" 2>/dev/null; then
        kill -TERM "$VELA_PRIVATE_PID" 2>/dev/null || :
        wait "$VELA_PRIVATE_PID" 2>/dev/null || :
    fi
    if [ -n "${VELA_PRIVATE_ROOT:-}" ]; then
        rm -rf -- "$VELA_PRIVATE_ROOT"
    fi
    exit "$status"
}

vela_run_private_session() {
    case "${VELA_TEST_COPY_CONFIG:-0}" in
        0|1) ;;
        *) echo "VELA_TEST_COPY_CONFIG must be 0 or 1" >&2; return 2 ;;
    esac
    # XDG base directories must be absolute. Never construct them from a
    # relative TMPDIR, or from a path supplied by an untrusted test command.
    temp_parent=${TMPDIR:-/tmp}
    case "$temp_parent" in
        /*) ;;
        *) echo "TMPDIR must be an absolute path" >&2; return 2 ;;
    esac

    # Capture the real location before switching HOME and XDG_CONFIG_HOME.
    real_config=${XDG_CONFIG_HOME:-${HOME:?HOME must be set}/.config}
    VELA_PRIVATE_ROOT=$(mktemp -d "$temp_parent/vela-session.XXXXXXXX") || return 1
    VELA_PRIVATE_PID=
    trap 'vela_private_cleanup $?' 0
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    export HOME="$VELA_PRIVATE_ROOT/home"
    export XDG_CONFIG_HOME="$VELA_PRIVATE_ROOT/config"
    export XDG_DATA_HOME="$VELA_PRIVATE_ROOT/data"
    export XDG_CACHE_HOME="$VELA_PRIVATE_ROOT/cache"
    export XDG_STATE_HOME="$VELA_PRIVATE_ROOT/state"
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"

    # Opt-in: copy the two Vela-specific config trees, without symlinks to
    # host files. Never point the test session at the originals.
    if [ "${VELA_TEST_COPY_CONFIG:-0}" = 1 ]; then
        for name in vela Vela; do
            if [ -d "$real_config/$name" ]; then
                cp -RL -- "$real_config/$name" "$XDG_CONFIG_HOME/$name"
            fi
        done
    fi

    # The shell must remain alive to run the exit trap; exec would leak the
    # temporary directory and leave it behind after normal termination.
    "$@" &
    VELA_PRIVATE_PID=$!
    wait "$VELA_PRIVATE_PID"
}
