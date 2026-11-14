#!/bin/sh
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
umask 077
compat=$1
ensure=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/vela" "$tmp/runtime"
export HOME="$tmp" XDG_CONFIG_HOME="$tmp" XDG_RUNTIME_DIR="$tmp/runtime"

check()
{
    wanted=$1
    desktop=$2
    kde=$3
    configured=$4
    (
        XDG_CURRENT_DESKTOP=Other
        KDE_SESSION_VERSION=8
        export XDG_CURRENT_DESKTOP KDE_SESSION_VERSION
        . "$compat"
        [ "$VELA_SECRET_BACKEND" = "$wanted" ]
        [ "$XDG_CURRENT_DESKTOP" = "$desktop" ]
        [ "${KDE_SESSION_VERSION-unset}" = "$kde" ]
        [ "$VELA_SECRET_CONFIGURED" = "$configured" ]
    )
}

# The old KWallet default is retained, but does not count as an explicit
# first-login decision. A user choosing KWallet must persist that decision.
check kwallet Vela:KDE 6 0
printf '%s\n' 'secret-backend=gnome' > "$tmp/vela/vela.conf"
check gnome Vela:GNOME unset 1
printf '%s\n' 'secret-backend=gnome' 'secret-backend=kwallet' > "$tmp/vela/vela.conf"
check kwallet Vela:KDE 6 1
printf '%s\n' 'secret-backend=kwallet' 'secret-backend=gnome' > "$tmp/vela/vela.conf"
check gnome Vela:GNOME unset 1
printf '%s\n' 'secret-backend=invalid' > "$tmp/vela/vela.conf"
check kwallet Vela:KDE 6 0
printf '%s\n' '# secret-backend=gnome' > "$tmp/vela/vela.conf"
check kwallet Vela:KDE 6 0

# A controlled, real POSIX-shell check of the D-Bus ownership policy. No
# user keyring or systemd service is ever started by this test.
. "$ensure"
binary="$tmp/daemon"
: > "$binary"
chmod 700 "$binary"
calls="$tmp/systemctl-calls"
status="$XDG_RUNTIME_DIR/vela-credential-compat.status"

MOCK_OWNER=none
MOCK_NEXT_OWNER=none
MOCK_FAIL=0
busctl()
{
    case "$MOCK_OWNER" in
        none) return 1 ;;
        malformed) printf '%s\n' 'not a typed PID'; return 0 ;;
        *) printf '%s\n' 'u 1234'; return 0 ;;
    esac
}
readlink()
{
    case "$MOCK_OWNER" in
        gnome) printf '%s\n' /usr/bin/gnome-keyring-daemon ;;
        kwallet) printf '%s\n' /usr/bin/kwalletd6 ;;
        other) printf '%s\n' /usr/bin/other-keyring ;;
        *) return 1 ;;
    esac
}
systemctl()
{
    printf '%s\n' "$*" >> "$calls"
    [ "$MOCK_FAIL" -eq 0 ] || return 1
    MOCK_OWNER=$MOCK_NEXT_OWNER
}
sleep() { :; }

: > "$calls"
MOCK_OWNER=gnome
vela_credential_ensure gnome "$tmp/does-not-exist" gnome.service
[ "$(cat "$status")" = ok ]
[ ! -s "$calls" ] # PAM-owned daemon: never start or take over.

MOCK_OWNER=kwallet
if vela_credential_ensure gnome "$binary" gnome.service; then
    echo 'wrong provider accepted' >&2; exit 1
fi
[ "$(cat "$status")" = conflict ]
[ ! -s "$calls" ] # Never start a competing secrets service.

MOCK_OWNER=none
if vela_credential_ensure gnome "$tmp/missing" gnome.service; then
    echo 'missing backend accepted' >&2; exit 1
fi
[ "$(cat "$status")" = missing ]
[ ! -s "$calls" ]

MOCK_OWNER=none
MOCK_NEXT_OWNER=gnome
vela_credential_ensure gnome "$binary" gnome.service
[ "$(cat "$status")" = ok ]
[ "$(wc -l < "$calls")" -eq 1 ]

MOCK_OWNER=none
MOCK_NEXT_OWNER=other
if vela_credential_ensure gnome "$binary" gnome.service; then
    echo 'foreign owner accepted after startup' >&2; exit 1
fi
[ "$(cat "$status")" = conflict ]

MOCK_OWNER=none
MOCK_NEXT_OWNER=none
if vela_credential_ensure kwallet "$binary" kwallet.service; then
    echo 'no owner accepted' >&2; exit 1
fi
[ "$(cat "$status")" = failed ]

MOCK_OWNER=malformed
if vela_credential_ensure gnome "$binary" gnome.service; then
    echo 'malformed bus response accepted' >&2; exit 1
fi
[ "$(cat "$status")" = unknown ] && exit 1
[ "$(cat "$status")" = unverified ]

MOCK_OWNER=none
MOCK_FAIL=1
if vela_credential_ensure gnome "$binary" gnome.service; then
    echo 'failed systemd start accepted' >&2; exit 1
fi
[ "$(cat "$status")" = failed ]

echo "credential compatibility selection and owner validation OK"
