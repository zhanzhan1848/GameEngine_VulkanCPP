// HZBGeneration.wgsl — Hierarchical Z-Buffer generation
// Two compute entry points:
//   copy_depth_to_hzb_mip0: copies depth buffer to HZB mip 0
//   generate_hzb_mip_level: generates one mip levels via 2x2 min-depth filter

struct HZBCopyParams {
    width: u32,
    height: u32,
    _pad0: u32,
    _pad1: u32,
};

struct HZBMipParams {
    srcWidth: u32,
    srcHeight: u32,
    dstWidth: u32,
    dstHeight: u32,
};

// --- Copy depth to HZB mip 0 ---
@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var hzbOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> copyParams: HZBCopyParams;

@compute @workgroup_size(8, 8, 1)
fn copy_depth_to_hzb_mip0(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= copyParams.width || gid.y >= copyParams.height) { return; }

    let depth = textureLoad(depthTex, gid.xy, 0);
    let d = clamp(depth, 0.0, 1.0);
    textureStore(hzbOutput, gid.xy, vec4f(d, 0.0, 0.0, 0.0));
}

// --- Generate HZB mip level (2x2 min-depth) ---
@group(0) @binding(0) var hzbSource: texture_2d<f32>;
@group(0) @binding(1) var hzbMipDest: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> mipParams: HZBMipParams;

@compute @workgroup_size(8, 8, 1)
fn generate_hzb_mip_level(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= mipParams.dstWidth || gid.y >= mipParams.dstHeight) { return; }

    let srcCoords = gid.xy * 2u;
    let srcMax = vec2u(mipParams.srcWidth - 1u, mipParams.srcHeight - 1u);

    let d00 = textureLoad(hzbSource, min(srcCoords, srcMax), 0).r;
    let d10 = textureLoad(hzbSource, min(srcCoords + vec2u(1u, 0u), srcMax), 0).r;
    let d01 = textureLoad(hzbSource, min(srcCoords + vec2u(0u, 1u), srcMax), 0).r;
    let d11 = textureLoad(hzbSource, min(srcCoords + vec2u(1u, 1u), srcMax), 0).r;

    let minDepth = min(min(d00, d10), min(d01, d11));
    textureStore(hzbMipDest, gid.xy, vec4f(minDepth, 0.0, 0.0, 0.0));
}
