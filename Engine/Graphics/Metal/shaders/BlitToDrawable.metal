// BlitToDrawable.metal
//
// Fullscreen-triangle blit used by surface::blit_and_present (Path B).
// Renders a fullscreen triangle that samples `src` into the destination
// texture. Required because MTKView drawables are framebufferOnly and
// cannot be a BlitEncoder destination (see memory: Material Preview
// Fragment Blit). Mirrors the engine's PostProcess pattern.

#include <metal_stdlib>
using namespace metal;

struct BlitVSOut {
    float4 position [[position]];
    float2 uv;
};

// Generate a fullscreen triangle from a 0..2 vertex_id (3 verts).
// Same math as the engine's fullscreen_triangle_vs so the UV convention
// matches what every other pass expects.
vertex BlitVSOut blit_to_drawable_vs(uint vid [[vertex_id]]) {
    BlitVSOut out;
    float2 p = float2((vid << 1) & 2, vid & 2);
    out.position = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    out.uv = p;
    // NOTE: do NOT flip Y here. The engine's fullscreen_triangle_vs leaves
    // uv = p (top-left origin), and Metal's framebufferOnly drawables are
    // also top-left. Flipping would mirror the image vertically.
    return out;
}

fragment float4 blit_to_drawable_fs(BlitVSOut in [[stage_in]],
                                    texture2d<float> src [[texture(0)]],
                                    sampler smp [[sampler(0)]]) {
    return src.sample(smp, in.uv);
}
