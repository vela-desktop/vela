#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Try the signed Vela Beta in a nested Wayland window without installing Vela
# on the host. See docs/distrobox.md.
set -eu

name=${VELA_DISTROBOX_NAME:-vela-nested}
image=${VELA_DISTROBOX_IMAGE:-quay.io/toolbx/arch-toolbox:latest}
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
box_home="$data_home/vela-distrobox/home"
wrapper="$HOME/.local/bin/vela-distrobox"
desktop="$data_home/applications/org.vela.Nested.Distrobox.desktop"

usage() {
    cat <<'EOF'
Usage: sh scripts/vela-distrobox.sh COMMAND [OPTION]

  setup [--nvidia]   Create an Arch Distrobox, install the signed Vela Beta,
                     and add a host launcher. --nvidia applies to new boxes.
  run                Open Vela nested inside the current Wayland desktop.
  update             Update Vela Beta inside the container.
  remove-launcher    Remove the host launcher (leave the container intact).
  help               Show this help.

Optional environment: VELA_DISTROBOX_NAME, VELA_DISTROBOX_IMAGE.
Default container: vela-nested.
EOF
}

fail() {
    printf 'vela-distrobox: %s\n' "$*" >&2
    exit 1
}

require_distrobox() {
    command -v distrobox >/dev/null 2>&1 ||
        fail "Distrobox is missing. Install Distrobox and Podman or Docker on the host."
}

validate_name() {
    case "$name" in
        ''|*[!a-zA-Z0-9_.-]*|[-.]*)
            fail "Invalid container name: $name" ;;
    esac
}

# Distrobox list is pipe-delimited: ID | NAME | STATUS | IMAGE.
# Compare full container names, not substrings.
box_exists() {
    distrobox list --no-color | awk -F '|' -v wanted="$name" '
        NR > 1 {
            box = $2
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", box)
            if (box == wanted) found = 1
        }
        END { exit(found ? 0 : 1) }
    '
}

write_launcher() {
    mkdir -p "$(dirname "$wrapper")" "$(dirname "$desktop")"
    # Atomic replacement also works when setup runs from the installed wrapper.
    tmp=$(mktemp "$(dirname "$wrapper")/.vela-distrobox.XXXXXXXX")
    cp "$0" "$tmp"
    chmod 755 "$tmp"
    mv -f "$tmp" "$wrapper"

    # Freedesktop Exec quoting preserves paths with spaces and quotes.
    escaped=$(printf '%s' "$wrapper" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\$/\\$/g')
    cat >"$desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Vela (nested)
Comment=Try Vela in a window via Distrobox
Exec="$escaped" run
Icon=preferences-system-windows
Terminal=false
Categories=System;
EOF
    printf 'Host launcher installed: %s\n' "$desktop"
    printf 'Run from a terminal: %s run\n' "$wrapper"
}

setup() {
    nvidia=0
    case "${1:-}" in
        --nvidia) nvidia=1; shift ;;
        '') ;;
        *) fail "Unknown setup option: $1" ;;
    esac
    [ "$#" -eq 0 ] || fail "Unexpected setup arguments."
    require_distrobox
    validate_name

    if box_exists; then
        printf 'Reusing existing Distrobox: %s\n' "$name"
        if [ "$nvidia" -eq 1 ]; then
            printf 'Note: --nvidia only applies when creating a new container.\n' >&2
        fi
        distrobox enter --name "$name" -- /bin/sh -c \
            '. /etc/os-release; [ "$ID" = arch ]' ||
            fail "Existing container is not Arch; choose another VELA_DISTROBOX_NAME."
    else
        umask 077
        mkdir -p "$box_home"
        printf 'Creating %s from %s\n' "$name" "$image"
        if [ "$nvidia" -eq 1 ]; then
            distrobox create --yes --name "$name" --image "$image" \
                --home "$box_home" --nvidia
        else
            distrobox create --yes --name "$name" --image "$image" \
                --home "$box_home"
        fi
    fi

    printf 'Installing prerequisites and verified Vela Beta inside %s...\n' "$name"
    # Setup needs a TTY for sudo. Only the container package manager is used.
    distrobox enter --name "$name" -- /bin/bash -lc '
        set -euo pipefail
        sudo pacman -Syu --needed --noconfirm \
            git curl base-devel cosign mesa vulkan-radeon vulkan-intel \
            vulkan-tools dbus
        checkout=$(mktemp -d)
        trap '\''rm -rf "$checkout"'\'' EXIT
        git clone --quiet --depth 1 https://github.com/vela-desktop/vela.git "$checkout/vela"
        sh "$checkout/vela/packaging/vela-update" --channel beta
        command -v vela-compositor >/dev/null
        command -v vela-shell >/dev/null
    '
    write_launcher
}

run() {
    [ "$#" -eq 0 ] || fail "The run command takes no arguments."
    require_distrobox
    validate_name
    [ -n "${WAYLAND_DISPLAY:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ] ||
        fail "A Wayland host session is required (WAYLAND_DISPLAY and XDG_RUNTIME_DIR)."
    box_exists || fail "Container $name does not exist. Run setup first."

    # Separate session bus: do not register services on the host session bus.
    # Device, system bus and audio integration are still shared with the host.
    exec distrobox enter --no-tty --name "$name" -- /usr/bin/env \
        WLR_BACKENDS=wayland \
        XKB_DEFAULT_LAYOUT="${XKB_DEFAULT_LAYOUT:-us}" \
        VELA_SCREEN_OFF=0 \
        VELA_POLKIT_AGENT=0 \
        VELA_BLUETOOTH=0 \
        VELA_UPDATE_DAEMON=0 \
        /usr/bin/dbus-run-session -- \
        /usr/bin/vela-compositor -s /usr/bin/vela-shell
}

update() {
    [ "$#" -eq 0 ] || fail "The update command takes no arguments."
    require_distrobox
    validate_name
    box_exists || fail "Container $name does not exist. Run setup first."
    distrobox enter --name "$name" -- vela-update --channel beta
}

remove_launcher() {
    [ "$#" -eq 0 ] || fail "The remove-launcher command takes no arguments."
    rm -f "$wrapper" "$desktop"
    printf 'Removed launcher; container %s was not changed.\n' "$name"
}

command=${1:-help}
[ "$#" -eq 0 ] || shift
case "$command" in
    setup) setup "$@" ;;
    run) run "$@" ;;
    update) update "$@" ;;
    remove-launcher) remove_launcher "$@" ;;
    help|-h|--help) usage ;;
    *) usage >&2; fail "Unknown command: $command" ;;
esac
