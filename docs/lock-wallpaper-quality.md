# Vela Lock wallpaper sharpness — design / investigation draft

> **Status:** proposed diagnosis and acceptance plan only. No visual or functional changes in this PR.

## User-visible regression

Vela Lock's wallpaper appears very low resolution **already on the clock screen, before password entry**. Therefore the login-only `blurred()` implementation is not sufficient to explain the primary symptom. On 24 October the earlier investigation identified a concrete mismatch in SVG rasterization, plus scaling and wallpaper selection questions that still need instrumental verification.

## Existing implementation

- `lock/main.cpp:wallpaperFor()` draws the default SVG `vela_splash_169.svg` with `QSvgRenderer` using only `QPainter::SmoothPixmapTransform`; `shell/src/wallpaperprovider.h:renderSvg()` enables `QPainter::Antialiasing`. SmoothPixmapTransform controls image resampling, not SVG vector-edge antialiasing. Missing vector antialiasing is a plausible cause of jagged/pixelated edges at the clock screen.
- The SVG's modest **intrinsic canvas** (~638x370) is not itself a bitmap-resolution limit: a valid SVG can be rasterized at full output resolution. Test rather than attributing blur to the SVG canvas size.
- `draw()` calculates a physical buffer size `round(logical * scale120 / 120)` and uses `fractional-scale-v1` + viewporter. This *appears designed* to avoid compositor upscaling but must be verified on real outputs; e.g. a physical 2560x1440 output at 150% corresponds to about 1707x960 logical, and should render ~2560x1440 physical, **not** 1707x960 later stretched.
- The final compositor-facing `QPainter` sets antialiasing, but it cannot reconstruct details already omitted by `wallpaperFor()`.
- `wallpaperFor()` reads `VELA_WALLPAPER` or the built-in default, while the shell reads `appearance/wallpaper` from `QSettings`. The lock image may not even be the same asset chosen for the desktop.
- The password screen uses `backgroundBlurred`; its multi-stage blur must be assessed *separately* from the pre-login artifact.

## Candidate fixes (not yet chosen)

1. At minimum enable `QPainter::Antialiasing | QPainter::SmoothPixmapTransform` on the painter used to rasterize SVG into `screen.background`.
2. Prefer a shared wallpaper loading/rasterization/crop helper between the shell and Vela Lock rather than two subtly different SVG paths. Account for Qt resource availability in the lock binary and the correct QSettings scope.
3. Add optional diagnostics for logical output size, `scale120`, allocated wl_shm buffer dimensions, viewport destination and actual asset path. Log only when enabled.
4. Recompute the cached wallpaper when output geometry, scale, or selected wallpaper changes. Audit buffer release, shm allocation/size overflow and reconfiguration/monitor hotplug separately.
5. Establish pixel-based comparisons against an expected offscreen rasterization, not screenshots judged solely by visual impression.

## Acceptance / test matrix

- Clock screen before any password input: sharp SVG edges at scale 1.0, 1.25, 1.5, 1.75 and 2.0. Include multi-output with differing scales and a reconfigure.
- Check buffer size in **physical pixels** and 1:1 presentation; distinguish antialiasing from unintended resampling.
- Compare Lock vs shell for same SVG and a high-resolution raster wallpaper, including equivalent crop and the wallpaper set in Settings.
- Login screen blur stays intentional; neither the clock screen nor the unblurred asset should be pre-blurred.
- Test on actual Wayland DRM output, as headless rendering alone does not guarantee scanout sharpness.

## Non-goals

No redesign of lock UI or changes to PAM/lock security behavior. First establish the root cause and regressions, then implement a narrowly scoped fix.
