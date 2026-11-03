# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# The container of the NVIDIA GPU CI (gpu-nvidia.yml), rented on Vast.ai by
# scripts/vast-runner.py. Vast adds the NVIDIA driver, its Vulkan ICD and GBM
# backend, and /dev/dri; this image has everything else. Arch, like the AMD
# runner and the NVIDIA laptop that came before.
FROM docker.io/library/archlinux:base-devel

RUN pacman -Syu --noconfirm --needed \
        git curl sudo \
    # Build dependencies, as in docs/development.md.
        cmake ninja pkgconf wlroots0.20 wayland wayland-protocols \
        libxkbcommon pixman libinput qt6-base qt6-declarative qt6-svg qt6-wayland qt6-tools qt6-multimedia layer-shell-qt \
        vulkan-headers vulkan-icd-loader shaderc mesa libdrm freetype2 harfbuzz fontconfig cairo \
        pam polkit librsvg zlib gtest python python-numpy python-pillow hicolor-icon-theme shared-mime-info xorg-xwayland \
        pipewire wireplumber systemd-libs \
    # What the GPU suites run against (docs/ci-quality.md).
        breeze-icons noto-fonts python-dbus python-gobject dbus glib2 xorg-xrandr xorg-xrdb xorg-xprop \
    # NVIDIA's Vulkan ICD needs libXext and libglvnd, and its EGL the external
    # platforms nvidia-utils depends on (Qt's Wayland windows need egl-wayland2
    # on recent drivers); the driver itself comes from the host.
        vulkan-tools vulkan-validation-layers libxext libglvnd egl-wayland egl-wayland2 egl-gbm egl-x11 \
    # The workflow's options: coverage and the promo smoke test.
        gcovr ffmpeg util-linux \
    # The GitHub Actions runner (.NET): its install script doesn't know pacman.
        icu krb5 openssl lttng-ust \
    && pacman -Scc --noconfirm

RUN useradd --create-home --shell /bin/bash runner \
    && echo 'runner ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/runner \
    && chmod 440 /etc/sudoers.d/runner
COPY --chmod=755 vela-ci-runner /usr/local/bin/vela-ci-runner
CMD ["/usr/local/bin/vela-ci-runner"]
