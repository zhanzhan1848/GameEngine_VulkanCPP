// ShadowDepth.wgsl — Depth-only shadow pass vertex shader
// Renders scene geometry from directional light's perspective into a D32_Float depth texture.
// No pixel shader (depth-only pass), front-face culling to reduce shadow acne.

struct ShadowPerObject {
    world: mat4x4<f32>,
    worldLightVP: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> obj: ShadowPerObject;

struct VSInput {
    @location(0) position: vec3<f32>,
    @location(1) _pad1: u32,
    @location(2) _pad2: u32,
    @location(3) _pad3: u32,
    @location(4) _pad4: vec2<f32>,
};

@vertex fn shadow_vs(input: VSInput) -> @builtin(position) vec4<f32> {
    return obj.worldLightVP * vec4f(input.position, 1.0);
}
