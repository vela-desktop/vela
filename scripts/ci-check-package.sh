#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
# Run only after installing vela-git in a fresh Arch container.
set -euo pipefail
fail() { printf '::error::%s\n' "$*" >&2; exit 1; }
pacman -Q vela-git
pacman -Qk vela-git
pacman -Q xorg-xwayland >/dev/null || fail "Missing Xwayland package"
test -x /usr/bin/Xwayland || fail "Missing Xwayland binary"
for program in vela-compositor vela-shell vela-files vela-settings vela-lock vela-session vela-update vela-report; do
    command -v "$program" >/dev/null || fail "Missing executable: $program"
done
for path in \
    /usr/share/wayland-sessions/vela.desktop \
    /usr/share/applications/vela-report.desktop \
    /usr/lib/vela/report/vela_report/cli.py \
    /usr/share/xdg-desktop-portal/vela-portals.conf \
    /usr/lib/systemd/user/vela-session.target \
    /usr/lib/udev/rules.d/70-vela-i2c.rules \
    /usr/lib/modules-load.d/vela-i2c.conf \
    /etc/xdg/xdg-desktop-portal-wlr/Vela \
    /etc/pam.d/vela-lock; do
    test -f "$path" || fail "Missing installed file: $path"
done
grep -Fq 'Exec=/usr/bin/vela-session' /usr/share/wayland-sessions/vela.desktop \
    || fail "Session entry does not launch /usr/bin/vela-session"
vela-report --help >/dev/null || fail "The reporting CLI cannot load its Python modules"
for helper in vela-bluetooth vela-brightness vela-polkit-agent vela-polkit-prompt vela-report-gui; do
    test -x "/usr/lib/vela/$helper" || fail "Missing helper: /usr/lib/vela/$helper"
done
# Without sd-bus the helper builds, but can't change a laptop's backlight.
ldd /usr/lib/vela/vela-brightness | grep -Fq libsystemd.so \
    || fail "vela-brightness was built without sd-bus (libsystemd)"
# vela-bluetooth is built only with libsystemd (sd-bus, sd-event).
ldd /usr/lib/vela/vela-bluetooth | grep -Fq libsystemd.so \
    || fail "vela-bluetooth doesn't link libsystemd"
for program in vela-compositor vela-shell vela-files vela-settings vela-lock \
    /usr/lib/vela/vela-bluetooth /usr/lib/vela/vela-brightness /usr/lib/vela/vela-polkit-agent /usr/lib/vela/vela-polkit-prompt \
    /usr/lib/vela/vela-report-gui; do
    path=$(command -v "$program")
    if ldd "$path" | grep -q 'not found'; then
        ldd "$path" >&2
        fail "$program has an unresolved runtime library"
    fi
done
# GTK supplies the default generic portals; wlr supplies screen capture.
for required in konsole breeze-icons xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr; do
    pacman -Q "$required" >/dev/null || fail "Missing desktop integration: $required"
done
for foreign_de in xdg-desktop-portal-kde plasma-workspace gnome-shell; do
    if pacman -Q "$foreign_de" >/dev/null 2>&1; then
        fail "Standalone install unexpectedly pulled in $foreign_de"
    fi
done
if pacman -Q base-devel >/dev/null 2>&1; then
    fail "base-devel should only be installed when a local build is requested"
fi
for executable in git curl sudo flock setsid; do
    command -v "$executable" >/dev/null || fail "Missing vela-update prerequisite: $executable"
done
if ! grep -Fxq 'default=gtk' /usr/share/xdg-desktop-portal/vela-portals.conf; then
    fail "Vela portal preferences must select GTK for generic portals"
fi
for interface in ScreenCast Screenshot; do
    if ! grep -Fxq "org.freedesktop.impl.portal.${interface}=wlr" /usr/share/xdg-desktop-portal/vela-portals.conf; then
        fail "Vela portal preferences must keep ${interface} on wlr"
    fi
done
echo "Standalone package smoke check passed; graphical login was not tested."
