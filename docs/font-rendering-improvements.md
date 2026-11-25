# Font rendering: Fontreal findings and the title bar's text

> **Status:** Proposed. **Checked:** 25 November 2026, `6127a13`.
>
> **Remaining:** coverage adjusted for linear-light composition, the hinting comparison, thin-stroke recovery and the Qt Quick follow-up are not started ([what comes next](#what-comes-next)). The title text changes in [What changed](#what-changed) still need the real-session check in [testing](testing.md#compositor-text).

This guide records what reviewing [Fontreal V3](https://github.com/calculatetech/fontreal/tree/593ffaf36eb45392743d9a5034e09391a6281f94) (MIT, commit `593ffaf`, 21 November 2026) against Vela's code taught us. It covers what the review found, the changes it led to in the compositor's title text, with measurements, and what is still open. It is not a renderer design: [renderer.md](renderer.md) remains the reference for Qt Quick's text policy (§3) and the title bar (§9).

## Scope

Two kinds of text reach the screen in Vela:

- **Compositor text**: window titles, drawn by [`compositor/src/text.c`](../compositor/src/text.c) for [`decoration.c`](../compositor/src/decoration.c). The compositor owns this text, so this is where we can experiment.
- **Qt Quick text**: the shell and Vela's apps, under the policy in [`common/src/renderingpolicy.cpp`](../common/src/renderingpolicy.cpp) (scalable distance-field glyphs, grayscale antialiasing; tested by `text-rendering-rhi`). That policy is unchanged.

When [renderer.md](renderer.md) says "do not add a custom compositor text pipeline", it means routing Qt Quick text through the compositor. Title text has always been the compositor's own.

## Findings

### Vela's title text

- FreeType renders grayscale coverage with `FT_LOAD_TARGET_LIGHT`, which hints vertically only. HarfBuzz lays the line out with unhinted, fractional advances (hb-ft's default load flags).
- The title image is composited in **linear light**. `texture.frag` un-premultiplies, decodes sRGB and premultiplies again, and the target view is `_SRGB` ([renderer.md §7.5](renderer.md)). So FreeType's area coverage acts as a linear-light alpha. Most compositors blend sRGB-encoded values instead, and most font tuning assumes that. The difference is large at the title bar's colors. A pixel half covered by the light palette's `#1a1a1a` on `#f3f3f3` comes out as 179/255 in linear light, against 135 when blended in sRGB values: dark text looks lighter and thinner. A pixel half covered by the dark palette's white on `#202020` comes out as 189 in linear light, against 144: light text looks heavier.
- Before the changes below:
  - every glyph was moved to the nearest whole pixel;
  - the image was exactly the line box, so ink outside it was cut off;
  - 1-bit bitmaps were read as 8-bit;
  - the title was drawn again on every width change.

  [What changed](#what-changed) measures each of these.
- One font face, with no fallback: characters the face lacks (emoji, CJK, most non-Latin scripts in a Latin font) show as missing-glyph boxes. Color glyphs are not drawn.

### Fontreal

- **4x outline sampling.** [`sampleOutline`](https://github.com/calculatetech/fontreal/blob/593ffaf36eb45392743d9a5034e09391a6281f94/src/fontreal_v3_outline.cpp) rasterizes the outline with FreeType's antialiasing at four times the size on each axis. Its grayscale value, `center()`, is the mean of each 4×4 block, and FreeType's coverage is already an exact area at 1x. We measured both on the same light-hinted outlines: 65 Latin letters, digits and symbols, 13.3 to 30 px, Noto Sans Regular and Italic and DejaVu Sans, FreeType 2.15.0. The mean difference per inked pixel was 1.4–1.5/255, total ink differed by 0.07–0.14%, and the largest difference was 22/255, on curved edges, where FreeType flattens Béziers more coarsely at 1x. In Fontreal the 4x buffer feeds the RGB stripes (`stripes()`) and the thin-stroke analysis. On its own in grayscale it is not a visible change, and it costs 16 times the samples.
- **Coverage and tone curves.** [`toneCurve`](https://github.com/calculatetech/fontreal/blob/593ffaf36eb45392743d9a5034e09391a6281f94/src/fontreal_v3_coverage.cpp) assumes sRGB-encoded blending. `darkInkCore` moves dark ink part of the way toward its linear-light value with `encodeSrgb`, and `lightInkLift` raises light ink. Vela already composites in linear light, so these tables push the wrong way here. Dark ink would need more coverage, not less. Applied on top of Vela's blending, light ink would be lifted twice.
- **Thin strokes.** [`fontreal_v3_thin.cpp`](https://github.com/calculatetech/fontreal/blob/593ffaf36eb45392743d9a5034e09391a6281f94/src/fontreal_v3_thin.cpp) compares hinted and unhinted outlines, sampled at 4x, to restore faint stems and diagonals at small sizes.
- **Subpixel positioning.** Fontreal rasterizes each glyph at its fractional phase (`outlineMask`, [`scenegraph.cpp`](https://github.com/calculatetech/fontreal/blob/593ffaf36eb45392743d9a5034e09391a6281f94/integrations/qt-stock/scenegraph.cpp)). Vela has adopted this technique.
- **Qt Quick integration.** It forces `NativeRendering` for the text it treats and replaces `QSGDefaultContext::createGlyphNode` through its mangled symbol and private Qt headers. RGB stripes are used only on native Wayland surfaces with an opaque clear color. Native rendering conflicts with Vela's policy, which keeps one scalable renderer at rest and in motion, and Vela does not use private Qt hooks.

These are findings from reading the code. The numbers are measurements of rasterizer output. They are not legibility judgments.

## What changed

All of these are in the compositor's title text. Hinting, grayscale antialiasing, colors, blending, vertical centering and the line box's position are unchanged.

| Change | Before | After | Checked by |
|---|---|---|---|
| Glyphs at HarfBuzz's exact horizontal position: whole pixels, plus the outline moved by the 26.6 remainder. Light hinting doesn't move points horizontally, so nothing is lost. | Position error up to 0.500 px, RMS 0.290 px | Up to 0.021 px, RMS 0.006 px, which is the measurement's own precision | `compositor.text` |
| No ink cut off: the image covers the glyphs' ink, up to an em (plus a pixel) outside the line box. That bound stops a title stacking marks (which an app can send) from asking for a large image. | 89 of 216 glyph renders cut, 1.75% of their ink lost; a title starting with "j" loses the tail | 0 cut | `compositor.text` |
| The line box keeps its pixels: the image grows left and up by whole logical units (`vela_whole_pixel_units`), never by fractions. | — | At 125, 150, 175 and 225%, a fractional offset would land the line box a pixel off at 2.5–5.6% of window positions. Whole units: none. | `compositor.Geometry.WholePixelUnitsKeepTheLineBoxOnItsPixels`; `functional-decorations` (`TitleLettersStayPut`, `BarImagesAtEveryPosition`) |
| Bitmaps with fewer than 8 bits per pixel (bitmap fonts, embedded strikes) converted before they are read | A 1-bit glyph: 20 bytes read from a 13-byte bitmap, so wrong pixels and a heap over-read | Exactly the set pixels | `compositor.text` |
| The title is drawn again on a new width only if it would be cut at another glyph (`vela_text_fits`) | Narrowing to 1 px and widening back, at 6 scales: 15,440 drawings. With room for the title: 21,600 | 1,230, one per distinct image. With room: none | `compositor.text`; `functional-decorations` (`TitleFollowsTheWidth`) |

The test compares each change with the old renderer, which it keeps line for line as `legacy_render`. That copy was checked byte for byte against the old `text.c` on 11,136 renders. The old renderer must show each defect, so the measurement can see it, and the new one must beat it by a wide margin. The test fonts are subsets of Noto Sans 2.015, KDE's default font, kept with their hinting in [`compositor/tests/fonts`](../compositor/tests/fonts) (OFL-1.1). Every machine therefore measures the same font.

## What comes next

In order. A glyph cache is a prerequisite for anything that costs more per glyph than FreeType's direct rasterization.

1. **Coverage for linear-light composition.** Try FreeType's stem darkening first. It is designed for linear blending, but it applies only where the autofitter or the CFF driver hints the font, so check which driver hints the user's font. Then try bounded curves derived for Vela's blending, not Fontreal's tables. The criteria have to be stated before measuring: comparable stroke darkness in both palettes, and the title's weight next to the Qt text right under it ([visual coherence](visual-coherence.md): the bar and the app's content are seen together).
2. **Hinting.** Compare Light with the hinting the user chose in fontconfig/KDE, which `text.c` ignores today. A comparison with Normal must also set HarfBuzz's load flags (`hb_ft_font_set_load_flags`). Otherwise hinted glyph widths no longer match unhinted advances, and the spacing measurement fails for reasons unrelated to quality.
3. **Thin-stroke recovery**, with 4x sampling as its infrastructure, only if small sizes show lost stems or diagonals.
4. **Qt Quick**: whether anything learned about coverage applies to the distance-field path without touching its stability in motion. This is a separate investigation, with no private Qt hooks.
5. **Font fallback**, which is separate work: choosing faces through fontconfig per run of characters, and color glyphs.

## How to judge a change

- Measure before and after on the same inputs with an objective metric, as `compositor.text` does. Include a negative control showing that the old renderer has the defect.
- Use the bundled fonts for automated checks, adding a subset when a case needs another style ([how they were made](../compositor/tests/fonts/README.md)). Cover regular, italic and bold; small and large sizes; accented letters and stacked marks; 100, 125, 150, 175, 200 and 225%; both palettes, active and inactive; opaque and Mica backgrounds.
- Look at the result at the display's real pixel size, next to Qt text, including cases where the old renderer looks better. A sharper or darker screenshot is not, by itself, better readability.
- Keep the existing guarantees: the 1:1 copy at every position (`functional-decorations`, `sharpness`) and Qt Quick's continuity tests (`text-rendering-rhi`).

## Boundaries

- Physical-pixel positioning and the unfiltered 1:1 copy stay.
- No LCD/RGB subpixel rendering: the bar is translucent, animated, moves between monitors and rotates.
- Never switch between native and distance-field rendering on whether something moves.
- The compositor doesn't change other applications' pixels or font settings.
- Code taken from Fontreal, rather than ideas, keeps its MIT copyright and permission notice.
