#!/bin/sh
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
umask 077
compat=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/vela"
export HOME="$tmp" XDG_CONFIG_HOME="$tmp"

check() {
    wanted=$1
    desktop=$2
    kde=$3
    (
        XDG_CURRENT_DESKTOP=Other
        KDE_SESSION_VERSION=8
        export XDG_CURRENT_DESKTOP KDE_SESSION_VERSION
        . "$compat"
        [ "$VELA_SECRET_BACKEND" = "$wanted" ]
        [ "$XDG_CURRENT_DESKTOP" = "$desktop" ]
        [ "${KDE_SESSION_VERSION-unset}" = "$kde" ]
    )
}

check kwallet Vela:KDE 6
printf '%s\n' 'secret-backend=gnome' > "$tmp/vela/vela.conf"
check gnome Vela:GNOME unset
printf '%s\n' 'secret-backend=gnome' 'secret-backend=kwallet' > "$tmp/vela/vela.conf"
check kwallet Vela:KDE 6
printf '%s\n' 'secret-backend=kwallet' 'secret-backend=gnome' > "$tmp/vela/vela.conf"
check gnome Vela:GNOME unset
printf '%s\n' 'secret-backend=invalid' > "$tmp/vela/vela.conf"
check kwallet Vela:KDE 6
echo "credential compatibility selection OK"
