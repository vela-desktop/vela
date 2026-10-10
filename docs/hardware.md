# Hardware coverage and alpha limitations

Vela needs a GPU and driver exposing **Vulkan 1.4** with the dmabuf/DRM capabilities described in [renderer.md](renderer.md#71-minimum-requirement). A successful build does not guarantee that a particular GPU, driver, compositor nesting setup or display configuration works.

## Evidence, not a compatibility guarantee

| Environment | What has been established | What remains uncertain |
|---|---|---|
| AMD Radeon RX 9070 XT + Mesa/RADV, CachyOS | Primary development machine; the functional and pixel sharpness suites are exercised there | Not representative of every AMD generation or every multi-display setup |
| NVIDIA proprietary drivers | Vela has driver-specific synchronization handling and receives manual tester reports | Broader DRM, screen-sharing, suspend/resume and GPU fault-path coverage is not established |
| Intel + Mesa/ANV | Vulkan support is an intended path | No repeated automated real-hardware matrix is documented |
| NVK | Listed as an intended Vulkan driver option | Compatibility is not demonstrated by the AMD test runs |
| VMs and virtio/Venus | Useful experimental debugging environment | Vulkan version, modifiers, dmabuf import and compositor behavior depend on the VM stack |
| Hybrid and multi-GPU systems | Supported configurations are an architectural goal | Correct operation across GPU boundaries needs dedicated tests |

These are **coverage descriptions**, not a ranking of hardware or a statement that every item was individually tested in this documentation change.

## Before trying Vela

- Retain a working desktop session or TTY as a fallback. Vela is alpha software.
- Confirm Vulkan 1.4, a usable render node and compatible drivers.
- Prefer [running nested](../README.md#try-it-without-logging-out) before using Vela as your login session. Nested mode does not prove physical DRM/scanout works.
- For reporting, note distro, kernel, GPU, Vulkan driver, GPU driver version, display mode/refresh/scale, nested or DRM session and the Vela commit.
- The headless functional and sharpness tests check important invariants but do not replace observing the real monitor and driver.

## Integration services

Different features rely on installed/running system services. On Arch, see optional dependencies in [the package](../packaging/arch/PKGBUILD).

| Feature | Typically needed |
|---|---|
| Network / Wi-Fi | NetworkManager |
| Bluetooth | BlueZ |
| Audio controls and per-app volumes | PipeWire/WirePlumber with PulseAudio-compatible control tools |
| X11 apps and older games | Xwayland |
| Removable and unmounted storage | UDisks2 |
| Screen sharing / application portals | xdg-desktop-portal with the configured Wayland/KDE backends |
| KDE styling and secrets | Appropriate KDE icon/theme/wallet components |

The Vela compositor and Vulkan renderer are Vela's own; reusing these Linux/KDE services is an intentional interoperability choice rather than using KDE's compositor or shell.

## Suggested manual smoke tests

On the affected physical GPU, test login/logout, multiple displays and scaling, Alt+Tab and snapping, locking/unlocking, audio mute and network scanning, screenshots and screen sharing, device hotplug, suspend/resume and recovery from a compositor/shell crash where safe. Record observed pass/fail/untested status per test; do not infer it from a generic CI green check.

See [testing.md](testing.md) and [reporting.md](reporting.md) for automated tests and privacy-aware diagnostics.
