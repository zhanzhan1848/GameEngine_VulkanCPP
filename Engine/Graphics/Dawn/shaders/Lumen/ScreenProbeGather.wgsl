// ScreenProbeGather.wgsl — Dawn port of ScreenProbeGather.comp (Vulkan GLSL)
//
// Vulkan Lumen Screen Probe GI — Gather pass (SH evaluation + probe bilateral
// gather).
//
// For each full-res pixel: reads depth/normal, finds the screen-space probe
// covering it, evaluates the already-filtered probe SH at the pixel's surface
// normal, and gathers center + 4 cardinal neighbors with bilateral weights
// (5-tap cross matching SpatialFilter's kernel) to fill sparse coverage.
//
// Dispatch: 2D, workgroups of (8, 8, 1) threads.
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  texture_2d<f32>                    depthTexture   (full-res, sampled)
//   binding 1  texture_2d<f32>                    normalTexture  (full-res, sampled, N*0.5+0.5)
//   binding 2  texture_storage_2d<rgba16float, write> outputTexture (full-res;
//                                  GLSL writeonly rgba16f image2D — WGSL has no
//                                  write-only texture_2d, storage texture is the
//                                  exact equivalent)
//   binding 3  storage, read                      probePositions (array<vec4<f32>>)
//   binding 4  storage, read                      probeSH        (array<vec4<f32>>;
//                                                                 filtered, 4 vec4/probe)
//   binding 5  uniform                            ScreenProbeGlobalData (352B)
//   binding 6  storage, read                      probeNormals   (array<vec4<f32>>)

// ============================================================================
// Struct definitions — must match C++ ScreenProbeGlobalData
// (ScreenProbeGIPass.h:52) field-for-field, std140 layout (352B). Identical
// to the struct in ScreenProbePlace.wgsl / the inline GLSL GlobalBlock.
// sdf_resolutions is uvec4 in the GLSL block — vec4<u32> here (identical
// 16B layout; reads raw uint bit patterns).
// ============================================================================

struct ScreenProbeGlobalData {
    view_projection:      mat4x4<f32>,     //   0
    inv_view_projection:  mat4x4<f32>,     //  64
    camera_position:      vec4<f32>,       // 128
    grid_params:          vec4<f32>,       // 144: x=gridW, y=gridH, z=downsample, w=raysPerProbe
    trace_params:         vec4<f32>,       // 160: x=maxRayDist, y=gatherRadius, z=renderW, w=renderH
    sdf_origins:          array<vec4<f32>, 3>,  // 176
    sdf_voxel_sizes:      array<vec4<f32>, 3>,  // 224
    sdf_extents:          array<vec4<f32>, 3>,  // 272
    sdf_resolutions:      vec4<u32>,       // 320: xyz=res0/1/2, w=cascadeCount
    surface_cache_params: vec4<f32>,       // 336: x=atlasSize, y=cardCount, z=available, w=unused
};

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var depthTexture: texture_2d<f32>;
@group(0) @binding(1) var normalTexture: texture_2d<f32>;
@group(0) @binding(2) var outputTexture: texture_storage_2d<rgba16float, write>;

@group(0) @binding(3) var<storage, read> probePositions: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> probeSH: array<vec4<f32>>;
@group(0) @binding(5) var<uniform> globalData: ScreenProbeGlobalData;
@group(0) @binding(6) var<storage, read> probeNormals: array<vec4<f32>>;

// ============================================================================
// SH basis (must match ScreenProbeAverage::shBasis):
//   Y0 = 0.282095
//   Y1 = 0.488603 * d.y
//   Y2 = 0.488603 * d.z
//   Y3 = 0.488603 * d.x
// Average stores SH coefficients C_i such that E(N) = sum C_i * Y_i(N).
// ============================================================================

fn evalSH(sh0: vec3<f32>, sh1: vec3<f32>, sh2: vec3<f32>, sh3: vec3<f32>,
          N: vec3<f32>) -> vec3<f32> {
    return sh0 * 0.282095
         + sh1 * (0.488603 * N.y)
         + sh2 * (0.488603 * N.z)
         + sh3 * (0.488603 * N.x);
}

// Manual bilinear sample replicating Metal linear sampler (clamp_to_edge).
fn sampleBilinearRGBA(tex: texture_2d<f32>, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(textureDimensions(tex));
    let px = uv * size - 0.5;
    let p0 = vec2<i32>(floor(px));
    let f = fract(px);

    let maxCoord = vec2<i32>(size) - vec2<i32>(1);
    let c00 = clamp(p0,                     vec2<i32>(0), maxCoord);
    let c10 = clamp(p0 + vec2<i32>(1, 0),   vec2<i32>(0), maxCoord);
    let c01 = clamp(p0 + vec2<i32>(0, 1),   vec2<i32>(0), maxCoord);
    let c11 = clamp(p0 + vec2<i32>(1, 1),   vec2<i32>(0), maxCoord);

    let s00 = textureLoad(tex, c00, 0);
    let s10 = textureLoad(tex, c10, 0);
    let s01 = textureLoad(tex, c01, 0);
    let s11 = textureLoad(tex, c11, 0);

    let a = mix(s00, s10, f.x);
    let b = mix(s01, s11, f.x);
    return mix(a, b, f.y);
}

// ============================================================================
// Main Kernel: screen_probe_gather
// ============================================================================

@compute @workgroup_size(8, 8, 1)
fn screen_probe_gather(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let tid: vec2<u32> = gid_vec.xy;

    let renderW: u32 = u32(globalData.trace_params.z);
    let renderH: u32 = u32(globalData.trace_params.w);

    if (tid.x >= renderW || tid.y >= renderH) {
        return;
    }

    let gridW: u32 = u32(globalData.grid_params.x);
    let gridH: u32 = u32(globalData.grid_params.y);
    let downsample: f32 = globalData.grid_params.z;

    // ---------------------------------------------------------------
    // Read pixel depth + normal
    // ---------------------------------------------------------------
    let invRes = 1.0 / vec2<f32>(f32(renderW), f32(renderH));
    let pixelUV = (vec2<f32>(f32(tid.x), f32(tid.y)) + 0.5) * invRes;

    let depth = sampleBilinearRGBA(depthTexture, pixelUV).r;

    if (depth <= 0.0001 || depth >= 0.999) {
        textureStore(outputTexture, vec2<i32>(tid), vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }

    let normalEncoded = sampleBilinearRGBA(normalTexture, pixelUV);
    let surfaceNormal = normalize(normalEncoded.xyz * 2.0 - 1.0);

    // ---------------------------------------------------------------
    // Center probe coord for this pixel
    // ---------------------------------------------------------------
    let centerProbe = vec2<i32>(i32(tid.x) / i32(downsample),
                                i32(tid.y) / i32(downsample));

    // 5-tap cross pattern: center + 4 cardinals (matches SpatialFilter kernel).
    var offsets: array<vec2<i32>, 5>;
    offsets[0] = vec2<i32>( 0,  0);
    offsets[1] = vec2<i32>(-1,  0);
    offsets[2] = vec2<i32>( 1,  0);
    offsets[3] = vec2<i32>( 0, -1);
    offsets[4] = vec2<i32>( 0,  1);

    // Spatial weights for the cross (center gets 2.0, neighbors 1.0).
    // Mirrors SpatialFilter's center-weighted blending so we don't double-smooth.
    var spatialW: array<f32, 5>;
    spatialW[0] = 2.0;
    spatialW[1] = 1.0;
    spatialW[2] = 1.0;
    spatialW[3] = 1.0;
    spatialW[4] = 1.0;

    var irradianceSum = vec3<f32>(0.0);
    var confidenceOut: f32 = 0.0;
    var weightSum: f32 = 0.0;

    for (var i: i32 = 0; i < 5; i++) {
        let npc = centerProbe + offsets[i];
        if (npc.x < 0 || npc.y < 0 ||
            npc.x >= i32(gridW) || npc.y >= i32(gridH)) {
            continue;
        }

        let probeIdx: u32 = u32(npc.y) * gridW + u32(npc.x);

        let posData = probePositions[probeIdx];
        if (posData.w <= 0.0) { continue; }

        let shBase: u32 = probeIdx * 4u;
        let sh0w = probeSH[shBase + 0u].w;
        if (sh0w <= 0.0) { continue; }

        let sh0 = probeSH[shBase + 0u].rgb;
        let sh1 = probeSH[shBase + 1u].rgb;
        let sh2 = probeSH[shBase + 2u].rgb;
        let sh3 = probeSH[shBase + 3u].rgb;

        // Proper SH evaluation at the pixel's surface normal.
        var irr = evalSH(sh0, sh1, sh2, sh3, surfaceNormal);
        irr = max(irr, vec3<f32>(0.0));

        // Reject probes whose normal faces away from the pixel's surface.
        var normalWeight: f32 = 1.0;
        let probeNormData = probeNormals[probeIdx];
        if (probeNormData.w > 0.0) {
            let probeN = normalize(probeNormData.xyz);
            let nDot = max(dot(surfaceNormal, probeN), 0.0);
            // Gentle falloff: similar-facing probes contribute fully, orthogonal
            // contribute half, opposite contribute ~0.
            normalWeight = pow(nDot, 2.0);
        }

        let w = spatialW[i] * normalWeight * sh0w;
        irradianceSum += irr * w;
        confidenceOut += sh0w * w;
        weightSum += w;
    }

    if (weightSum <= 0.0) {
        textureStore(outputTexture, vec2<i32>(tid), vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }

    let finalIrr = irradianceSum / weightSum;
    let finalConf = confidenceOut / weightSum;
    textureStore(outputTexture, vec2<i32>(tid),
                 vec4<f32>(max(finalIrr, vec3<f32>(0.0)), finalConf));
}
