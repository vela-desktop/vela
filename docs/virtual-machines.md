# Running Vela in a virtual machine

The experimental path is **QEMU/KVM + virtio-gpu + Venus**, with a Vulkan 1.4
host driver. The host GPU remains available to the host desktop; PCI GPU
passthrough is not required. A VM exposing only VirGL/OpenGL, QXL or a basic
framebuffer does not meet Vela's Vulkan requirements. There is no software
compositor fallback.

Nested startup, shell, Files, a native Vulkan client and output capture have
been verified on one Fedora 44 KDE guest (see [validation](#verified-guest)).
Full-session DRM/KMS and the other host/driver combinations still need
verification. CPU tests and compilation alone do not demonstrate VM support.

## Configure the virtual GPU

Use the [Mesa Venus setup guide](https://docs.mesa3d.org/drivers/venus.html#qemu)
for the host-driver, kernel and QEMU requirements. The important QEMU device
options are:

```text
-device virtio-gpu-gl,hostmem=4G,blob=true,venus=true
-vga none
-display sdl,gl=on
```

These are options to incorporate into the VM's configuration, not a complete
VM launch command. Mesa's example also uses a memfd memory backend. With
libvirt/virt-manager, check the resulting QEMU configuration: selecting Virtio
video and enabling 3D acceleration alone does not establish Venus support.
On libvirt 12.9 / QEMU 11.1, the tested setup uses shared memfd RAM, Virtio
video with `accel3d='yes'` and `blob='on'`, and an `egl-headless` display
paired with SPICE. Add Venus and host memory to the **existing** video device
using a stable user alias and
[QEMU property overrides](https://libvirt.org/drvqemu.html#overriding-properties-of-qemu-devices):

```xml
<!-- Add xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0' to <domain>. -->
<memoryBacking>
  <source type='memfd'/>
  <access mode='shared'/>
</memoryBacking>
<!-- In <devices>, alongside the existing SPICE display: -->
<graphics type='egl-headless'>
  <gl rendernode='/dev/dri/renderD128'/>
</graphics>
<video>
  <model type='virtio' device='virtio-vga-gl' heads='1' primary='yes' blob='on'>
    <acceleration accel3d='yes'/>
  </model>
  <alias name='ua-venus-video'/>
</video>
<!-- Under <domain>, outside <devices>: -->
<qemu:override>
  <qemu:device alias='ua-venus-video'>
    <qemu:frontend>
      <qemu:property name='venus' type='bool' value='true'/>
      <qemu:property name='hostmem' type='unsigned' value='4294967296'/>
    </qemu:frontend>
  </qemu:device>
</qemu:override>
```

These are XML fragments to merge into the existing definition, preserving its
disk, network and PCI addresses. Choose the host's actual render node. Save a
copy of the original XML, validate the result with `virt-xml-validate`, and
fully shut down/start the guest to apply the video-device changes.
Expose the host CPU (`-cpu host` / libvirt `host-passthrough`); Vela's normal
packages require x86-64-v3. CPU host-passthrough does not pass through the GPU.

For a Fedora 44 KDE guest, install `mesa-vulkan-drivers` and `vulkan-tools`,
then run:

```sh
vulkaninfo --summary
```

Confirm a **Venus** driver/device and **device API Vulkan 1.4**, rather than
only the loader's instance version. llvmpipe/lavapipe alone is insufficient:
Vela also needs a render node and the [required DMA-BUF/DRM
extensions](renderer.md#71-minimum-requirement). If Venus is absent, correct the
host/guest graphics configuration before testing Vela.

## Build and first launch

Use this branch's [Fedora build dependencies](development.md#distribution-dependencies)
and [source build instructions](development.md#build). For a local nested
test from the checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
VELA_DEBUG_DMABUF=1 sh scripts/run-nested.sh
```

Vela detects `VK_DRIVER_ID_MESA_VENUS` and selects the Vulkan DMA-BUF export
allocator automatically. The startup log should say:

```text
Output buffer allocator: Vulkan DMA-BUF export
```

Then test a full Vela session from the guest login manager after
[installing this source build](installation.md#other-distributions). A nested
window passing does not prove virtio KMS scanout works. Keep the full session
log (`~/.local/state/vela/vela.log`), the exact Vela commit, `vulkaninfo`, guest
kernel/Mesa versions, and host QEMU/virglrenderer/driver versions with results.

## Buffer allocation and the work recovered from PR 19

[PR 19](https://github.com/vela-desktop/vela/pull/19) diagnosed a Venus abort
while importing GBM DMA-BUFs (`invalid res_id` /
`vkGetMemoryResourcePropertiesMESA resulted in CS error`). It did not contain
a demonstrated fix. Its import validation, memory-type checks and error
cleanup were already present in current main. Its opt-in trace has been
adapted here: initial/frame/scanout output commits, GBM exports, Vulkan format
queries, imports and render-target views, with FD identity to correlate buffers.

The new allocator creates an image and dedicated exportable memory on Vela's
Vulkan device, exports its DMA-BUF, and reports the actual modifier, memory
plane offsets and strides to wlroots. The renderer imports the exported
resource through its existing target cache and uses its existing fences and
queue ownership transfers. This avoids relying on GBM's choice of backend for
compositor-owned buffers; it is a candidate workaround for the reported
resource mismatch, not proof of the original driver failure's root cause.

GBM remains the default on physical GPUs. `VELA_ALLOCATOR=gbm` selects the old
path for a controlled comparison; `VELA_ALLOCATOR=vulkan` exercises the new
path on a physical GPU; `VELA_ALLOCATOR=auto` restores detection. Invalid
values stop startup with a diagnostic. Vulkan allocation requires modifiers
that support **both export and import**. The dedicated GBM cursor optimization
is not installed with the Vulkan allocator, to avoid a separate GBM import.
Client DMA-BUF imports still use the normal renderer and need guest testing.

On the tested guest, compositor-owned Vulkan buffers import successfully,
but a VirGL/EGL shell buffer aborts in Venus's `vn_GetMemoryFdPropertiesKHR`
(`vn_call_vkGetMemoryResourcePropertiesMESA`). Qt's Vulkan backend avoids that
abort; OpenGL through **Zink on Venus** also works for the shell and Files.
Vela therefore defaults `MESA_LOADER_DRIVER_OVERRIDE=zink` for processes it
launches on Venus, preserving explicit overrides, including empty values.
The full-session environment hook also passes this selection to systemd/D-Bus
activation and clears it on exit. Its existing guard leaves another running
graphical session alone. Physical GPU driver selection is unchanged.

`VELA_DEBUG_DMABUF=1` enables the trace, and `VELA_VULKAN_VALIDATION=1` enables
validation layers when installed. An unmatched `before` without `after`
locates an aborting call; it does not establish its cause. Compare one change
at a time. `VELA_DEBUG_LINEAR=1` and `WLR_DRM_NO_MODIFIERS=1` are diagnostic
comparisons, not substitutes for Venus support.

## Verification

The GPU-free `compositor.vulkan-allocator` test injects errors at image
creation, memory allocation/binding, modifier query and FD export, including
device loss. It checks multi-plane layouts, FD duplication and closure,
buffer locks, unsupported export formats and invalid metadata. The
`compositor.vulkan-device-init` test covers device initialization failures,
allocator selection for Venus and physical drivers, and import/export feature
requirements, plus the Venus client-driver default and explicit overrides.
The CPU `scripts/tests/test_session_environment.py` tests activation propagation,
cleanup and the other-desktop guard.

The functional suite's `DmabufDiagnostics` tests render and capture an output
using the forced Vulkan allocator, and check that tracing stays opt-in:

```sh
ctest --test-dir build -R 'compositor.vulkan-(allocator|device-init)' --output-on-failure
VELA_BUILD="$PWD/build" python3 tests/functional/run.py test_session.DmabufDiagnostics
```

The functional tests require a real Vulkan device and are reported as skipped
without one. They need to pass on GPU runners as well as in the target guest.
In the VM, also verify nested and full-session startup, shell and client
windows (including EGL and Vulkan clients), resizing, cursor movement,
screenshots, lock/unlock and logout. Record failures and untested cases.

## Verified guest

On 2026-11-14, the Fedora 44 KDE guest ran kernel `7.3.3-200.fc44.x86_64`,
Mesa/Venus `26.2.6`, Vulkan device API `1.4.334`, Qt `6.11.2`, and wlroots
`0.20.2`. The host used QEMU `11.1.3`, libvirt `12.9.0`, virglrenderer
`1.3.0`, kernel `7.3.3-1-cachyos`, and a Radeon RX 9070 XT / RADV `26.2.6`.
The VM used the shared GPU, without PCI GPU passthrough.

- Passed: Venus discovery, nested Vela shell, Files with Zink, native Wayland
  `vkcube` selecting Venus, window activation, and Vela's own output capture.
- Reproduced: the default VirGL/EGL shell buffer crashes the compositor inside
  Venus; the Vulkan output allocator alone does not resolve client imports.
- Also passed: the two `DmabufDiagnostics` GPU tests on the physical host with
  Vulkan validation enabled.
- Pending: full-session DRM/KMS, Xwayland, resize/scale, lock/unlock, logout,
  and the full GPU functional/sharpness suites inside the guest.

This is a recorded configuration and smoke-test result, not a compatibility
guarantee for every virtio/Venus stack.
