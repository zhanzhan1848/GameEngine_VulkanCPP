// HZBCopy.wgsl — Dawn port of Nanite/HZBGeneration.metal::copy_depth_to_hzb_mip0.
//
// Copies source depth buffer into HZB mip 0 (R32_Float storage texture). The
// min() filter is applied later in HZBMip.wgsl; this stage is a 1:1 copy with
// clamp for NaN/Inf safety (mirrors metal:48-50).
//
// Bindings (WebGPU single namespace):
//   0: texture_depth_2d                    — source depth buffer (SampledDepthImage)
//   1: texture_storage_2d<r32float, write> — HZB mip 0 (StorageImage)
//
// Workgroup size: 16x16x1 (matches HZB_THREAD_GROUP_SIZE in metal:11).

@group(0) @binding(0) var src_depth: texture_depth_2d;
@group(0) @binding(1) var dst_hzb: texture_storage_2d<r32float, write>;

@compute @workgroup_size(16, 16, 1)
fn copy_depth_to_hzb_mip0(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dim = textureDimensions(src_depth);
    if (gid.x >= dim.x || gid.y >= dim.y) { return; }

    let d = textureLoad(src_depth, gid.xy, 0);
    var value = d;
    if (!(value >= 0.0 && value <= 1.0)) {
        value = 1.0;
    }
    textureStore(dst_hzb, gid.xy, vec4<f32>(value, 0.0, 0.0, 0.0));
}
