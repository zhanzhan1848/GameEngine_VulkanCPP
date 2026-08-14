// SSRPass.wgsl — Screen Space Reflection (placeholder)
// Used by ForwardRenderer's SSR pass

struct SSRParams {
    viewProj: mat4x4<f32>,
    invViewProj: mat4x4<f32>,
    screenWidth: f32,
    screenHeight: f32,
    _pad0: f32,
    _pad1: f32,
};

@group(0) @binding(0) var colorTex: texture_2d<f32>;
@group(0) @binding(1) var depthTex: texture_2d<f32>;
@group(0) @binding(2) var normalTex: texture_2d<f32>;
@group(0) @binding(3) var outputTex: texture_storage_2d<rgba32float, write>;
@group(0) @binding(4) var<uniform> params: SSRParams;

@compute @workgroup_size(8, 8, 1)
fn ssrCS(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(u32(params.screenWidth), u32(params.screenHeight));
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }
    let texCoord = vec2<u32>(gid.x, gid.y);
    let color = textureLoad(colorTex, texCoord, 0);
    textureStore(outputTex, texCoord, color);
}
