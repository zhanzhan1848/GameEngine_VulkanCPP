// ScreenProbePlace.wgsl — Dawn port of ScreenProbePlace.comp (Vulkan GLSL)
//
// Places probes on a uniform screen-space grid. Each probe samples the
// GBuffer depth/normal at its center to determine its world position. Sky
// pixels (depth ~0 or very far) are marked inactive (w = 0).
//
// T5.3 two-level adaptive grid: checkerboard coarse tier always active;
// fine-tier cells activate only where the local block has depth/normal
// variance. Gather/SpatialFilter use a cross pattern whose neighbors of
// any (x+y odd) cell are all coarse — inactive fine cells stay covered.
//
// Dispatch: ((totalProbes + 63) / 64, 1, 1) workgroups of (64, 1, 1) threads.
// Workgroup size 64 matches ScreenProbeGIPass.cpp:658
// (pipeDesc.threadGroupSize = {64, 1, 1}) and the dispatch divisor at
// ScreenProbeGIPass.cpp:1184.
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  texture_2d<f32>         depthTexture   (texelFetch nearest)
//   binding 1  texture_2d<f32>         normalTexture  (texelFetch nearest)
//   binding 2  storage, read_write     probePositions (array<vec4<f32>>)
//   binding 3  storage, read_write     probeNormals   (array<vec4<f32>>)
//   binding 4  uniform                 ScreenProbeGlobalData (352B)

// ============================================================================
// Struct definitions — must match C++ ScreenProbeGlobalData
// (ScreenProbeGIPass.h:52) field-for-field, std140 layout.
// This Place-shader view ends at surface_cache_params (352B); the TraceRays
// variant appends ddgi_fb[2]. sdf_resolutions is uvec4 in the GLSL block —
// vec4<u32> here (identical 16B layout; reads raw uint bit patterns).
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
@group(0) @binding(2) var<storage, read_write> probePositions: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> probeNormals: array<vec4<f32>>;
@group(0) @binding(4) var<uniform> globalData: ScreenProbeGlobalData;

// ============================================================================
// Helper: reconstruct world position from depth + invViewProj
// (ported from ScreenProbePlace.metal:reconstructWorldPos)
// ============================================================================

fn reconstructWorldPos(uv: vec2<f32>, depth: f32, invVP: mat4x4<f32>) -> vec3<f32> {
    // NDC coordinates [-1, 1]
    let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    let clipPos = vec4<f32>(ndc, depth, 1.0);
    let worldPos = invVP * clipPos;
    return worldPos.xyz / worldPos.w;
}

// ============================================================================
// Main Kernel: screen_probe_place
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn screen_probe_place(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let global_id: u32 = gid_vec.x;

    let gridW: u32 = u32(globalData.grid_params.x);
    let gridH: u32 = u32(globalData.grid_params.y);
    let totalProbes: u32 = gridW * gridH;

    if (global_id >= totalProbes) {
        return;
    }

    // Compute 2D grid coordinates from 1D index
    let gx: u32 = global_id % gridW;
    let gy: u32 = global_id / gridW;

    let downsample: f32 = globalData.grid_params.z;
    let renderW: f32 = globalData.trace_params.z;
    let renderH: f32 = globalData.trace_params.w;

    let probeIdx: u32 = global_id;

    // Screen-space center of this probe
    let pixelCenter = vec2<f32>(f32(gx) * downsample + downsample * 0.5,
                                f32(gy) * downsample + downsample * 0.5);

    // UV coordinates [0, 1]
    let uv = pixelCenter / vec2<f32>(renderW, renderH);

    // Sample depth with POINT filtering — linear interpolation on depth
    // produces ghost values at object edges, creating floating probes.
    let depthTexel = vec2<i32>(clamp(uv * vec2<f32>(renderW, renderH),
                                     vec2<f32>(0.0),
                                     vec2<f32>(renderW - 1.0, renderH - 1.0)));
    let depth = textureLoad(depthTexture, depthTexel, 0).r;

    // Check for sky / far plane
    var isActive: bool = (depth > 0.0001 && depth < 0.999);

    // ------------------------------------------------------------------
    // T5.3 two-level adaptive grid: fine-tier cells activate only where
    // the local block has depth/normal variance.
    // ------------------------------------------------------------------
    if (isActive && ((gx + gy) & 1u) != 0u) {
        var nC = textureLoad(normalTexture, depthTexel, 0).rgb;
        nC = normalize(nC * 2.0 - 1.0);
        var dMin: f32 = depth;
        var dMax: f32 = depth;
        var minNDot: f32 = 1.0;
        let halfBlock = vec2<i32>(i32(downsample) / 2);
        let cardinals = array<vec2<i32>, 4>(
            depthTexel + vec2<i32>(halfBlock.x, 0),
            depthTexel - vec2<i32>(halfBlock.x, 0),
            depthTexel + vec2<i32>(0, halfBlock.y),
            depthTexel - vec2<i32>(0, halfBlock.y));
        let maxTexel = vec2<i32>(i32(renderW) - 1, i32(renderH) - 1);
        for (var i: i32 = 0; i < 4; i++) {
            let t = clamp(cardinals[i], vec2<i32>(0), maxTexel);
            let d = textureLoad(depthTexture, t, 0).r;
            dMin = min(dMin, d);
            dMax = max(dMax, d);
            var n = textureLoad(normalTexture, t, 0).rgb;
            n = normalize(n * 2.0 - 1.0);
            minNDot = min(minNDot, dot(nC, n));
        }
        // Depth range in [0,1] NDC + normal deviation drive the fine tier.
        let highVariance = (dMax - dMin > 0.0015) || (minNDot < 0.8);
        if (!highVariance) {
            isActive = false;
        }
    }

    if (!isActive) {
        // Mark as inactive: position = (0,0,0,0), normal = (0,0,0,0)
        probePositions[probeIdx] = vec4<f32>(0.0, 0.0, 0.0, 0.0);
        probeNormals[probeIdx] = vec4<f32>(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Reconstruct world position
    let worldPos = reconstructWorldPos(uv, depth, globalData.inv_view_projection);

    // Sample normal (world-space, encoded as N*0.5+0.5). Metal used a linear
    // sampler; texelFetch nearest is acceptable for normals here.
    var normal = textureLoad(normalTexture, depthTexel, 0).rgb;
    normal = normalize(normal * 2.0 - 1.0);

    // Write outputs: w = 1.0 = active flag
    probePositions[probeIdx] = vec4<f32>(worldPos, 1.0);
    probeNormals[probeIdx] = vec4<f32>(normal, 1.0);
}
