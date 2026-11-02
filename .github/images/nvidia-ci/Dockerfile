# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# The container of the NVIDIA GPU CI (gpu-nvidia.yml), started on Vast.ai by
# Cirun (.cirun.yml). Vast adds the NVIDIA driver, its Vulkan ICD and GBM
# backend, and /dev/dri; this image has everything else.
FROM ubuntu:26.10

ENV DEBIAN_FRONTEND=noninteractive

# Vast's start-up script runs sshd and Cirun's creates the runner's user with
# adduser and gives it sudo: the minimal Ubuntu image has none of them.
RUN apt-get update && apt-get install -y --no-install-recommends \
        adduser sudo openssh-server openssh-client ca-certificates curl wget \
        git rsync less locales tar \
    # Build dependencies, as in ubuntu.yml.
        build-essential cmake ninja-build pkgconf \
        libwlroots-0.20-dev libwayland-dev wayland-protocols libxkbcommon-dev \
        libpixman-1-dev libinput-dev libgbm-dev libdrm-dev \
        libvulkan-dev glslc libfreetype-dev libharfbuzz-dev libfontconfig-dev \
        libcairo2-dev librsvg2-dev libpam0g-dev libpolkit-agent-1-dev \
        libpolkit-gobject-1-dev zlib1g-dev libgtest-dev libsystemd-dev \
        qt6-base-dev qt6-base-private-dev qt6-declarative-dev \
        qt6-svg-dev qt6-wayland-dev qt6-tools-dev qt6-tools-dev-tools \
        qt6-l10n-tools liblayershellqtinterface-dev \
        libpipewire-0.3-dev libwireplumber-0.5-dev \
    # What the GPU suites run against.
        qt6-wayland qt6-svg-plugins layer-shell-qt breeze-icon-theme \
        qml6-module-qtquick qml6-module-qtquick-controls \
        qml6-module-qtquick-layouts qml6-module-qtqml-workerscript \
        python3 python3-numpy python3-pil python3-dbus dbus dbus-daemon libglib2.0-bin \
        xwayland x11-xserver-utils x11-utils \
    # NVIDIA's Vulkan ICD needs libXext.
        vulkan-tools vulkan-validationlayers libxext6 \
    # The workflow's options: coverage and the promo smoke test.
        gcovr ffmpeg pipewire pipewire-bin wireplumber util-linux \
    && rm -rf /var/lib/apt/lists/*
