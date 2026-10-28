# Vela Lock: password-screen wallpaper blur quality

> **Status (28 October 2026): implementation included in PR #149; automated and on-device verification required before merging.**
>
> **Corrected observation:** the wallpaper appears sharp on the clock screen. The low-detail / overly enlarged appearance is confined to the **password-entry screen**, starting immediately on the transition to Login, not after typing characters.

## Root cause addressed

In `lock/main.cpp`, the Clock mode draws `screen.background`, while Login and Checking draw the cached `screen.backgroundBlurred`. The original `blurred()` downscaled the image five times (to **1/32 per dimension**, e.g. 2560x1440 → 80x45), then upscaled through all the intermediate sizes. It generated heavy deliberate defocus, but the extremely small intermediate image could give the wallpaper a coarse, low-resolution appearance. The implementation also applies a darker overlay in Login (alpha 110 versus 60 in Clock); that difference is unchanged.

The previously suspected clock-screen SVG antialiasing problem is **not implicated by the corrected reproduction**. Physical Wayland scaling and wallpaper selection remain separate possible improvements, not this fix's scope.

## Implementation

- `lock/wallpaperblur.h` supplies `vela::lock::blurWallpaper()`, called directly from the real lock drawing path.
- Downsample with two filtered half-size steps, stopping at **1/4 resolution** instead of 1/32.
- Apply **three pairs of separable horizontal and vertical box passes** (radius 6 at 1× scale, scaled proportionally and rounded for HiDPI); repeated box blurs approximate a smooth Gaussian defocus (approximately **26 logical pixels** of standard deviation at all normal Wayland scales). Clamped-edge sampling avoids wraparound/edge garbage. Processing is linear per pixel.
- Smoothly upscale **once**, to the original physical output dimensions, and keep the existing small deterministic ±2-channel-level dither to reduce banding.
- Keep the blurred image **cached per output physical size and scale120**, including scale changes with identical physical dimensions, with no blur work for password keypresses, redraws, or authentication-state changes. Preserve full-size sharp `screen.background` for Clock.
- Do not alter the darker login overlay, Wayland lock protocol, input, PAM, or security code.

This fixes the identified extreme-thumbnail reconstruction in the password-screen render path. **Visual improvement on actual hardware must still be confirmed**; blur strength is a UX judgment, not established by unit tests.

## Automated tests

`lock/tests/test_wallpaper_blur.cpp` uses the production header, and `lock/CMakeLists.txt` registers the non-GPU `lock-wallpaper-blur` CTest whenever `VELA_BUILD_TOOLS=ON` and the shell is built. It checks:

1. Original image data remains unchanged; output retains exact physical dimensions, opaque RGB32 format, and deterministic dither.
2. Uniform colors do not shift except for the documented ±2 dither, including tiny/odd-sized inputs.
3. A hard black/white edge becomes a smooth, non-blocky transition, including an explicit upper bound on positive steps.
4. Blur transition width measured in logical pixels stays consistent between 1×, 1.25×, 1.5×, 1.75× and 2×.
5. Gradients are smooth and high-frequency checkerboard texture is strongly attenuated (login blur still intentionally hides sharp details).
6. Null images are handled without crashes.

Run alongside the usual project CTest suite:

```sh
cmake --build build --target vela-lock-blur-tests
ctest --test-dir build --output-on-failure -R '^lock-wallpaper-blur$'
# Optional one-time blur timing on the current machine (1080p / 1440p / 4K):
build/lock/vela-lock-blur-tests --benchmark
```

Timing output is diagnostic only: no fragile fixed millisecond limit is enforced in CI.
The benchmark excludes wallpaper decoding and the initial SVG rendering.

## Manual visual validation before merge

- On a real Wayland session: lock, inspect the sharp clock wallpaper, press a key **without entering any password**, compare the deliberately blurred Login wallpaper, type, enter Checking, Escape/idle back to Clock. No sudden blockiness or additional degradation from typing.
- Compare representative detailed SVG, photography and smooth gradients; inspect transitions, coarse artifacts, bleeding, noise, banding and intentional blur strength.
- Check single/mixed-scale monitors at 1.0, 1.25, 1.5, 1.75 and 2.0; ensure the blurred cache and wl_shm buffer retain the correct **physical** dimensions with no extra compositor upscaling.
- Validate 1440p and 4K memory / one-time generation latency and ensure keystroke redraws do not regenerate blur.
- Verify unlock and PAM behavior unchanged.

## Out of scope

Wallpaper choice consistency with `QSettings`, shared wallpaper loaders, SVG antialiasing on the **clock** page, broader cache invalidation for live wallpaper changes, and compositor buffer-lifetime refactors are independent tasks. These should not block the narrow login-only blur improvement unless hardware testing uncovers a related regression.
