#!/bin/sh
# SPDX-FileCopyrightText: 2026 Vela contributors
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
umask 077
compat=$1
ensure=$2
hook=$3
# Sourcing with the hook as argv[0] keeps its relative helper paths intact.
# Ordinary cases wait for all workers; race cases get an explicit completion
# acknowledgement from the exact old hook worker.
hook_start()
{
    sh -c '. "$0"; wait' "$hook" start
}

start_hook_worker()
{
    worker_done=$1
    ( hook_start; : > "$worker_done" ) &
    worker_pid=$!
}

wait_hook_worker()
{
    waited_pid=$1
    waited_done=$2
    waited=0
    while [ ! -e "$waited_done" ] && [ "$waited" -lt 500 ]; do
        /bin/sleep 0.01
        waited=$((waited + 1))
    done
    [ -e "$waited_done" ] || { echo "old hook worker did not finish" >&2; return 1; }
    wait "$waited_pid"
}

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

# Keep the launcher, installed unit, and Settings in agreement about the
# KDE Secret Service daemon. kwalletd6 is a separate native API wrapper.
if [ "$#" -ge 4 ]; then
    grep -q 'vela_credential_ensure kwallet /usr/bin/ksecretd' "$hook"
    grep -q '^ConditionFileIsExecutable=/usr/bin/ksecretd$' "$4"
    grep -q '^ExecStart=/usr/bin/ksecretd$' "$4"
fi

# A controlled, real POSIX-shell check of the D-Bus ownership policy. No
# user keyring or systemd service is ever started by this test.
. "$ensure"
unset PAM_KWALLET5_LOGIN
# Exercise the real read-only metadata probe without inspecting host daemons.
(
    proc="$tmp/proc"
    mkdir -p "$proc/self" "$proc/net" "$proc/42"
    : > "$proc/self/status"
    printf '%s\n' 'Num RefCount Protocol Flags Type St Inode Path' > "$proc/net/unix"
    uid=$(id -u)
    other_uid=$((uid + 1))
    printf '%s\n' ksecretd > "$proc/42/comm"
    printf 'State:\tS (sleeping)\nUid:\t%s\t%s\t%s\t%s\n' \
        "$uid" "$uid" "$uid" "$uid" > "$proc/42/status"
    ln -s /usr/bin/ksecretd "$proc/42/exe"
    # Reading either credential-related file would hang this regression.
    mkfifo "$proc/42/cmdline" "$proc/42/environ"
    [ "$(vela_credential_kwallet_pending "$proc")" = pending ]
    rm "$proc/42/exe"
    ln -s '/usr/bin/ksecretd (deleted)' "$proc/42/exe"
    [ "$(vela_credential_kwallet_pending "$proc")" = pending ]
    printf 'State:\tS\nUid:\t%s\t%s\t%s\t%s\n' \
        "$other_uid" "$other_uid" "$other_uid" "$other_uid" > "$proc/42/status"
    [ "$(vela_credential_kwallet_pending "$proc")" = none ]
    printf 'State:\tZ\nUid:\t%s\t%s\t%s\t%s\n' \
        "$uid" "$uid" "$uid" "$uid" > "$proc/42/status"
    [ "$(vela_credential_kwallet_pending "$proc")" = none ]
    printf 'State:\tS\nUid:\t%s\t%s\t%s\t%s\n' \
        "$uid" "$uid" "$uid" "$uid" > "$proc/42/status"
    rm "$proc/42/exe"
    [ "$(vela_credential_kwallet_pending "$proc")" = unknown ]
    ln -s /usr/bin/another-daemon "$proc/42/exe"
    [ "$(vela_credential_kwallet_pending "$proc")" = none ]
    rm -r "$proc/42"

    PAM_KWALLET5_LOGIN="$tmp/pam.socket"
    export PAM_KWALLET5_LOGIN
    # A stale inherited marker is not evidence of a pending PAM instance.
    [ "$(vela_credential_kwallet_pending "$proc")" = none ]
    # A live PAM listener also detects the interval before ksecretd execs.
    printf '0000: 2 0 00010000 0001 01 123 %s\n' "$PAM_KWALLET5_LOGIN" > "$proc/net/unix"
    [ "$(vela_credential_kwallet_pending "$proc")" = pending ]
    printf '0000: 2 0 00000000 0001 03 123 %s\n' "$PAM_KWALLET5_LOGIN" > "$proc/net/unix"
    [ "$(vela_credential_kwallet_pending "$proc")" = none ]
    rm "$proc/net/unix"
    [ "$(vela_credential_kwallet_pending "$proc")" = unknown ]
    [ "$(vela_credential_kwallet_pending "$tmp/absent-proc")" = unknown ]
)

binary="$tmp/daemon"
: > "$binary"
chmod 700 "$binary"
calls="$tmp/systemctl-calls"
status="$XDG_RUNTIME_DIR/vela-credential-compat.status"

MOCK_OWNER=none
MOCK_NEXT_OWNER=none
MOCK_NATIVE_OWNER=none
MOCK_NEXT_NATIVE_OWNER=none
MOCK_FAIL=0
MOCK_PENDING=none
MOCK_SLEEP_COUNT=0
MOCK_SLEEP_EVENT=
MOCK_SLEEP_AT=0
MOCK_SLEEP_OWNER=ksecretd
vela_credential_kwallet_pending() { printf '%s\n' "$MOCK_PENDING"; }
busctl()
{
    case "$*" in
        *org.kde.kwalletd6) owner=$MOCK_NATIVE_OWNER ;;
        *) owner=$MOCK_OWNER ;;
    esac
    case "$*" in
        *NameHasOwner*)
            case "$owner" in
                none) printf '%s\n' 'b false' ;;
                malformed) printf '%s\n' 'not a boolean' ;;
                busdown) return 1 ;;
                *) printf '%s\n' 'b true' ;;
            esac ;;
        *GetConnectionUnixProcessID*)
            case "$owner" in
                malformed_pid) printf '%s\n' 'not a typed PID' ;;
                *) case "$*" in
                    *org.kde.kwalletd6) printf '%s\n' 'u 5678' ;;
                    *) printf '%s\n' 'u 1234' ;;
                esac ;;
            esac ;;
    esac
}
readlink()
{
    case "$1" in
        /proc/5678/exe)
            case "$MOCK_NATIVE_OWNER" in
                kwallet) printf '%s\n' /usr/bin/kwalletd6 ;;
                other) printf '%s\n' /usr/bin/other-keyring ;;
                *) return 1 ;;
            esac ;;
        *)
            case "$MOCK_OWNER" in
                gnome) printf '%s\n' /usr/bin/gnome-keyring-daemon ;;
                kwallet) printf '%s\n' /usr/bin/kwalletd6 ;;
                ksecretd) printf '%s\n' /usr/bin/ksecretd ;;
                other) printf '%s\n' /usr/bin/other-keyring ;;
                *) return 1 ;;
            esac ;;
    esac
}
systemctl()
{
    printf '%s\n' "$*" >> "$calls"
    case "$*" in *'start '*) [ "$MOCK_PENDING" = none ] || return 1 ;; esac
    [ "$MOCK_FAIL" -eq 0 ] || return 1
    MOCK_OWNER=$MOCK_NEXT_OWNER
    MOCK_NATIVE_OWNER=$MOCK_NEXT_NATIVE_OWNER
    if [ -n "${MOCK_ROLLOVER_SESSION:-}" ]; then
        printf '%s\n' new-login > "$VELA_CREDENTIAL_SESSION_STATE"
    fi
}
sleep()
{
    MOCK_SLEEP_COUNT=$((MOCK_SLEEP_COUNT + 1))
    if [ "$MOCK_SLEEP_COUNT" -eq "$MOCK_SLEEP_AT" ]; then
        case "$MOCK_SLEEP_EVENT" in
            register) MOCK_OWNER=$MOCK_SLEEP_OWNER; MOCK_PENDING=none ;;
            exit) MOCK_PENDING=none ;;
        esac
    fi
}

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

# Recent KWallet splits native org.kde.kwalletd6 from ksecretd/Secret Service.
# Treat native-only availability as limited, not a broken wallet or full success.
MOCK_OWNER=ksecretd
vela_credential_ensure kwallet "$tmp/does-not-exist" kwallet.service
[ "$(cat "$status")" = ok ]
[ ! -s "$calls" ]

MOCK_OWNER=none
MOCK_NATIVE_OWNER=kwallet
MOCK_NEXT_OWNER=ksecretd
MOCK_NEXT_NATIVE_OWNER=kwallet
: > "$calls"
# An existing native wrapper must not prevent starting ksecretd.
vela_credential_ensure kwallet "$binary" kwallet.service
[ "$(cat "$status")" = ok ]
[ "$(grep -c 'start kwallet.service' "$calls")" -eq 1 ]
if grep -q 'stop kwallet.service' "$calls"; then
    echo 'working provider was stopped' >&2; exit 1
fi

# Each scenario needs a fresh call log: the previous successful start is
# intentional, but must not be mistaken for a start during a conflict.
: > "$calls"
MOCK_OWNER=none
MOCK_NATIVE_OWNER=other
if vela_credential_ensure kwallet "$binary" kwallet.service; then
    echo 'foreign native API owner accepted' >&2; exit 1
fi
[ "$(cat "$status")" = conflict ]
[ ! -s "$calls" ]

MOCK_NATIVE_OWNER=none
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
# The daemon was ours to start, so this failed attempt must stop its unit.
grep -q -- '--no-block stop gnome.service' "$calls"

MOCK_OWNER=none
MOCK_NEXT_OWNER=none
MOCK_NEXT_NATIVE_OWNER=kwallet
: > "$calls"
if vela_credential_ensure kwallet "$binary" kwallet.service; then
    echo 'new native-only daemon reported full Secret Service compatibility' >&2; exit 1
fi
[ "$(cat "$status")" = limited ]
[ "$(grep -c 'start kwallet.service' "$calls")" -eq 1 ]
grep -q -- '--no-block stop kwallet.service' "$calls"

# A native-only wallet with no ksecretd executable remains limited.
MOCK_OWNER=none
MOCK_NATIVE_OWNER=kwallet
: > "$calls"
if vela_credential_ensure kwallet "$tmp/missing" kwallet.service; then
    echo 'missing ksecretd accepted' >&2; exit 1
fi
[ "$(cat "$status")" = limited ]
[ ! -s "$calls" ]

MOCK_OWNER=none
MOCK_NEXT_OWNER=none
MOCK_NEXT_NATIVE_OWNER=none
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

# PAM may have a process/listener without any registered D-Bus name yet.
# Wait for it to register or exit; a timeout must not start or stop a unit.
(
    MOCK_FAIL=0
    for native in none kwallet; do
        MOCK_OWNER=none
        MOCK_NATIVE_OWNER=$native
        MOCK_NEXT_NATIVE_OWNER=$native
        MOCK_NEXT_OWNER=none
        MOCK_PENDING=pending
        MOCK_SLEEP_EVENT=register
        MOCK_SLEEP_AT=3
        MOCK_SLEEP_OWNER=ksecretd
        : > "$calls"
        vela_credential_ensure kwallet "$binary" kwallet.service
        [ "$(cat "$status")" = ok ]
        [ ! -s "$calls" ]
    done

    MOCK_OWNER=none
    MOCK_PENDING=pending
    MOCK_SLEEP_EVENT='exit'
    MOCK_SLEEP_AT=1
    MOCK_NEXT_OWNER=ksecretd
    : > "$calls"
    vela_credential_ensure kwallet "$binary" kwallet.service
    [ "$(cat "$status")" = ok ]
    [ "$(grep -c 'start kwallet.service' "$calls")" -eq 1 ]

    for pending in pending unknown; do
        MOCK_PENDING=$pending
        MOCK_SLEEP_EVENT=
        MOCK_NEXT_OWNER=none
        : > "$calls"
        if vela_credential_ensure kwallet "$binary" kwallet.service; then
            echo 'unverified pending provider accepted' >&2; exit 1
        fi
        [ "$(cat "$status")" = unverified ]
        [ ! -s "$calls" ]
    done

    MOCK_PENDING=pending
    MOCK_SLEEP_EVENT=register
    MOCK_SLEEP_AT=2
    MOCK_SLEEP_OWNER=other
    : > "$calls"
    if vela_credential_ensure kwallet "$binary" kwallet.service; then
        echo 'foreign provider accepted during PAM wait' >&2; exit 1
    fi
    [ "$(cat "$status")" = conflict ]
    [ ! -s "$calls" ]

    # Registration during the *last* wait must win over native-only cleanup.
    for backend in kwallet gnome; do
        MOCK_PENDING=none
        MOCK_OWNER=none
        MOCK_NATIVE_OWNER=kwallet
        MOCK_NEXT_NATIVE_OWNER=kwallet
        MOCK_NEXT_OWNER=none
        MOCK_SLEEP_EVENT=register
        MOCK_SLEEP_AT=10
        MOCK_SLEEP_OWNER=$backend
        : > "$calls"
        vela_credential_ensure "$backend" "$binary" "$backend.service"
        [ "$(cat "$status")" = ok ]
        [ "$(grep -c "start $backend.service" "$calls")" -eq 1 ]
        if grep -q 'stop ' "$calls"; then
            echo 'provider registered on the final wait was stopped' >&2; exit 1
        fi
    done

    MOCK_SLEEP_OWNER=other
    : > "$calls"
    if vela_credential_ensure kwallet "$binary" kwallet.service; then
        echo 'foreign final-wait provider accepted' >&2; exit 1
    fi
    [ "$(cat "$status")" = conflict ]
    grep -q -- '--no-block stop kwallet.service' "$calls"

    # The native fallback query can itself be slow. A provider that becomes
    # ready during it must be accepted by the final Secret Service lookup.
    (
        owner_file="$tmp/late-native-owner"
        printf '%s\n' none > "$owner_file"
        : > "$calls"
        vela_credential_owner()
        {
            if [ "${1:-}" = org.kde.kwalletd6 ]; then
                [ ! -s "$calls" ] || printf '%s\n' kwallet > "$owner_file"
                printf '%s\n' kwallet
            else
                cat "$owner_file"
            fi
        }
        sleep() { :; }
        vela_credential_ensure kwallet "$binary" kwallet.service
        [ "$(cat "$status")" = ok ]
        if grep -q 'stop ' "$calls"; then
            echo 'provider became ready during native lookup and was stopped' >&2; exit 1
        fi
    )
)

# Run the actual session hook in an isolated user-manager fixture to verify
# that the first start probes the owner once, restarts do not, and a competing
# graphical session is left untouched. No real systemd or keyring is contacted.
# An old provider checker must not start or stop units, or overwrite the
# status marker, after the same display belongs to a different login.
VELA_CREDENTIAL_SESSION_STATE="$tmp/provider-session"
VELA_CREDENTIAL_SESSION_TOKEN=old-login
printf '%s\n' new-login > "$VELA_CREDENTIAL_SESSION_STATE"
: > "$calls"
previous_status=$(cat "$status")
vela_credential_ensure kwallet "$binary" kwallet.service
[ ! -s "$calls" ]
[ "$(cat "$status")" = "$previous_status" ]
# Rollover while systemctl start was in flight also must not stop a unit
# that may now be needed by the new login.
printf '%s\n' old-login > "$VELA_CREDENTIAL_SESSION_STATE"
MOCK_FAIL=0
MOCK_OWNER=none
MOCK_NATIVE_OWNER=none
MOCK_NEXT_OWNER=other
MOCK_ROLLOVER_SESSION=1
vela_credential_ensure kwallet "$binary" kwallet.service
[ "$(grep -c 'start kwallet.service' "$calls")" -eq 1 ]
if grep -q 'stop kwallet.service' "$calls"; then
    echo 'old checker stopped a later login unit' >&2; exit 1
fi
[ "$(cat "$status")" = "$previous_status" ]
unset VELA_CREDENTIAL_SESSION_STATE VELA_CREDENTIAL_SESSION_TOKEN MOCK_ROLLOVER_SESSION

mockbin="$tmp/mockbin"
mkdir -p "$mockbin" "$tmp/hook-runtime"
cat > "$mockbin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$MOCK_SYSTEMCTL_CALLS"
if [ "$*" = '--user cat vela-session.target' ] &&
    [ "${MOCK_ACTION_KIND:-}" = target ] && [ -f "$MOCK_ACTION_WAIT" ]; then
    : > "$MOCK_ACTION_SIGNAL"
    while [ -f "$MOCK_ACTION_WAIT" ]; do /bin/sleep 0.01; done
fi
case "$*" in
    *'is-active graphical-session.target')
        [ "${MOCK_GRAPHICAL_ACTIVE:-0}" = 1 ] ;;
    *) exit 0 ;;
esac
EOF
cat > "$mockbin/busctl" <<'EOF'
#!/bin/sh
if [ -n "${MOCK_PROVIDER_WAIT_MARKER:-}" ] && [ -f "$MOCK_PROVIDER_WAIT_MARKER" ]; then
    : > "$MOCK_PROVIDER_SIGNAL"
    while [ -f "$MOCK_PROVIDER_WAIT_MARKER" ]; do
        /bin/sleep 0.01
    done
fi
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
# Optionally delay the old prompt child's *first* read of STATE so a logout
# and new login can happen before that read completes.
cat > "$mockbin/cat" <<'EOF'
#!/bin/sh
if [ "$#" -eq 1 ] &&
    [ "$1" = "$XDG_RUNTIME_DIR/vela-session-$WAYLAND_DISPLAY" ] &&
    [ -n "${MOCK_CAT_WAIT_MARKER:-}" ] && [ -f "$MOCK_CAT_WAIT_MARKER" ]; then
    : > "$MOCK_CAT_SIGNAL"
    while [ -f "$MOCK_CAT_WAIT_MARKER" ]; do
        /bin/sleep 0.01
    done
fi
exec /bin/cat "$@"
EOF
cat > "$mockbin/dbus-update-activation-environment" <<'EOF'
#!/bin/sh
exit 0
EOF
# No real UI is opened in tests: capture the first-login prompt invocation.
cat > "$mockbin/vela-settings" <<'EOF'
#!/bin/sh
if [ -n "${MOCK_SETTINGS_FAIL_ONCE:-}" ] && [ -e "$MOCK_SETTINGS_FAIL_ONCE" ]; then
    rm -f "$MOCK_SETTINGS_FAIL_ONCE"
    exit 1
fi
printf '%s\n' "$*" >> "$MOCK_SETTINGS_CALLS"
EOF
cat > "$mockbin/sleep" <<'EOF'
#!/bin/sh
if [ "$1" = 2 ] && [ -n "${MOCK_SLEEP_WAIT_MARKER:-}" ] && [ -f "$MOCK_SLEEP_WAIT_MARKER" ]; then
    : > "$MOCK_SLEEP_SIGNAL"
    while [ -f "$MOCK_SLEEP_WAIT_MARKER" ]; do
        /bin/sleep 0.01
    done
fi
exit 0
EOF
cat > "$mockbin/notify-send" <<'EOF'
#!/bin/sh
if [ "${MOCK_ACTION_KIND:-}" = notification ] && [ -f "$MOCK_ACTION_WAIT" ]; then
    : > "$MOCK_ACTION_SIGNAL"
    while [ -f "$MOCK_ACTION_WAIT" ]; do /bin/sleep 0.01; done
fi
printf '%s\n' "$*" >> "$MOCK_NOTIFICATION_CALLS"
EOF
# Pause after token validation, inside the status commit or target action.
cat > "$mockbin/mktemp" <<'EOF'
#!/bin/sh
if [ "${MOCK_ACTION_KIND:-}" = status ] && [ -f "$MOCK_ACTION_WAIT" ]; then
    : > "$MOCK_ACTION_SIGNAL"
    while [ -f "$MOCK_ACTION_WAIT" ]; do /bin/sleep 0.01; done
fi
exec /usr/bin/mktemp "$@"
EOF
cat > "$mockbin/flock" <<'EOF'
#!/bin/sh
if [ -n "${MOCK_LOCK_BLOCKED:-}" ] && [ "$*" = '-x 9' ]; then
    if /usr/bin/flock -n 9; then
        exit 0
    fi
    : > "$MOCK_LOCK_BLOCKED"
fi
exec /usr/bin/flock "$@"
EOF
chmod 700 "$mockbin/"*
export PATH="$mockbin:$PATH"
export MOCK_BUS_CALLS="$tmp/bus-calls" MOCK_SYSTEMCTL_CALLS="$tmp/systemctl-calls"
export MOCK_SETTINGS_CALLS="$tmp/settings-calls" MOCK_NOTIFICATION_CALLS="$tmp/notification-calls"
: > "$MOCK_NOTIFICATION_CALLS"
: > "$MOCK_SETTINGS_CALLS"
export MOCK_GRAPHICAL_ACTIVE=0 VELA_SECRET_BACKEND=kwallet VELA_SECRET_CONFIGURED=1
export WAYLAND_DISPLAY=wayland-73 XDG_CURRENT_DESKTOP=Vela:KDE
export XDG_RUNTIME_DIR="$tmp/hook-runtime"
: > "$MOCK_BUS_CALLS"
: > "$MOCK_SYSTEMCTL_CALLS"
hook_start
[ -f "$XDG_RUNTIME_DIR/vela-session-wayland-73" ]
initial_token=$(cat "$XDG_RUNTIME_DIR/vela-session-wayland-73")
[ -n "$initial_token" ]
[ "$(cat "$XDG_RUNTIME_DIR/vela-credential-compat.status")" = ok ]
[ "$(wc -l < "$MOCK_BUS_CALLS")" -eq 2 ]
hook_start
# A supervised compositor restart must retain the session identity.
[ "$(cat "$XDG_RUNTIME_DIR/vela-session-wayland-73")" = "$initial_token" ]
[ "$(wc -l < "$MOCK_BUS_CALLS")" -eq 2 ]
sh "$hook" stop
[ ! -e "$XDG_RUNTIME_DIR/vela-session-wayland-73" ]
[ ! -e "$XDG_RUNTIME_DIR/vela-credential-compat.status" ]
MOCK_GRAPHICAL_ACTIVE=1
export MOCK_GRAPHICAL_ACTIVE
hook_start
[ ! -e "$XDG_RUNTIME_DIR/vela-session-wayland-73" ]
[ "$(wc -l < "$MOCK_BUS_CALLS")" -eq 2 ]

# A working implicit KWallet fallback must STILL prompt for an explicit
# choice. Opening the compositor again may not prompt twice in the session.
MOCK_GRAPHICAL_ACTIVE=0
VELA_SECRET_CONFIGURED=0
export MOCK_GRAPHICAL_ACTIVE VELA_SECRET_CONFIGURED
hook_start
i=0
while [ ! -s "$MOCK_SETTINGS_CALLS" ] && [ "$i" -lt 100 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ "$(cat "$MOCK_SETTINGS_CALLS")" = "--page credential-setup" ]
hook_start
/bin/sleep 0.05
[ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -eq 1 ]
sh "$hook" stop

# After a persisted choice, a fresh login no longer requests the dialog.
VELA_SECRET_CONFIGURED=1
export VELA_SECRET_CONFIGURED
hook_start
/bin/sleep 0.05
[ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -eq 1 ]
sh "$hook" stop

# Deferring the choice must prompt again on the NEXT actual login.
VELA_SECRET_CONFIGURED=0
export VELA_SECRET_CONFIGURED
hook_start
i=0
while [ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -lt 2 ] && [ "$i" -lt 100 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -eq 2 ]
sh "$hook" stop

# A transient Settings launch failure must be retried, not silently lost.
export MOCK_SETTINGS_FAIL_ONCE="$tmp/fail-once"
: > "$MOCK_SETTINGS_FAIL_ONCE"
hook_start
i=0
while [ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -lt 3 ] && [ "$i" -lt 100 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ ! -e "$MOCK_SETTINGS_FAIL_ONCE" ]
[ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -eq 3 ]
sh "$hook" stop
unset MOCK_SETTINGS_FAIL_ONCE

# A delayed child from a logged-out session must not prompt during a new
# session, even if the same WAYLAND_DISPLAY is reused.
export MOCK_SLEEP_WAIT_MARKER="$tmp/hold-prompt" MOCK_SLEEP_SIGNAL="$tmp/prompt-ready"
: > "$MOCK_SLEEP_WAIT_MARKER"
old_done="$tmp/old-prompt-done"
start_hook_worker "$old_done"
old_worker=$worker_pid
i=0
while [ ! -e "$MOCK_SLEEP_SIGNAL" ] && [ "$i" -lt 100 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ -e "$MOCK_SLEEP_SIGNAL" ]
pending_token=$(cat "$XDG_RUNTIME_DIR/vela-session-wayland-73")
sh "$hook" stop
VELA_SECRET_CONFIGURED=1
export VELA_SECRET_CONFIGURED
sh "$hook" start
# Reusing the same WAYLAND_DISPLAY must not reuse the session's token.
new_token=$(cat "$XDG_RUNTIME_DIR/vela-session-wayland-73")
[ "$new_token" != "$initial_token" ]
[ "$new_token" != "$pending_token" ]
rm -f "$MOCK_SLEEP_WAIT_MARKER"
# The old helper must observe the changed session token and exit quietly.
wait_hook_worker "$old_worker" "$old_done"
[ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -eq 3 ]
sh "$hook" stop
unset MOCK_SLEEP_WAIT_MARKER MOCK_SLEEP_SIGNAL

# A second scheduling race: the old prompt child might not start executing
# until AFTER the new login has written a different STATE. Its token must
# come from the parent login, not the latest file content. A mocked cat traps
# the old implementation before its first STATE read; the new implementation
# pauses at its first sleep. Either way, restarting the login must not open
# the stale prompt.
VELA_SECRET_CONFIGURED=0
export VELA_SECRET_CONFIGURED
export MOCK_CAT_WAIT_MARKER="$tmp/hold-token-read" MOCK_CAT_SIGNAL="$tmp/token-read-ready"
export MOCK_SLEEP_WAIT_MARKER="$tmp/hold-first-sleep" MOCK_SLEEP_SIGNAL="$tmp/first-sleep-ready"
: > "$MOCK_CAT_WAIT_MARKER"
: > "$MOCK_SLEEP_WAIT_MARKER"
old_done="$tmp/old-token-worker-done"
start_hook_worker "$old_done"
old_worker=$worker_pid
i=0
while [ ! -e "$MOCK_CAT_SIGNAL" ] && [ ! -e "$MOCK_SLEEP_SIGNAL" ] && [ "$i" -lt 100 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ -e "$MOCK_CAT_SIGNAL" ] || [ -e "$MOCK_SLEEP_SIGNAL" ]
sh "$hook" stop
VELA_SECRET_CONFIGURED=1
export VELA_SECRET_CONFIGURED
sh "$hook" start
rm -f "$MOCK_CAT_WAIT_MARKER" "$MOCK_SLEEP_WAIT_MARKER"
wait_hook_worker "$old_worker" "$old_done"
[ "$(wc -l < "$MOCK_SETTINGS_CALLS")" -eq 3 ]
sh "$hook" stop
unset MOCK_CAT_WAIT_MARKER MOCK_CAT_SIGNAL MOCK_SLEEP_WAIT_MARKER MOCK_SLEEP_SIGNAL

# A provider owner lookup may need a GUI round trip. Keep it blocked until
# the hook has returned, exactly the ordering the compositor needs to dispatch.
VELA_SECRET_CONFIGURED=1
export VELA_SECRET_CONFIGURED
export MOCK_PROVIDER_WAIT_MARKER="$tmp/hold-provider" MOCK_PROVIDER_SIGNAL="$tmp/provider-ready"
: > "$MOCK_PROVIDER_WAIT_MARKER"
parent_done="$tmp/start-hook-returned"
targets_before=$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)
( sh "$hook" start; : > "$parent_done" ) &
parent_pid=$!
i=0
while [ ! -e "$parent_done" ] && [ "$i" -lt 200 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
returned_while_provider_blocked=0
[ ! -e "$parent_done" ] || returned_while_provider_blocked=1
i=0
while [ ! -e "$MOCK_PROVIDER_SIGNAL" ] && [ "$i" -lt 200 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
provider_reached_barrier=0
[ ! -e "$MOCK_PROVIDER_SIGNAL" ] || provider_reached_barrier=1
[ "$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)" -eq "$targets_before" ]
rm -f "$MOCK_PROVIDER_WAIT_MARKER"
wait "$parent_pid"
[ "$returned_while_provider_blocked" -eq 1 ] && [ "$provider_reached_barrier" -eq 1 ] || {
    echo "start hook waited for provider verification" >&2; exit 1;
}
i=0
while [ ! -e "$XDG_RUNTIME_DIR/vela-credential-compat.status" ] && [ "$i" -lt 200 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ "$(cat "$XDG_RUNTIME_DIR/vela-credential-compat.status")" = ok ]
i=0
while [ "$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)" -le "$targets_before" ] && [ "$i" -lt 200 ]; do
    /bin/sleep 0.01
    i=$((i + 1))
done
[ "$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)" -eq "$((targets_before + 1))" ]
sh "$hook" stop
unset MOCK_PROVIDER_WAIT_MARKER MOCK_PROVIDER_SIGNAL

# Logout/new login must serialize with an action already past token validation.
# A held provider query itself must never hold that lock (covered above).
for action_kind in target status notification; do
    export MOCK_ACTION_KIND=$action_kind
    export MOCK_ACTION_WAIT="$tmp/hold-$action_kind" MOCK_ACTION_SIGNAL="$tmp/$action_kind-ready"
    : > "$MOCK_ACTION_WAIT"
    old_done="$tmp/$action_kind-worker-done"
    if [ "$action_kind" = notification ]; then
        # The mock owner is KWallet: choosing GNOME produces the warning.
        VELA_SECRET_BACKEND=gnome
        export VELA_SECRET_BACKEND
    fi
    start_hook_worker "$old_done"
    old_worker=$worker_pid
    i=0
    while [ ! -e "$MOCK_ACTION_SIGNAL" ] && [ "$i" -lt 200 ]; do
        /bin/sleep 0.01
        i=$((i + 1))
    done
    [ -e "$MOCK_ACTION_SIGNAL" ]
    # The side effect owns the same real lock as login/logout.
    if /usr/bin/flock -n "$XDG_RUNTIME_DIR/vela-credential-compat.lock" true; then
        echo "$action_kind action was not serialized" >&2; exit 1
    fi
    stop_blocked="$tmp/$action_kind-stop-blocked"
    stop_done="$tmp/$action_kind-stop-done"
    ( MOCK_LOCK_BLOCKED="$stop_blocked" sh "$hook" stop; : > "$stop_done" ) &
    stop_pid=$!
    i=0
    while [ ! -e "$stop_blocked" ] && [ "$i" -lt 200 ]; do
        /bin/sleep 0.01
        i=$((i + 1))
    done
    [ -e "$stop_blocked" ] && [ ! -e "$stop_done" ]
    rm -f "$MOCK_ACTION_WAIT"
    wait "$stop_pid"
    wait_hook_worker "$old_worker" "$old_done"
    [ -e "$stop_done" ] && [ ! -e "$XDG_RUNTIME_DIR/vela-credential-compat.status" ]
    unset MOCK_ACTION_KIND MOCK_ACTION_WAIT MOCK_ACTION_SIGNAL
    VELA_SECRET_BACKEND=kwallet
    export VELA_SECRET_BACKEND

    # A new login may begin only after old side effects have been cleaned up.
    # Hold its provider lookup and ensure no stale status/target appears.
    export MOCK_PROVIDER_WAIT_MARKER="$tmp/new-$action_kind-provider"
    export MOCK_PROVIDER_SIGNAL="$tmp/new-$action_kind-ready"
    : > "$MOCK_PROVIDER_WAIT_MARKER"
    targets_before=$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)
    notifications_before=$(wc -l < "$MOCK_NOTIFICATION_CALLS")
    new_done="$tmp/new-$action_kind-done"
    start_hook_worker "$new_done"
    new_worker=$worker_pid
    i=0
    while [ ! -e "$MOCK_PROVIDER_SIGNAL" ] && [ "$i" -lt 200 ]; do
        /bin/sleep 0.01
        i=$((i + 1))
    done
    [ -e "$MOCK_PROVIDER_SIGNAL" ]
    [ ! -e "$XDG_RUNTIME_DIR/vela-credential-compat.status" ]
    [ "$(wc -l < "$MOCK_NOTIFICATION_CALLS")" -eq "$notifications_before" ]
    [ "$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)" -eq "$targets_before" ]
    rm -f "$MOCK_PROVIDER_WAIT_MARKER"
    wait_hook_worker "$new_worker" "$new_done"
    [ "$(cat "$XDG_RUNTIME_DIR/vela-credential-compat.status")" = ok ]
    [ "$(grep -c 'start vela-session.target' "$MOCK_SYSTEMCTL_CALLS" || :)" -eq "$((targets_before + 1))" ]
    sh "$hook" stop
    unset MOCK_PROVIDER_WAIT_MARKER MOCK_PROVIDER_SIGNAL
done

echo "credential selection, prompt/retry/logout safety, owner validation and restart lifecycle OK"
