// HZBMip.wgsl — Dawn port of Nanite/HZBGeneration.metal::generate_hzb_mip_level_basic.
//
// Generates one HZB mip level from the previous mip via 2x2 MIN-depth filter.
// Min (not max) is intentional — see metal:54-81: the HZB stores the nearest
// occluder depth per region; anything farther is conservatively occluded.
//
// Bindings (WebGPU single namespace):
//   0: texture_2d<f32>                     — source mip (R32_Float color view)
//   1: texture_storage_2d<r32float, write> — target mip (StorageImage)
//
// Workgroup size: 16x16x1.

@group(0) @binding(0) var src_mip: texture_2d<f32>;
@group(0) @binding(1) var dst_mip: texture_storage_2d<r32float, write>;

@compute @workgroup_size(16, 16, 1)
fn generate_hzb_mip_level(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dst_dim = textureDimensions(dst_mip);
    if (gid.x >= dst_dim.x || gid.y >= dst_dim.y) { return; }

    let src_dim = textureDimensions(src_mip, 0);
    let src_base = gid.xy * 2u;
    let src_max = vec2<u32>(
        select(0u, src_dim.x - 1u, src_dim.x > 0u),
        select(0u, src_dim.y - 1u, src_dim.y > 0u)
    );

    let d00 = textureLoad(src_mip, min(src_base,                   src_max), 0).r;
    let d10 = textureLoad(src_mip, min(src_base + vec2<u32>(1u, 0u), src_max), 0).r;
    let d01 = textureLoad(src_mip, min(src_base + vec2<u32>(0u, 1u), src_max), 0).r;
    let d11 = textureLoad(src_mip, min(src_base + vec2<u32>(1u, 1u), src_max), 0).r;

    let min_d = min(min(d00, d10), min(d01, d11));
    textureStore(dst_mip, gid.xy, vec4<f32>(min_d, 0.0, 0.0, 0.0));
}
