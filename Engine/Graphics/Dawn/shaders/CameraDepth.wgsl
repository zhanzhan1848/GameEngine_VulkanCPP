// CameraDepth.wgsl — Non-jittered depth prepass vertex shader.
// Renders opaque scene depth from camera POV without applying TAA jitter,
// so HZB/SSR/SSAO can consume a stable depth buffer.
//
// Critical: does NOT read GlobalShaderData.jitterOffset. The whole point of
// this pass is to produce depth that doesn't change per frame from jitter.

struct CameraDepthPerObject {
    worldViewProjection: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> obj: CameraDepthPerObject;

struct VSInput {
    @location(0) position: vec3<f32>,
    @location(1) _pad1: u32,
    @location(2) _pad2: u32,
    @location(3) _pad3: u32,
    @location(4) _pad4: vec2<f32>,
};

@vertex fn camera_depth_vs(input: VSInput) -> @builtin(position) vec4<f32> {
    return obj.worldViewProjection * vec4f(input.position, 1.0);
}
