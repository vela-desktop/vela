#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# The Python supervisor validates the compositor and optional shell paths.
# Keep the Popen invocation fixed rather than accepting arbitrary CLI options.
set -eu
if [ -n "${VELA_PRIVATE_SHELL:-}" ]; then
    exec "$VELA_PRIVATE_COMPOSITOR" -s "$VELA_PRIVATE_SHELL"
else
    exec "$VELA_PRIVATE_COMPOSITOR"
fi
