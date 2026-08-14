// ShadowBlit.wgsl — Dawn port of Nanite/ShadowBlit.metal.
//
// D32_Float depth render target → R32_Float sampleable color texture.
// WebGPU can sample depth textures directly via texture_depth_2d in compute,
// but the downstream lighting passes expect an R32_Float color input, so we
// still need this blit step.
//
// Bindings (WebGPU single namespace — matches metal's texture/buffer split):
//   0: texture_depth_2d                    — source depth (SampledDepthImage)
//   1: texture_storage_2d<r32float, write> — target depth (StorageImage)
//   2: uniform vec2<u32>                   — resolution
//
// Workgroup size: 8x8x1 (matches metal).

@group(0) @binding(0) var src_depth: texture_depth_2d;
@group(0) @binding(1) var dst_depth: texture_storage_2d<r32float, write>;
@group(0) @binding(2) var<uniform> resolution: vec2<u32>;

@compute @workgroup_size(8, 8, 1)
fn shadow_depth_blit(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= resolution.x || gid.y >= resolution.y) { return; }

    let d = textureLoad(src_depth, vec2<i32>(gid.xy), 0);
    textureStore(dst_depth, vec2<i32>(gid.xy), vec4<f32>(d, 0.0, 0.0, 0.0));
}
