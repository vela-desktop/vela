# Font rendering: Fontreal findings and planned improvements

**Status:** Analysis completed in November 2026; implementation and measurements pending.

This is a short guide to the font-rendering improvements we intend to pursue in Vela. It records what we learned by reviewing [Fontreal V3](https://github.com/calculatetech/fontreal) alongside Vela's current code. It is **not** a renderer design document, an implementation specification, or a claim that any proposed change has already improved image quality.

## What we analyzed

We reviewed these parts of Vela:

- [`compositor/src/text.c`](../compositor/src/text.c): FreeType rasterization, HarfBuzz shaping, grayscale glyph coverage and premultiplied blending.
- [`compositor/src/decoration.c`](../compositor/src/decoration.c): title-bar text rendered at physical output scale, and regenerated when relevant inputs change rather than once per frame.
- [`compositor/src/scene/frame.c`](../compositor/src/scene/frame.c) and [`renderer.md`](renderer.md): pixel alignment and preservation of matching surface buffers through unfiltered 1:1 composition.
- [`common/src/renderingpolicy.cpp`](../common/src/renderingpolicy.cpp) and [`common/tests/text_rendering.cpp`](../common/tests/text_rendering.cpp): Qt Quick's scalable text rendering, grayscale distance-field antialiasing, and tests for scrolling, scaling, transparency and colored fringes.

We also reviewed Fontreal's [outline sampling](https://github.com/calculatetech/fontreal/blob/main/src/fontreal_v3_outline.cpp), [coverage and tone curves](https://github.com/calculatetech/fontreal/blob/main/src/fontreal_v3_coverage.cpp), [thin-stroke treatment](https://github.com/calculatetech/fontreal/blob/main/src/fontreal_v3_thin.cpp), and [Qt Quick integration](https://github.com/calculatetech/fontreal/blob/main/integrations/qt-stock/scenegraph.cpp).

Our findings:

- Vela's compositor currently renders text using FreeType with `FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT`; it does not have Fontreal's extra 4x-per-axis outline sampling or its custom coverage correction.
- Fontreal rasterizes eligible outlines at four times the final resolution on **each axis**, derives per-pixel coverage, and applies size- and polarity-sensitive adjustments. Its additional thin-stroke handling uses hinted and, where applicable, unhinted outlines to recover details that might otherwise fade.
- Fontreal conditionally offers RGB subpixel coverage, with restrictions related to output orientation, opacity, transforms and composition. This is not a universally safe rendering mode.
- Vela's Qt Quick policy intentionally keeps the scalable distance-field renderer stable during animations and fractional scrolling. Fontreal can select native glyph rendering for treated Qt Quick text. Those approaches optimize different properties; a sharper static frame alone would not justify replacing Vela's policy.
- Vela already protects glyphs *after* rasterization through physical-pixel-aware composition. Improving the glyph mask itself is a separate opportunity.

These are **code-review findings**, not benchmark results. We have not yet established that Fontreal, stock Qt, or any proposed Vela variant is more legible under comparable conditions.

## Improvements we will implement and evaluate

We will start with Vela's **compositor-owned text** (not all text rendered by applications). This provides a limited, measurable place to experiment without changing Qt Quick, other clients, or the Vulkan compositor's surface-composition behavior.

1. **Reproducible baseline and hinting comparison.** Capture the existing FreeType Light output and compare it with other relevant FreeType hinting settings, especially Normal. Keep the font, size, positioning and text identical for each comparison.
2. **4x outline supersampling prototype.** Add an experimental grayscale glyph rasterization path that samples scalable outlines at 4x resolution on both axes and reduces the result to physical-pixel coverage. Compare it against FreeType's existing direct rasterization. Preserve HarfBuzz advances, text placement and the intended output dimensions.
3. **Perceptual coverage adjustment.** Experiment with bounded coverage/tone curves, accounting for physical font size and light-on-dark versus dark-on-light text. Evaluate whether they strengthen weak strokes without making regular text too heavy, collapsing counters, or creating halos. Treat transparent and tinted backgrounds explicitly in testing.
4. **Selective thin-stroke recovery.** If the baseline and supersampling comparisons expose lost thin strokes or diagonals, prototype a controlled comparison of hinted and unhinted outlines, inspired by Fontreal's method. Only adopt changes that preserve letter shapes and improve actual readability.
5. **Qt Quick follow-up.** After the compositor experiments, investigate whether any lessons about coverage or perceived contrast can be applied to the existing **SDF** path without compromising animation stability. This is a separate investigation, not a commitment to replace Qt's text renderer or add private Qt hooks.

We intend to **implement these experiments**, review their results, and carry forward the improvements that stand up to visual and performance validation. The experiments are planned work; this document does not mark them as completed.

## How we will judge the results

For every candidate, use the same font files and shaping inputs. Include normal, light, bold, italic and monospaced text; small UI sizes and larger control sizes; punctuation, accented characters and scripts supported by the selected fonts. Compare at 100%, 125%, 150%, 175% and 200% output scaling, with dark, light, opaque, transparent and tinted surfaces.

We will check:

- Stroke continuity, curves, diagonal edges, spacing and preservation of small counters.
- No new RGB fringes, incorrect alpha, unexpected halos, or changes to the intended text layout.
- Visual behavior at the actual display pixel size, not only zoomed screenshots.
- Rasterization time, allocations and memory usage when titles or output scale change; avoid recurring work on otherwise unchanged frames.
- Regression coverage for existing 1:1 buffer preservation and Qt Quick scrolling/scaling behavior.

We will use controlled captures and side-by-side inspection, including cases where the original renderer looks better. Sharper edges are not automatically better readability. No candidate should become the default solely because its screenshots look more contrasted.

## Boundaries we will keep

- Preserve Vela's physical-pixel positioning and unfiltered 1:1 composition when buffer geometry matches.
- Preserve Qt Quick's existing scalable, grayscale SDF policy unless a separately tested change demonstrates a net improvement. Do not switch between native and SDF rendering based on whether an animation is active.
- Do not enable LCD/RGB subpixel rendering globally: translucent, transformed, multi-monitor and rotated output cases need particular care.
- Do not modify the rendered pixels or font settings of unrelated applications from the compositor.
- Keep experiments reversible and separate from production behavior until their benefit, compatibility and cost are demonstrated.
- Respect Fontreal's authorship and licensing if any implementation code, rather than just ideas, is reused.

## Outcome of this analysis

**We have completed the comparison and identified concrete improvements to implement and test.** The first implementation work will be an isolated compositor-text comparison covering hinting, 4x supersampling and coverage adjustments. Thin-stroke recovery and any Qt Quick work will follow the evidence. We will update this guide when those experiments produce reproducible results; this PR itself changes documentation only.
