# Vela in a VM: from host setup to Fedora 44 KDE with Venus

This guide prepares a Linux host, creates a QEMU/KVM VM with virt-manager,
and installs Vela in Fedora 44 KDE **without GPU passthrough**. The GPU
remains available to the host desktop. The verified setup uses an
Arch/CachyOS host with AMD RADV; the host package commands below target
those distributions. The guest uses traditional, DNF-based Fedora 44 KDE.

The Vela/Venus support described here is **experimental**, on the
[`feat/vm-venus`](https://github.com/vela-desktop/vela/tree/feat/vm-venus)
branch, in [draft PR 251](https://github.com/vela-desktop/vela/pull/251).
A regular Vela release may not include these changes. The tested
configuration and measured results are recorded in
[virtual-machines.md](virtual-machines.md#verified-guest).

Run blocks marked **HOST** on the physical computer and blocks marked
**GUEST** in a Fedora terminal inside the VM. Run commands as a normal user,
using `sudo` only where shown.

Follow the steps in order: [requirements](#2-requirements-and-reference-configuration) →
[host setup](#3-prepare-the-archcachyos-host) →
[VM creation](#4-create-and-install-fedora-44-kde) →
[Venus XML](#5-enable-venus-in-the-libvirt-definition) →
[guest graphics](#6-prepare-guest-graphics) →
[Vela installation](#7-install-vela-with-vm-support) →
[verification](#8-log-in-to-vela-and-verify-the-full-session).
The remaining sections cover [SSH](#9-optional-ssh-for-diagnostics),
[troubleshooting](#10-troubleshooting),
[rollback](#11-restore-the-previous-configuration), and
[rebuilding virglrenderer](#appendix-rebuild-virglrenderer-with-venus).

## 1. Understand the components

| Location | Component | Purpose |
|---|---|---|
| Host | Kernel with KVM and GPU driver | Runs the VM and manages the physical GPU. |
| Host | Native Vulkan driver, such as RADV | Executes the guest's requests on the GPU. |
| Host | QEMU with its OpenGL backend and virglrenderer with Venus | Exposes the virtual GPU and receives Vulkan requests. |
| Host | libvirt and virt-manager | Configure and start the VM. |
| Guest | Kernel `virtio_gpu` driver and Mesa Venus | Expose Vulkan through the virtual GPU. |
| Guest | Mesa Zink | Runs OpenGL through Vulkan for Vela's clients. |
| Guest | Vela from the specified branch | Uses the Vulkan allocator and selects Zink automatically on Venus. |

**The host does not need the Mesa Venus driver.** It uses its physical GPU's
driver; Venus is the guest's Vulkan driver. On Arch, for example,
`vulkan-radeon` provides the AMD host driver, while `vulkan-virtio` would
provide Venus for an Arch guest. In Fedora, Venus is included in
`mesa-vulkan-drivers`.

VirGL and Venus serve different purposes: selecting only “3D acceleration”
in virt-manager can give the guest OpenGL without exposing Vulkan. The
[QEMU virtio-gpu documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
explains the available backends.

## 2. Requirements and reference configuration

The host needs a CPU with virtualization enabled in firmware (AMD SVM or
Intel VT-x), a working GPU driver, and enough resources for its own desktop
as well as the VM. For this procedure, allocate 8 GiB of RAM, 4–8 vCPUs, and
at least 40 GiB of disk space to the VM, especially when building Vela.

Vela requires **Vulkan 1.4 on the guest device**, the renderer's required
[DMA-BUF/DRM extensions](renderer.md#71-minimum-requirement), and an
x86-64-v3 CPU for standard packages. The “Vulkan Instance Version” alone
does not establish that the GPU meets these requirements.

These are the versions used in the successful test, rather than minimum
version requirements:

| Component | Tested version/configuration |
|---|---|
| Host | CachyOS, kernel `7.3.3-1-cachyos` |
| Host GPU | Radeon RX 9070 XT, RADV/Mesa `26.2.6` |
| Virtualization | QEMU `11.1.3`, libvirt `12.9.0`, virglrenderer `1.3.0` |
| VM | Q35, UEFI, 8 GiB RAM, 8 vCPUs, `host-passthrough` CPU |
| Guest | Fedora 44 KDE, kernel `7.3.3-200.fc44.x86_64` |
| Guest graphics | Mesa/Venus `26.2.6`, Vulkan device API `1.4.334` |
| Vela | Changes from commit `416bd23`, Qt `6.11.2`, wlroots `0.20.2` |

For generic Venus support, QEMU documents host kernel 6.13+, virglrenderer
1.0+, and guest kernel 5.16+/Mesa 24.2+. **These minimums do not guarantee
Vulkan 1.4 or every feature Vela needs**: use an updated stack and complete
the guest checks below.
[QEMU reference](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html#virtio-gpu-virglrenderer).

For Intel Meteor Lake/the `xe` driver, or discrete GPUs paired with Intel
CPUs, Mesa lists additional requirements: kernel 6.16+, QEMU 11+, and the
KVM option `honor-guest-pat=on`. This case has not been validated with the
libvirt configuration below. Check the
[Venus host-driver notes](https://docs.mesa3d.org/drivers/venus.html#requirements)
and your libvirt version's support before proceeding; do not blindly add
a second `-accel` option to the XML.

## 3. Prepare the Arch/CachyOS host

### Install QEMU, libvirt, and tools

**HOST:**

```sh
sudo pacman -Syu --needed qemu-desktop libvirt virt-manager virt-viewer \
    edk2-ovmf dnsmasq nftables virglrenderer mesa vulkan-tools
```

Also install the Vulkan driver for the selected GPU. For AMD with RADV:

```sh
sudo pacman -S --needed vulkan-radeon
```

For Intel with ANV, the corresponding package is `vulkan-intel`. NVIDIA
requires a driver stack compatible with the GPU, kernel, and Venus; follow
the appropriate driver installation procedure. Choose the package for your
actual GPU. After a kernel or driver update, reboot the host before
continuing.

The [`qemu-desktop`](https://archlinux.org/packages/extra/x86_64/qemu-desktop/)
metapackage supplies QEMU's desktop stack. With a smaller QEMU installation,
check in particular for the `qemu-ui-opengl` and
`qemu-hw-display-virtio-gpu-gl` modules.

### Check KVM, Vulkan, and the QEMU backend

**HOST:**

```sh
uname -r
lscpu
ls -l /dev/kvm /dev/dri/renderD*
vulkaninfo --summary
qemu-system-x86_64 --version
qemu-system-x86_64 -display help
qemu-system-x86_64 -device virtio-vga-gl,help
pacman -Q qemu-desktop libvirt virglrenderer mesa
```

Check that:

- `/dev/kvm` exists. If it is missing, check SVM/VT-x in firmware and the
  kernel's `kvm_amd` or `kvm_intel` module.
- `vulkaninfo` lists the physical GPU and its native driver with **device
  API 1.4**. The host is expected to show its native driver, rather than Venus.
- QEMU offers the `egl-headless` display, the `virtio-vga-gl` device, and
  the `venus`, `blob`, and `hostmem` properties.

If the GL device or a property is missing, install/update the appropriate
QEMU packages. The `venus` property establishes QEMU-side support; it does
**not** establish that the installed virglrenderer library was built with
Venus. Check the package's build recipe for that; the complete confirmation
will be `vulkaninfo` inside the guest. If the package disables Venus, follow
the [rebuild appendix](#appendix-rebuild-virglrenderer-with-venus).

On a host with multiple GPUs, identify the selected GPU's render node:

```sh
ls -l /dev/dri/by-path/
udevadm info --query=property --name=/dev/dri/renderD128
```

Use that node in the XML below. `renderD128` is an example: numbering can
change and may identify a different GPU on another host.

### Start libvirt and the NAT network

**HOST:** first check which service model is already in use:

```sh
systemctl is-active libvirtd.service libvirtd.socket \
    virtqemud.service virtqemud.socket
```

If libvirt already works, keep its existing service model. For a fresh
Arch/CachyOS installation using the monolithic daemon:

```sh
sudo systemctl enable --now libvirtd.service
```

On a system configured for modular daemons, enable the required driver
sockets instead, without also enabling `libvirtd`:

```sh
sudo systemctl enable --now virtqemud.socket virtnetworkd.socket \
    virtstoraged.socket virtnodedevd.socket virtnwfilterd.socket \
    virtsecretd.socket virtlogd.socket virtlockd.socket
```

The [libvirt daemon documentation](https://libvirt.org/daemons.html)
describes both models and how to identify them.

```sh
sudo virt-host-validate qemu
sudo virsh -c qemu:///system list --all
sudo virsh -c qemu:///system net-list --all
```

The KVM checks must pass. An IOMMU warning alone does not prevent this
virtual-GPU setup from working.

If the `default` network exists but is inactive, start it and enable
automatic startup:

```sh
sudo virsh -c qemu:///system net-start default
sudo virsh -c qemu:///system net-autostart default
```

If it is already active, only `net-autostart` is needed. If it is absent,
create it in virt-manager under connection details → virtual networks:
name `default`, NAT mode, DHCP enabled, and an unused subnet. This procedure
does not require bridging the physical network interface.

## 4. Create and install Fedora 44 KDE

**HOST, virt-manager:**

1. Download the **Fedora KDE Plasma Desktop 44 x86_64** ISO from an
   [official Fedora source](https://fedoraproject.org/kde/download/) and verify
   its checksum as instructed by Fedora. Use the traditional DNF-based edition.
2. Open virt-manager and connect to **system QEMU/KVM** (`qemu:///system`).
   The user connection `qemu:///session` handles networking, devices, and
   permissions differently and is not the connection used here.
3. Create a VM from the ISO, select Fedora, and assign the name
   `fedora44-venus`, 8 GiB RAM, 4–8 vCPUs, and a qcow2 disk of at least 40 GiB.
4. Select “Customize configuration before install.” Use a Q35 machine,
   UEFI/OVMF firmware, Virtio disk and network devices, and the `default` NAT
   network. Expose the host CPU; its XML mode must be `host-passthrough`.
5. Virtio 2D video with a SPICE display is sufficient to install Fedora.
   The next step applies the Venus configuration. Do not add the physical
   GPU as a PCI device.
6. If absent, add a virtio channel for the QEMU guest agent named
   `org.qemu.guest_agent.0`. It helps with diagnostics and IP discovery but
   is not required for Vela rendering.
7. Install Fedora to disk, create an administrator user, and log in to KDE.
   Eject the ISO after installation.

**GUEST:** update the system, then shut it down completely:

```sh
sudo dnf upgrade --refresh
sudo systemctl poweroff
```

## 5. Enable Venus in the libvirt definition

This configuration uses **SPICE for the console** and **EGL-headless for
the host graphics context**. It was verified with libvirt 12.9/QEMU 11.1.
Native SPICE GL support requires a local UNIX connection; keeping SPICE
without GL and adding EGL-headless preserves the SPICE console used here.
[libvirt display reference](https://libvirt.org/formatdomain.html#graphical-framebuffers).

### Back up the original XML

**HOST:** confirm that the VM is shut off, then save a copy:

```sh
VELA_VM_NAME=fedora44-venus
VELA_VM_CONFIG="$HOME/vela-vm-config"
mkdir -p "$VELA_VM_CONFIG"
sudo virsh -c qemu:///system domstate "$VELA_VM_NAME"
(set -C
sudo virsh -c qemu:///system dumpxml --inactive "$VELA_VM_NAME" \
    > "$VELA_VM_CONFIG/${VELA_VM_NAME}.before-venus.xml")
cp -n "$VELA_VM_CONFIG/${VELA_VM_NAME}.before-venus.xml" \
    "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml"
nano "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml"
```

Keep the first file as a backup throughout subsequent tests: `set -C`
prevents overwriting it if it already exists. The following changes are
**fragments to merge into the existing XML**, rather than a complete
replacement VM definition. Preserve the existing UUID, firmware, disks,
network, controllers, and PCI addresses.

### Namespace, shared memory, and CPU

Add the `qemu` namespace to the root `<domain>` element, preserving any
existing attributes:

```xml
<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
```

As a direct child of `<domain>`, add or update the single `memoryBacking`
block:

```xml
<memoryBacking>
  <source type='memfd'/>
  <access mode='shared'/>
</memoryBacking>
```

Update the existing `<cpu>` block, if present:

```xml
<cpu mode='host-passthrough' check='none' migratable='off'/>
```

Here, `host-passthrough` applies to the **CPU**. It does not pass through
the GPU or add x86-64-v3 instructions that the physical CPU lacks.

### Display and video

Inside `<devices>`, keep a single SPICE display **without** its own
`<gl enable='yes'/>`, and add the EGL-headless display. For a local console,
this SPICE example listens only on localhost:

```xml
<graphics type='spice' autoport='yes'>
  <listen type='address' address='127.0.0.1'/>
  <image compression='off'/>
</graphics>
<graphics type='egl-headless'>
  <gl rendernode='/dev/dri/renderD128'/>
</graphics>
```

Replace `renderD128` if the selected host GPU uses a different render node.
Update the single `<video>` device as follows, **preserving any existing
`<address>` assignment**:

```xml
<video>
  <model type='virtio' device='virtio-vga-gl' heads='1' primary='yes' blob='on'>
    <acceleration accel3d='yes'/>
  </model>
  <alias name='ua-vela-video'/>
</video>
```

The tested libvirt version accepts the `device` attribute. If an older
schema rejects it, update libvirt or omit only `device='virtio-vga-gl'` and
check that `accel3d='yes'` still selects `virtio-vga-gl` or
`virtio-gpu-gl-pci` in the generated QEMU command. The 2D backend is
insufficient.

### Venus and hostmem properties

As a direct child of `<domain>`, **outside `<devices>`**, add:

```xml
<qemu:override>
  <qemu:device alias='ua-vela-video'>
    <qemu:frontend>
      <qemu:property name='venus' type='bool' value='true'/>
      <qemu:property name='hostmem' type='unsigned' value='4294967296'/>
    </qemu:frontend>
  </qemu:device>
</qemu:override>
```

If `qemu:override` already exists, add the device to that block. The alias
must match the video device's alias exactly. Here, `hostmem` is 4 GiB: this
is the virtual GPU's memory window, rather than the guest's total RAM or
an exclusive reservation of 4 GiB of VRAM. See the
[libvirt property-override documentation](https://libvirt.org/drvqemu.html#overriding-properties-of-qemu-devices).

### Validate and apply

**HOST, in the same terminal where the variables were defined:**

```sh
virt-xml-validate "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml" domain
sudo virsh -c qemu:///system domxml-to-native qemu-argv \
    --xml "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml" \
    > "$VELA_VM_CONFIG/${VELA_VM_NAME}.qemu-argv.txt"
cat "$VELA_VM_CONFIG/${VELA_VM_NAME}.qemu-argv.txt"
```

The generated command must include the following, possibly in JSON form:

```text
memory-backend-memfd ... share=true
-display egl-headless,rendernode=/dev/dri/renderD128
virtio-vga-gl ... blob=true ... venus=true ... hostmem=4294967296
```

Do not execute the printed QEMU command; it is used to inspect the XML
translation. If validation and inspection succeed, define and start the VM:

```sh
sudo virsh -c qemu:///system define \
    "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml"
sudo virsh -c qemu:///system start "$VELA_VM_NAME"
```

Open the console in virt-manager. Video-device changes require a
**complete VM shutdown followed by a fresh start**. Rebooting only the
guest operating system keeps the QEMU process alive and does not apply
these changes.

## 6. Prepare guest graphics

**GUEST, from KDE:**

```sh
sudo dnf install mesa-vulkan-drivers mesa-dri-drivers vulkan-tools pciutils \
    qemu-guest-agent spice-vdagent
sudo systemctl start qemu-guest-agent
uname -r
ls -l /dev/dri/
lspci -k
vulkaninfo --summary
```

The guest agent requires the channel configured in step 4. Without it, the
service may not start; this does not prevent Vulkan verification.

The expected `vulkaninfo` output includes a device resembling:

```text
deviceName = Virtio-GPU Venus (AMD Radeon RX 9070 XT ...)
driverID   = DRIVER_ID_MESA_VENUS
apiVersion = 1.4.xxx
```

The physical GPU name depends on the host. Check **that device's
apiVersion**. An additional llvmpipe device is harmless, but having only
llvmpipe/lavapipe does not satisfy this procedure. The guest must also have
a `card*` node, a `renderD*` node, and the `virtio_gpu` kernel driver.

Save the full report to check Vela's required extensions:

```sh
mkdir -p "$HOME/vela-vm-test"
vulkaninfo > "$HOME/vela-vm-test/vulkaninfo.txt" 2>&1
rpm -q mesa-vulkan-drivers mesa-dri-drivers vulkan-tools
vkcube --wsi wayland --c 120
```

The cube should be visible and complete its frames. If KDE is running an
X11 session, select “Plasma (Wayland)” at login before this test. Resolve
missing Vulkan support using the [troubleshooting section](#10-troubleshooting)
before installing Vela.

## 7. Install Vela with VM support

**GUEST:** this procedure builds the branch and installs a native RPM using
the project's updater. It does not require a GitHub account or an old CI
artifact that may have expired.

```sh
sudo dnf install git curl rpm-build dnf5-plugins
mkdir -p "$HOME/src"
cd "$HOME/src"
git clone --branch feat/vm-venus https://github.com/vela-desktop/vela.git vela-vm
cd vela-vm
sh packaging/vela-update --channel experimental \
    --source https://github.com/vela-desktop/vela.git \
    --branch feat/vm-venus --build --jobs 4
```

Run the updater in an interactive terminal. It requests `sudo` to install
dependencies with `dnf builddep` and then install the RPM. Run the script
itself as a normal user. With limited memory, reduce `--jobs` to 2. When it
finishes:

```sh
rpm -q vela-git
test -f /usr/share/wayland-sessions/vela.desktop
```

To reproduce the exact code used in the recorded test, pass
`--commit 416bd23b17fbe98d707ad1c72fdc112f50851723` to the updater **instead
of** `--branch feat/vm-venus`; never combine the two. The branch may include
later fixes.

`--branch` and `--commit` select code **for that invocation only**. While
VM support remains on this branch, update explicitly with:

```sh
vela-update --channel experimental --branch feat/vm-venus --build --jobs 4
```

An update without a selector uses the repository's default branch. A
rebuild after a Qt update must also retain the selector:

```sh
vela-update --channel experimental --branch feat/vm-venus --force --build --jobs 4
```

The general installation procedure, channels, and package policies are
documented in [installation.md](installation.md#fedora-44-and-45).

### Optional nested test

To test inside a KDE window, also build a normal development tree using
the [Fedora build dependencies](development.md#distribution-dependencies).
Then, from the branch checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
VELA_DEBUG_DMABUF=1 sh scripts/run-nested.sh
```

The script looks for binaries in `build/compositor` and `build/shell`;
installing the RPM alone does not create this tree. Close the window or
press `Alt+Shift+Esc` to end the nested session. This tests windowed
rendering, rather than the full session's DRM/KMS path.

## 8. Log in to Vela and verify the full session

**GUEST:** log out of KDE. Select the **Vela** session in the login manager
and log in with the same user. Keep KDE available as a recovery session.

On Venus, this branch automatically selects:

- the **Vulkan DMA-BUF export** allocator for compositor outputs;
- `MESA_LOADER_DRIVER_OVERRIDE=zink` for OpenGL clients, including the shell
  and Files, and propagates it to systemd --user (also used by systemd-activated
  D-Bus services). Direct dbus-daemon activation does not inherit this
  override, because its environment cannot unset it at logout; keeping Zink
  there could break a later desktop on older Mesa.

Setting `QSG_RHI_BACKEND=vulkan` or globally forcing Zink on the host or in
KDE is unnecessary. Zink is Mesa's
[OpenGL-on-Vulkan driver](https://docs.mesa3d.org/drivers/zink.html).
An existing user-defined `MESA_LOADER_DRIVER_OVERRIDE`, including an empty
value, is respected; check it if automatic selection does not occur.

From a terminal in the Vela session:

```sh
printf 'desktop=%s type=%s wayland=%s\n' \
    "$XDG_CURRENT_DESKTOP" "$XDG_SESSION_TYPE" "$WAYLAND_DISPLAY"
loginctl show-session "$XDG_SESSION_ID" -p Type -p Desktop -p Active
pgrep -a -x vela-compositor
pgrep -a -x vela-shell
grep -Ei 'drm|virtio|venus|allocator|zink|Virtual-' \
    "$HOME/.local/state/vela/vela.log"
systemctl --user show-environment | \
    grep -E '^(MESA_LOADER_DRIVER_OVERRIDE|WAYLAND_DISPLAY|XDG_CURRENT_DESKTOP)='
vulkaninfo --summary
vkcube --wsi wayland --c 120
```

The log should identify the DRM backend with the `virtio_gpu` device, a
virtual output such as `Virtual-1`, the Venus driver, and these lines:

```text
Output buffer allocator: Vulkan DMA-BUF export
Venus: OpenGL clients use Zink on Vulkan (MESA_LOADER_DRIVER_OVERRIDE=zink)
```

If the log instead shows a nested Wayland backend, the test is still
running inside a KDE window. The verified full session used
`/dev/dri/card1` and `/dev/dri/renderD128`; guest node numbering does not
need to match the host's numbering.

At minimum, test opening Files, moving and resizing windows,
maximizing/restoring, an X11 application, and the visible Vulkan cube.
Save logs before further experiments. The recorded test also passed 125%
scaling and output capture; lock/unlock, logout, video modes, and
multiple-monitor configurations still need verification.

## 9. Optional SSH for diagnostics

SSH lets you read logs even when the guest's graphical session fails to
start. With the default NAT network, the host can reach the VM's IP
directly; there is no need to expose guest port 22 to the Internet.

**GUEST, in its terminal:**

```sh
sudo dnf install openssh-server
sudo systemctl enable --now sshd
sudo firewall-cmd --permanent --add-service=ssh
sudo firewall-cmd --add-service=ssh
hostname -I
```

**HOST:** replace the example values with the guest's actual username and IP:

```sh
sudo pacman -S --needed openssh
VELA_VM_USER=username
VELA_VM_IP=192.168.122.100
install -d -m 700 "$HOME/.ssh"
ssh-keygen -t ed25519 -f "$HOME/.ssh/vela-vm" -C vela-vm
ssh-copy-id -i "$HOME/.ssh/vela-vm.pub" "$VELA_VM_USER@$VELA_VM_IP"
ssh -i "$HOME/.ssh/vela-vm" "$VELA_VM_USER@$VELA_VM_IP"
```

Choose an unused key filename and verify the host-key fingerprint on first
connection. The private key stays on the host. To find the IP through
libvirt, with the VM name set in the host terminal:

```sh
sudo virsh -c qemu:///system domifaddr "$VELA_VM_NAME" --source lease
```

Fedora's guest agent can read VM information, but SELinux may prevent it
from executing administrative commands. Use SSH or the guest terminal
for installation and configuration, keeping SELinux enabled.

## 10. Troubleshooting

| Symptom | Check and action |
|---|---|
| QEMU does not recognize `virtio-vga-gl` or offer `egl-headless` | Install the QEMU GL backend/desktop modules on the host. |
| QEMU does not recognize `venus`, `blob`, or `hostmem` | Update QEMU; check `-device virtio-vga-gl,help`. |
| The guest has VirGL OpenGL but no Venus | Check the generated QEMU command, `venus=true`, `blob=true`, `hostmem`, shared memfd RAM, and Venus support in the virglrenderer build. The 3D checkbox alone is insufficient. |
| The guest shows only llvmpipe/lavapipe | Install Mesa Vulkan in the guest, check the `virtio_gpu` kernel driver, and review the host configuration. Resolve the missing Venus device before proceeding with Vela. |
| Venus appears but its device API is below 1.4 or extensions are missing | Update the host/guest stack. A higher Vulkan loader version does not compensate for missing device capabilities. |
| SPICE rejects GL with a TCP listener | Use SPICE without GL plus EGL-headless as above, or a libvirt-supported SPICE GL configuration with a UNIX socket. |
| libvirt rejects `device` in the video model | Update the schema, or omit that attribute and check that the resulting QEMU device still uses GL. |
| QEMU cannot open the render node | Check the selected GPU, node existence, QEMU process permissions, and libvirt/SELinux logs. Specify the node in EGL-headless; avoid `chmod 666`, running QEMU as root, or disabling system protections. |
| Video does not change after rebooting Fedora | Shut down the VM, wait until libvirt reports it shut off, and start it again. |
| Vela fails the x86-64-v3 CPU check | Expose the host CPU and verify that the physical CPU has the required instructions. |
| Vela exits when the shell starts, with Venus error `invalid res_id` / `vn_GetMemoryFdPropertiesKHR` | Check the updated branch, Vulkan allocator, and automatic Zink selection. In testing, the allocator alone did not fix imports of client EGL/VirGL buffers. |
| Automatic Zink selection does not occur | Look for an existing `MESA_LOADER_DRIVER_OVERRIDE` in environment/profile files; Vela respects even an explicitly empty value. Also check for a forced `VELA_ALLOCATOR=gbm`. |
| The shell fails after a Qt update | Rebuild Vela against the guest's Qt, keeping `--branch feat/vm-venus` in the update command. |
| A black rectangle appears with KDE installed | Check `xwaylandvideobridge`: its autostart window was visible below other windows in the tested guest. See below. |

For the last case, **GUEST, in the affected session**:

```sh
systemctl --user status app-org.kde.xwaylandvideobridge@autostart.service
systemctl --user stop app-org.kde.xwaylandvideobridge@autostart.service
```

This stops the service for the current session without changing its
autostart policy for later logins. Avoid masking it globally as part of
installation; it may be needed when returning to KDE.

To investigate a compositor failure, preserve the current log and consult
the [diagnostic options](configuration.md), particularly
`VELA_DEBUG_DMABUF=1`. Do not force the Venus driver on the host with
`VK_DRIVER_FILES`: QEMU/virglrenderer must use the physical GPU's driver.

**HOST**, for VM startup failures:

```sh
sudo journalctl -b -u libvirtd -u virtqemud --no-pager -n 150
sudo tail -n 150 "/var/log/libvirt/qemu/${VELA_VM_NAME}.log"
```

**GUEST**, to collect a minimal report:

```sh
uname -a
rpm -q vela-git mesa-vulkan-drivers mesa-dri-drivers qt6-qtbase wlroots
vulkaninfo --summary
tail -n 200 "$HOME/.local/state/vela/vela.log"
```

Also include the host's kernel, QEMU, libvirt, virglrenderer, and GPU driver
versions, the Vela commit, and whether the test used a nested or DRM
session. Recorded results and unresolved observations are in
[virtual-machines.md](virtual-machines.md#verified-guest).

## 11. Restore the previous configuration

If Vela cannot start its session, return to the login manager and select
KDE. To restore the VM's video configuration, shut Fedora down completely
and use the XML backup from the host:

```sh
VELA_VM_NAME=fedora44-venus
VELA_VM_CONFIG="$HOME/vela-vm-config"
sudo virsh -c qemu:///system domstate "$VELA_VM_NAME"
sudo virsh -c qemu:///system define \
    "$VELA_VM_CONFIG/${VELA_VM_NAME}.before-venus.xml"
sudo virsh -c qemu:///system start "$VELA_VM_NAME"
```

Proceed with `define` only after the VM reports that it is shut off.
Restoring the definition does not delete the Fedora disk. If needed,
remove only the Vela package from a guest KDE terminal:

```sh
sudo dnf remove vela-git
```

Review DNF's proposed removal list before confirming.

## Appendix: rebuild virglrenderer with Venus

This step is needed **only if the host package does not enable Venus**.
The recorded test used the host's packaged virglrenderer. The rebuild
procedure below is an alternative preparation route, rather than another
VM-validated stack. Prefer an updated distribution package first.

The current upstream build enables Venus with the Meson option
`-Dvenus=true`, as documented by
[Mesa](https://docs.mesa3d.org/drivers/venus.html#vtest). Very old recipes
may use different option names; check those for the version being built.
Keep VirGL/OpenGL support enabled for the QEMU GL device used in this guide.

On Arch, rebuild the native package so that libvirt loads the library from
its expected system path. **HOST:**

```sh
sudo pacman -S --needed base-devel devtools git vulkan-headers
mkdir -p "$HOME/src/vela-vm-host"
cd "$HOME/src/vela-vm-host"
pkgctl repo clone --protocol https virglrenderer
cd virglrenderer
nano PKGBUILD
```

The command for retrieving the recipe is documented in
[`pkgctl repo clone`](https://man.archlinux.org/man/pkgctl-repo-clone.1).
In the PKGBUILD:

1. Read the recipe and check the source version.
2. Add `-Dvenus=true` to the Meson/`arch-meson` invocation or its option
   list. Replace `-Dvenus=false` if it is already present.
3. Increase `pkgrel` relative to the original recipe to distinguish the
   local package. Preserve other options and dependencies. If the recipe
   omits Vulkan dependencies required by the selected version, add them
   before building.

Build as a normal user; `makepkg` uses sudo for dependencies and installation:

```sh
makepkg --syncdeps --install
pacman -Q virglrenderer
```

On CachyOS, this may replace the distribution's optimized package with one
built from the Arch recipe. Review the version, dependencies, and
transaction before accepting. Keep the modified PKGBUILD and generated
package; a future update may replace it.

After installation, shut down and start the VM completely so the QEMU
process loads the new library. Repeat the guest Venus check. Avoid
copying `.so` files manually into `/usr/lib`, relying on a terminal's
`LD_LIBRARY_PATH` for libvirt-launched QEMU, or installing Mesa's Venus
driver on the host to fix a missing renderer.
