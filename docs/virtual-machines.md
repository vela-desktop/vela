# Running Vela in a virtual machine

The experimental path is **QEMU/KVM + virtio-gpu + Venus**, with a Vulkan 1.4
host driver. The host GPU remains available to the host desktop; PCI GPU
passthrough is not required. A VM exposing only VirGL/OpenGL, QXL or a basic
framebuffer does not meet Vela's Vulkan requirements. There is no software
compositor fallback.

This work is an **unverified VM compatibility candidate** until the affected
guest completes the checks below. CPU tests and compilation do not demonstrate
that a Venus guest can display Vela.

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
requirements.

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
