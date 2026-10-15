#version 450
// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#extension GL_GOOGLE_include_directive : require

// Live blur (§8.3): the blurred background under a panel, and a veil of noise
// (no banding in gradients). Vela's own surfaces (flags bit 3) also get
// Windows 11's acrylic recipe: a bit more saturation and a tint; other clients
// get the plain blur they asked for (docs/visual-coherence.md §6.7). The
// panel itself gives the shape: where its surface is transparent (outside the
// rounded corners) nothing is blurred. The panel is then drawn on top.
//
// binding 0: the panel surface (uv); binding 1: the blurred background, read
// at the output point: (pixel - pad) * shape.zw. color: the tint (linear,
// premultiplied: alpha = how much; none without bit 3).

#include "push.glsl"
#include "shape.glsl"

layout(set = 0, binding = 0) uniform sampler2D mask;
layout(set = 0, binding = 1) uniform sampler2D blurred;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

float noise(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
    vec3 c = texture(blurred, (gl_FragCoord.xy - pc.pad) * pc.shape.zw).rgb;
    const float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, (pc.flags & 8u) != 0u ? 1.25 : 1.0); // saturation
    // The background read already has the output's filter (if any): only the
    // tint takes it here.
    c = c * (1.0 - pc.color.a) + colorFilter(pc.color).rgb; // tint
    c += (noise(gl_FragCoord.xy) - 0.5) * (2.0 / 255.0);
    c = max(c, 0.0);
    const float coverage = smoothstep(0.0, 0.5, texture(mask, uv).a) * shapeClip() * pc.alpha;
    outColor = vec4(c, 1.0) * coverage;
}
