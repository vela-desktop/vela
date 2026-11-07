# Try Vela inside Distrobox (nested preview)

This is an **experimental preview path**, not a supported full desktop
installation. It runs Vela's own Vulkan compositor and Qt shell in a Wayland
window on an existing Linux desktop. The host distribution does not need
wlroots 0.20, Qt, or native Vela packages: an Arch Linux Distrobox provides
them. This is especially useful on immutable hosts such as SteamOS or Bazzite.

## Requirements

- A **Wayland** host session (e.g. KDE Plasma or GNOME), and Distrobox with
  Podman or Docker installed on the host.
- A GPU/driver exposing **Vulkan 1.4**, with working render-node access
  from containers. An existing Vulkan-capable host does not guarantee Vulkan
  works inside the container.
- An x86_64 host (the official Vela Beta Arch package is x86_64).
- Internet access for the container image, Arch packages and the signed Beta.

Do not use this procedure from an X11-only host session. It does not install
a display-manager session or replace the host compositor.

## Install and launch

Run these commands **on the host**, not inside a Distrobox:

```sh
git clone https://github.com/vela-desktop/vela.git
cd vela
sh scripts/vela-distrobox.sh setup
```

The setup creates a dedicated `vela-nested` Arch container with a separate
HOME at `~/.local/share/vela-distrobox/home` (or under `XDG_DATA_HOME`).
It installs Mesa Vulkan ICDs for AMD/Intel, Git, Cosign and the other
prerequisites, then uses Vela's existing `vela-update --channel beta` flow:
the prebuilt **Beta** package must pass the normal provenance verification.
It does **not** install Vela packages on the host or fall back to an unsigned
package.

Launch from the app menu (**Vela (nested)**), or run:

```sh
~/.local/bin/vela-distrobox run
```

The compositor is started with `WLR_BACKENDS=wayland`, the real
`vela-shell`, a separate session D-Bus, and without the Vela login-session
supervisor, Bluetooth/update daemons or polkit agent. Close the window or
use Alt+Shift+Esc to exit. The host normally handles Super; refer to
[the nested shortcuts](shortcuts.md) for Alt alternatives.

To update the installed Beta without touching the host packages:

```sh
~/.local/bin/vela-distrobox update
```

To remove **only** the host menu entry and wrapper:

```sh
~/.local/bin/vela-distrobox remove-launcher
```

The container is intentionally retained. To remove it separately, use
`distrobox rm vela-nested` after making sure no process is running. The
separate HOME and the optional saved launcher settings are not automatically
deleted.

### NVIDIA proprietary drivers

When creating a **new** container, use:

```sh
sh scripts/vela-distrobox.sh setup --nvidia
```

This passes Distrobox's `--nvidia` integration flag to the container.
The proprietary userspace driver must match the host's NVIDIA driver;
its presence and Vulkan compatibility still require validation. Passing
`--nvidia` when the box already exists cannot retrofit that integration.

The default container can be overridden with `VELA_DISTROBOX_NAME` (the
chosen name is saved for the launcher). `VELA_DISTROBOX_IMAGE` overrides
the Arch Toolbox image, but alternative images must still supply an Arch
userspace and pacman. The script refuses to reuse a non-Arch container.

## Troubleshooting

Check Vulkan inside the box (an ICD may be missing or incompatible):

```sh
distrobox enter --name vela-nested -- vulkaninfo --summary
```

If Vela cannot open a nested Wayland backend, ensure the host session has
`WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` set, and that the corresponding
Wayland socket is shared with Distrobox. Check `/dev/dri/renderD*` inside
the container and the host's container runtime permissions.

If the launcher was installed before switching from an X11 session, log
into a Wayland session and try again. On NVIDIA, also verify Distrobox's
driver integration. If the user session's D-Bus/portal or PipeWire services
are unavailable within the private nested bus, desktop integration features
may be missing even when the compositor works.

## What this does and does not test

Nested mode exercises actual Vela rendering, shell UI, window management,
input and applications inside a host-owned Wayland window. It **does not**
test DRM/KMS, real login/suspend behavior, direct scanout, VRR on the physical
display, or every portal/audio/system-service interaction. Performance and
input-to-display latency also include the host compositor.

**Distrobox is not a security sandbox.** It deliberately shares host
resources including devices, sockets, the system bus and potentially host
files even with a custom HOME. The private D-Bus session reduces name
collisions but is not comprehensive isolation. Vela's power-management
actions should not be used to test actual host suspend, reboot or logout.
Avoid treating this as a secure VM.

The helper does not yet have end-to-end graphical validation on SteamOS,
Bazzite, AMD, Intel and NVIDIA. Please report the host distribution,
GPU/driver and logs when testing it.

The existing development workflow, `sh scripts/run-nested.sh`, remains
the recommended way to test locally built source.
