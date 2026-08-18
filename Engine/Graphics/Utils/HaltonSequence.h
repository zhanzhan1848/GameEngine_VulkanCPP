#pragma once

#include "Utilities/MathTypes.h"

namespace primal::graphics::utils {

// Generate a single Halton sequence value.
// index: 1-based sample index (use i+1 for the i-th sample).
// base: prime base (2 for X, 3 for Y is the standard jitter pattern).
inline float Halton(u32 index, u32 base) {
    float f = 1.0f;
    float r = 0.0f;
    u32 i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

// Compute subpixel jitter for TAA, using a 16-sample Halton(2,3) sequence.
// Returns offset in clip space: range approximately [-1/width, 1/width] for X
// and [-1/height, 1/height] for Y, suitable for `clipPos.xy += jitter * clipPos.w`.
//
// frameIndex advances the sequence; mod 16 loops the pattern (16-sample cycle).
//
// Safe to keep jitter on for the color path: downstream passes (HZB/SSR/SSAO)
// consume a non-jittered depth from ForwardRenderer::RenderDawnDepthPrepass, so
// jitter no longer leaks into reflection ray-marching.
inline math::v2 GetJitterOffset(u32 frameIndex, u32 width, u32 height) {
    u32 i = frameIndex % 16;
    // Halton returns [0, 1); remap to [-0.5, 0.5) then scale by 1/screen —
    // the standard ±half-pixel jitter. (The old *2-1 remap produced ±1 pixel,
    // a 2× over-jitter that read as visible shimmer even with TAA resolving.)
    float x = Halton(i + 1, 2) - 0.5f;
    float y = Halton(i + 1, 3) - 0.5f;
    return { x / static_cast<float>(width), y / static_cast<float>(height) };
}

} // namespace primal::graphics::utils
