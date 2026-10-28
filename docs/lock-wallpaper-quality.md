# Vela Lock wallpaper quality — diagnosis and blur validation draft

> **Status:** revised diagnosis and proposed acceptance plan only. This PR contains documentation, not functional or visual changes.
>
> **Correction (28 October 2026):** the previous report was inaccurate. The wallpaper looks sharp on the **clock screen**; the reported low-resolution / excessively soft appearance occurs **on the password-entry screen**. This changes the primary investigation target from SVG rasterization / output scaling to the login-only blur pipeline.

## Observed symptom and reproduction

1. Lock the session and stay on the clock/date screen: the wallpaper appears sharp.
2. Press a key or click to reveal the password UI: the background becomes noticeably low-detail, as though it were displayed at a reduced resolution.
3. This happens when entering **Login** mode, *before any password characters need to be typed*. Typing itself does not select a different wallpaper or apply further blur.
4. Return to the clock with Escape, or wait for the login timeout: the unblurred wallpaper is shown again.

The deliberate transition to a blurred wallpaper is expected; its **quality / appearance** is what is being reported as undesirable. This is a user observation, not yet a quantified rendering-quality regression. Confirm on a real output and distinguish deliberate defocus from blockiness, interpolation artifacts, or banding.

## Relevant code path (`lock/main.cpp`)

- `wallpaperFor(width, height)` creates a full-size `screen.background` (SVG or raster, cropped and scaled); it is used on the clock screen.
- When the output's physical dimensions change or the cached background is absent, `draw()` creates `screen.backgroundBlurred = blurred(screen.background)`. The blurred image is **cached**; it is not recomputed for each keypress.
- `const bool login = mode != Mode::Clock;` selects `screen.backgroundBlurred` for **Login** and **Checking**, and `screen.background` for **Clock**. The darker overlay also changes from alpha 60 to 110; assess it independently from blur quality.
- `blurred()` makes up to **five successive half-resolution downscales**, then smooth-upscales through the saved sizes back to the original resolution. At 2560x1440, five reductions reach 80x45 before upsampling; this necessarily loses fine detail and may make the result look like a heavily enlarged low-resolution image. A small deterministic noise pass (±2 channel levels) is then added to mitigate banding.
- The buffer is sized in physical pixels using `round(logical * scale120 / 120)`, with fractional-scale and viewporter support. Since the same output path presents a sharp clock wallpaper, incorrect compositor scaling is **not the leading explanation for this particular transition**.

The result resembles an approximate multi-scale blur (commented as dual Kawase), but repeated `QImage::scaled()` calls are **not equivalent to verifying a true dual-Kawase kernel**. Do not assume the exact cause or the ideal number of passes without experiments.

## Primary investigation: login-only blur

1. **Establish a baseline:** capture comparable output frames for Clock vs Login on the same output and wallpaper; record physical dimensions, configured scale, and the actual blur implementation/settings. Confirm whether the perceived issue is excessive defocus, coarse texture, block boundaries, ringing, or gradient banding.
2. **Compare controlled candidates:** run the existing 5-level pipeline alongside 2-, 3-, and 4-level variants at full final output resolution; consider a better-defined blur kernel / sampling strategy if all variants show objectionable artifacts. Compare perceived blur strength as well as image fidelity so that merely weakening blur is not mistaken for an algorithmic fix.
3. **Preserve UX intent:** the password screen should remain deliberately blurred and darker than the clock screen, with no readable fine-detail background distracting from the form. Avoid introducing visually abrupt or inconsistent transitions.
4. **Keep caching:** compute the blurred wallpaper only when its source, physical dimensions, scale or relevant blur parameters change; password typing and authentication state changes should reuse it. Verify redraws do not repeatedly downscale and upscale the source.
5. **Avoid regressions:** test high-frequency edges and gradients (SVG and raster), particularly blockiness and banding. Preserve deterministic output when useful for image comparisons; any noise treatment must be evaluated as part of the final pipeline.

## Acceptance and test matrix

- **Mode selection:** Clock uses the sharp image; revealing Login switches to the intended blurred image *before typing*; Login and Checking use the same consistent effect; Escape and the idle timeout restore Clock. No additional degradation after repeated keypresses or redraws.
- **Resolution / scaling:** test outputs at scale 1.0, 1.25, 1.5, 1.75 and 2.0, with single and mixed-scale multi-monitor setups. Verify both cache images have the expected physical size and are presented without accidental extra resampling.
- **Blur quality:** compare baseline (5 levels) with alternatives at matching dimensions, using a detailed SVG, a sharp photographic raster and a smooth-gradient wallpaper. Examine fine-edge bleed, coarse/pixelated appearance, banding, noise, and overall aesthetic intent; use representative cropped comparisons and documented review criteria rather than only an arbitrary numerical threshold.
- **Performance:** benchmark one-time blur generation at representative resolutions (including 1440p and 4K), memory use per output, first transition latency, and repeated-key redraw latency. Cache recomputation should occur only when necessary; never sacrifice interactive responsiveness.
- **Regression guardrails:** Clock remains as sharp as before, no new wallpaper-selection surprises, no change to authentication/PAM behavior, input handling, lock safety or ability to unlock. Test real Wayland outputs in addition to any offscreen tests.

## Secondary findings (not the cause implied by the corrected report)

The 24 October code review also noted potential separate improvements:

- `wallpaperFor()` enables `QPainter::SmoothPixmapTransform` but not `QPainter::Antialiasing` when rasterizing SVG, whereas `shell/src/wallpaperprovider.h` enables antialiasing. This may affect vector-edge quality but **does not explain why only the login wallpaper looks wrong**, given the corrected observation. Pursue independently if a measurable clock-screen issue is reproduced.
- SVG intrinsic canvas dimensions (~638x370) do not impose a bitmap-resolution ceiling on SVG rendering.
- Physical-buffer size and fractional scaling appear intentionally handled; retain scaling checks as regression coverage, not the leading root-cause hypothesis.
- Vela Lock reads `VELA_WALLPAPER` or its built-in default, while the shell uses `appearance/wallpaper` in `QSettings`; wallpaper selection consistency is separate follow-up work.
- Shared wallpaper loading / crop utilities, cache invalidation on wallpaper change, buffer lifetime, shm overflow, and monitor hotplug may deserve independent work; do not conflate these with the login-only blur issue.

## Non-goals

This draft does **not** implement or select a final blur algorithm, redesign the lock screen, change user authentication, or refactor the compositor. First verify the visual symptom and compare proposed blur implementations with reproducible evidence; then implement the smallest justified fix in a follow-up change.
