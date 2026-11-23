#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tools scripts/ci-beta-upgrade.py and the Beta updater need in a clean
# Arch, Fedora or Ubuntu container (CI only; runs as root).
set -eu
. /etc/os-release
case "$ID" in
arch)
    pacman -Syu --noconfirm --needed python sudo git ;;
fedora)
    dnf install -y python3 sudo git curl util-linux ;;
ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends python3 ca-certificates sudo git curl util-linux ;;
*)
    echo "ci-beta-upgrade-tools: unsupported distribution $ID" >&2
    exit 1 ;;
esac
