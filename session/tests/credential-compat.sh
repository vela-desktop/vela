#!/bin/sh
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
umask 077
compat=$1
ensure=$2
hook=$3
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
    case "$*" in
        *NameHasOwner*)
            case "$MOCK_OWNER" in
                none) printf '%s\n' 'b false' ;;
                malformed) printf '%s\n' 'not a boolean' ;;
                busdown) return 1 ;;
                *) printf '%s\n' 'b true' ;;
            esac ;;
        *GetConnectionUnixProcessID*)
            case "$MOCK_OWNER" in
                malformed_pid) printf '%s\n' 'not a typed PID' ;;
                *) printf '%s\n' 'u 1234' ;;
            esac ;;
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

MOCK_OWNER=busdown
if vela_credential_ensure gnome "$binary" gnome.service; then
    echo 'unreachable bus accepted' >&2; exit 1
fi
[ "$(cat "$status")" = unverified ]

MOCK_OWNER=none
MOCK_FAIL=1
if vela_credential_ensure gnome "$binary" gnome.service; then
    echo 'failed systemd start accepted' >&2; exit 1
fi
[ "$(cat "$status")" = failed ]

# Run the actual session hook in an isolated user-manager fixture to verify
# that the first start probes the owner once, restarts do not, and a competing
# graphical session is left untouched. No real systemd or keyring is contacted.
mockbin="$tmp/mockbin"
mkdir -p "$mockbin" "$tmp/hook-runtime"
cat > "$mockbin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$MOCK_SYSTEMCTL_CALLS"
case "$*" in
    *'is-active graphical-session.target')
        [ "${MOCK_GRAPHICAL_ACTIVE:-0}" = 1 ] ;;
    *) exit 0 ;;
esac
EOF
cat > "$mockbin/busctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$MOCK_BUS_CALLS"
case "$*" in
    *NameHasOwner*) printf '%s\n' 'b true' ;;
    *) printf '%s\n' 'u 1234' ;;
esac
EOF
cat > "$mockbin/readlink" <<'EOF'
#!/bin/sh
printf '%s\n' /usr/bin/kwalletd6
EOF
cat > "$mockbin/dbus-update-activation-environment" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 700 "$mockbin/"*
export PATH="$mockbin:$PATH"
export MOCK_BUS_CALLS="$tmp/bus-calls" MOCK_SYSTEMCTL_CALLS="$tmp/systemctl-calls"
export MOCK_GRAPHICAL_ACTIVE=0 VELA_SECRET_BACKEND=kwallet VELA_SECRET_CONFIGURED=1
export WAYLAND_DISPLAY=wayland-73 XDG_CURRENT_DESKTOP=Vela:KDE
export XDG_RUNTIME_DIR="$tmp/hook-runtime"
: > "$MOCK_BUS_CALLS"
: > "$MOCK_SYSTEMCTL_CALLS"
sh "$hook" start
[ -f "$XDG_RUNTIME_DIR/vela-session-wayland-73" ]
[ "$(cat "$XDG_RUNTIME_DIR/vela-credential-compat.status")" = ok ]
[ "$(wc -l < "$MOCK_BUS_CALLS")" -eq 2 ]
sh "$hook" start
[ "$(wc -l < "$MOCK_BUS_CALLS")" -eq 2 ]
sh "$hook" stop
[ ! -e "$XDG_RUNTIME_DIR/vela-session-wayland-73" ]
[ ! -e "$XDG_RUNTIME_DIR/vela-credential-compat.status" ]
MOCK_GRAPHICAL_ACTIVE=1
export MOCK_GRAPHICAL_ACTIVE
sh "$hook" start
[ ! -e "$XDG_RUNTIME_DIR/vela-session-wayland-73" ]
[ "$(wc -l < "$MOCK_BUS_CALLS")" -eq 2 ]

echo "credential compatibility selection, owner validation, and restart lifecycle OK"
