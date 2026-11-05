#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Linux-only test launcher. Python supervises detached children via subreaper;
# a plain shell wait or process-group kill would leave setsid() clients alive.
vela_run_private_session() {
    command -v python3 >/dev/null 2>&1 || {
        echo "python3 is required for isolated nested/headless sessions" >&2
        return 127
    }
    exec python3 "$(dirname "$0")/private-session.py" "$@"
}
