#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Reports whether this machine could run Vela's GPU suites on NVIDIA:
# the proprietary driver with Vulkan >= 1.4, an NVIDIA render node and GBM,
# first on the host, then in an Arch container (Vela's build needs newer
# packages than the Vast.ai VM images have). Never stops at the first
# failure: the summary at the end is the result.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
summary=()
note() { summary+=("$1"); }
section() { printf '\n===== %s =====\n' "$1"; }

if [[ $(id -u) -eq 0 ]]; then sudo=(); else sudo=(sudo -n); fi

section "Machine"
. /etc/os-release && echo "OS: $PRETTY_NAME"
echo "Kernel: $(uname -r)"
echo "CPUs: $(nproc)"
free -g | sed -n 2p
df -h / | tail -1
id
"${sudo[@]}" true && echo "sudo: ok" || echo "sudo: not available"

section "NVIDIA driver"
lspci -nn | grep -i nvidia || true
if nvidia-smi --query-gpu=name,driver_version --format=csv,noheader; then
    note "driver: $(nvidia-smi --query-gpu=name,driver_version --format=csv,noheader | head -1)"
else
    note "driver: nvidia-smi FAILED"
fi
lsmod | grep -E '^nvidia' || true
echo "nvidia_drm modeset: $(cat /sys/module/nvidia_drm/parameters/modeset 2>/dev/null || echo 'module not loaded')"
dpkg -l 2>/dev/null | awk '/nvidia|libnvidia/ {print $2, $3}' | head -40

section "Render nodes"
ls -l /dev/dri 2>/dev/null || echo "no /dev/dri"
render_node=
for node in /dev/dri/renderD*; do
    [[ -e $node ]] || continue
    vendor="$(cat "/sys/class/drm/$(basename "$node")/device/vendor" 2>/dev/null)"
    echo "$node vendor=$vendor"
    [[ $vendor == 0x10de && -z $render_node ]] && render_node="$node"
done
if [[ -n $render_node ]]; then
    note "host render node: $render_node (NVIDIA)"
else
    note "host render node: NONE for NVIDIA"
fi

section "Vulkan ICDs and GBM backends on the host"
ls -l /usr/share/vulkan/icd.d /etc/vulkan/icd.d 2>/dev/null
ls -l /usr/lib/x86_64-linux-gnu/gbm 2>/dev/null || echo "no GBM backend directory"

section "Host packages"
export DEBIAN_FRONTEND=noninteractive
"${sudo[@]}" apt-get update -qq
"${sudo[@]}" apt-get install -y -qq vulkan-tools libgbm-dev gcc pciutils >/dev/null \
    && echo "installed vulkan-tools libgbm-dev gcc" || echo "apt-get install FAILED"

section "Host Vulkan"
vulkaninfo --summary 2>&1 | tee /tmp/vk-host.txt | head -80
if grep -q 'DRIVER_ID_NVIDIA_PROPRIETARY' /tmp/vk-host.txt; then
    note "host Vulkan: $(grep -m1 -E '^\s*apiVersion' /tmp/vk-host.txt | xargs) NVIDIA proprietary"
else
    note "host Vulkan: no NVIDIA proprietary device"
fi

section "Host GBM"
if [[ -n $render_node ]] && gcc -O2 -o /tmp/gbm-probe "$here/gbm-probe.c" -lgbm; then
    /tmp/gbm-probe "$render_node" && note "host GBM: PASS" || note "host GBM: FAIL"
else
    note "host GBM: not run"
fi

section "Container runtime"
if ! command -v docker >/dev/null; then
    "${sudo[@]}" apt-get install -y -qq docker.io >/dev/null || echo "docker.io install FAILED"
fi
if ! command -v nvidia-ctk >/dev/null; then
    curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
        | "${sudo[@]}" gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
    curl -fsSL https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
        | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#' \
        | "${sudo[@]}" tee /etc/apt/sources.list.d/nvidia-container-toolkit.list >/dev/null
    "${sudo[@]}" apt-get update -qq
    "${sudo[@]}" apt-get install -y -qq nvidia-container-toolkit >/dev/null || echo "toolkit install FAILED"
fi
docker --version || true
nvidia-ctk --version || true
"${sudo[@]}" nvidia-ctk runtime configure --runtime=docker >/dev/null && "${sudo[@]}" systemctl restart docker

section "Arch container"
devices=()
for node in /dev/dri/*; do [[ -e $node ]] && devices+=(--device "$node"); done
if "${sudo[@]}" docker run --rm --runtime=nvidia --gpus all \
        -e NVIDIA_DRIVER_CAPABILITIES=all "${devices[@]}" \
        -v "$here:/probe:ro" docker.io/library/archlinux:latest bash -c '
            set -u
            pacman -Sy --noconfirm --needed vulkan-tools gcc mesa >/dev/null 2>&1 || echo "pacman FAILED"
            ls -l /dev/dri
            ls -l /usr/share/vulkan/icd.d /etc/vulkan/icd.d 2>/dev/null
            ls -l /usr/lib/gbm 2>/dev/null || echo "no GBM backend directory"
            vulkaninfo --summary 2>&1 | head -60
            if vulkaninfo --summary 2>/dev/null | grep -q DRIVER_ID_NVIDIA_PROPRIETARY; then
                echo "RESULT container Vulkan: NVIDIA proprietary"
            else
                echo "RESULT container Vulkan: no NVIDIA proprietary device"
            fi
            node=
            for n in /dev/dri/renderD*; do
                [[ $(cat /sys/class/drm/$(basename $n)/device/vendor 2>/dev/null) == 0x10de ]] && { node=$n; break; }
            done
            if [[ -n $node ]] && gcc -O2 -o /tmp/gbm-probe /probe/gbm-probe.c -lgbm; then
                /tmp/gbm-probe "$node" && echo "RESULT container GBM: PASS" || echo "RESULT container GBM: FAIL"
            else
                echo "RESULT container GBM: not run (render node: ${node:-none})"
            fi
        ' 2>&1 | tee /tmp/container.txt; then
    while IFS= read -r line; do note "${line#RESULT }"; done < <(grep '^RESULT ' /tmp/container.txt)
else
    note "container: docker run FAILED"
fi

section "Summary"
printf '%s\n' "${summary[@]}" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
