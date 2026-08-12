// DDGIUpdateDepth.wgsl — Dawn port of DDGIUpdateDepth.metal
//
// Bins ray hit distances for one probe into 64 (8x8) octahedral texels,
// computes mean + variance per texel, applies a temporal EMA with a
// history buffer, and writes the result.
//
// Buffer layout per probe: 128 floats
//   Indices [0..63]:   mean distance per octahedral texel
//   Indices [64..127]: variance per octahedral texel
//   Index: probeIdx*128 + texelIdx (mean) / probeIdx*128 + 64 + texelIdx (var)
//
// Dispatch: ((ProbeUpdateCount + 63) / 64, 1, 1) workgroups of (64, 1, 1) threads.
// Workgroup size 64 matches LumenDDGIPass.cpp:363
// (pipeDesc.threadGroupSize = {64, 1, 1}) and the dispatch divisor at
// LumenDDGIPass.cpp:899. The Metal source header comment claims (1,1,1), but
// Metal's workgroup size is set by the C++ dispatch API, not the shader —
// WGSL requires it in the shader.
//
// === Binding layout ===
// Buffers only (no textures), so engine bindings map 1:1 to WGPU bindings.
//
//   binding 0  uniform   GlobalShaderData
//   binding 1  uniform   DDGIVolumeData
//   binding 2  storage   ray_buffer (read)
//   binding 3  storage   depth_history (read)
//   binding 4  storage   depth_output (read_write)
//   binding 5  storage   probe_update_list (read)

// ============================================================================
// Constants
// ============================================================================

// From DDGIVolumeData.metal:22-23
const DDGI_DEPTH_RES: u32 = 8u;
const DDGI_DEPTH_TEXELS: u32 = 64u;  // DDGI_DEPTH_RES * DDGI_DEPTH_RES

// ============================================================================
// Struct definitions (verbatim from DDGIUpdateIrradiance.wgsl for layout parity)
// ============================================================================

// GlobalShaderData — must match C++ GlobalShaderData.
struct GlobalShaderData {
    View:               mat4x4<f32>,
    Projection:         mat4x4<f32>,
    InvProjection:      mat4x4<f32>,
    ViewProjection:     mat4x4<f32>,
    PreviousViewProjection: mat4x4<f32>,
    InvViewProjection:  mat4x4<f32>,
    CameraPositionAndViewWidth: vec4<f32>,
    CameraDirectionAndViewHeight: vec4<f32>,
    NumDirectionalLights: u32,
    DeltaTime: f32,
    FrameCount: f32,
    _padding: u32,
};

// DDGIVolumeData — must match C++ DDGIVolumeData struct in LumenDDGIPass.cpp.
// Every offset is reproduced; see DDGITraceRays.wgsl for detailed commentary.
struct DDGIVolumeData {
    ProbeOrigin: vec4<f32>,            //   0: xyz = origin, w unused
    ProbeSpacing: f32,                 //  16
    _pad_before_counts_0: f32,         //  20  explicit 12-byte pad
    _pad_before_counts_1: f32,         //  24
    _pad_before_counts_2: f32,         //  28
    ProbeCounts: vec4<u32>,            //  32: xyz = Nx,Ny,Nz, w unused
    RaysPerProbe: u32,                 //  48
    ProbeCountTotal: u32,              //  52
    IrradianceBlurSigma: f32,          //  56
    DepthBlurSigma: f32,               //  60
    DeltaTime: f32,                    //  64
    FrameIndex: u32,                   //  68
    RayMaxDistance: f32,               //  72
    ProbeHysteresis: f32,             //  76
    TemporalAlpha: f32,               //  80
    ProbeUpdateCount: u32,            //  84
    _pad_before_relocation: f32,      //  88
    ProbeRelocationShiftX: i32,       //  92
    ProbeRelocationShiftY: i32,       //  96
    ProbeRelocationShiftZ: i32,       // 100
    _pad_to_sdf_0: f32,               // 104
    _pad_to_sdf_1: f32,               // 108
    SdfOrigins: array<vec4<f32>, 3>,    // 112
    SdfVoxelSizes: array<vec4<f32>, 3>, // 160
    SdfExtents: array<vec4<f32>, 3>,    // 208
    SdfResolutionsAndCount: vec4<u32>,  // 256
    LightDirection: vec4<f32>,          // 272
    LightColor: vec4<f32>,              // 288
};

// DDGIRayData — storage buffer element.
struct DDGIRayData {
    radiance_and_dist: vec4<f32>,  // xyz = radiance, w = hit_distance
};

// ============================================================================
// Helpers (verbatim from DDGIUpdateIrradiance.wgsl)
// ============================================================================

// Fibonacci sphere ray direction
fn ddgiRayDirection(rayIndex: u32, rayCount: u32, frameIndex: u32) -> vec3<f32> {
    let INV_PHI: f32 = 0.6180339887498948482;
    let PI: f32 = 3.14159265358979323846;
    let u: f32 = fract((f32(rayIndex) + 0.5) * INV_PHI);
    let v: f32 = fract((f32(rayIndex) + 0.5) * INV_PHI * INV_PHI);
    let theta: f32 = 2.0 * PI * u;
    let phi: f32 = acos(1.0 - 2.0 * v);
    let sinPhi: f32 = sin(phi);
    return vec3<f32>(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi));
}

// Sphere direction -> octahedral UV in [0,1]^2
// Port of DDGIVolumeData.metal:158-166.
//
// select() note: Metal select(a, b, c) returns (c ? b : a).
// WGSL select(f, t, c) returns (c ? t : f). Same argument order means
// the same syntax yields the same result.
fn octahedralEncode(d: vec3<f32>) -> vec2<f32> {
    let l1norm: f32 = abs(d.x) + abs(d.y) + abs(d.z);
    var uv: vec2<f32> = d.xy / l1norm;
    if (d.z < 0.0) {
        let s: vec2<f32> = select(vec2<f32>(-1.0), vec2<f32>(1.0), uv.xy >= vec2<f32>(0.0));
        uv = (1.0 - abs(uv.yx)) * s;
    }
    return uv * 0.5 + 0.5;
}

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(1) var<uniform> volume: DDGIVolumeData;
@group(0) @binding(2) var<storage, read> ray_buffer: array<DDGIRayData>;
@group(0) @binding(3) var<storage, read> depth_history: array<f32>;
@group(0) @binding(4) var<storage, read_write> depth_output: array<f32>;
@group(0) @binding(5) var<storage, read> probe_update_list: array<u32>;

// ============================================================================
// Main Kernel: ddgi_update_depth
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn ddgi_update_depth(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let gid: u32 = gid_vec.x;

    if (gid >= volume.ProbeUpdateCount) {
        return;
    }

    let probeIdx: u32 = probe_update_list[gid];

    // -----------------------------------------------------------------------
    // Accumulate per-texel mean + variance from all rays (octahedral binning)
    // -----------------------------------------------------------------------

    let texels: u32 = DDGI_DEPTH_TEXELS;
    var sumDist: array<f32, 64>;
    var sumDistSq: array<f32, 64>;
    var rayCount: array<u32, 64>;
    for (var t: u32 = 0u; t < texels; t++) {
        sumDist[t] = 0.0;
        sumDistSq[t] = 0.0;
        rayCount[t] = 0u;
    }

    let res: u32 = DDGI_DEPTH_RES;

    for (var r: u32 = 0u; r < volume.RaysPerProbe; r++) {
        let ray: DDGIRayData = ray_buffer[probeIdx * volume.RaysPerProbe + r];

        // Skip miss rays (negative distance)
        if (ray.radiance_and_dist.w < 0.0) {
            continue;
        }

        // Compute ray direction and map to octahedral texel
        let rayDir: vec3<f32> = ddgiRayDirection(r, volume.RaysPerProbe, volume.FrameIndex);
        let uv: vec2<f32> = octahedralEncode(rayDir);
        let texel: vec2<u32> = vec2<u32>(
            clamp(u32(uv.x * f32(res)), 0u, res - 1u),
            clamp(u32(uv.y * f32(res)), 0u, res - 1u),
        );
        let texelIdx: u32 = texel.y * res + texel.x;

        let dist: f32 = ray.radiance_and_dist.w;
        sumDist[texelIdx] = sumDist[texelIdx] + dist;
        sumDistSq[texelIdx] = sumDistSq[texelIdx] + dist * dist;
        rayCount[texelIdx] = rayCount[texelIdx] + 1u;
    }

    // -----------------------------------------------------------------------
    // Temporal filter with history and write output
    // -----------------------------------------------------------------------

    let floatsPerProbe: u32 = texels * 2u;  // 128
    let probeBase: u32 = probeIdx * floatsPerProbe;

    // Smooth alpha ramp for depth
    let rampFrames: f32 = 60.0;
    let t: f32 = clamp(f32(max(volume.FrameIndex, 1u) - 1u) / rampFrames, 0.0, 1.0);
    let alpha: f32 = mix(1.0, volume.DepthBlurSigma, t);

    for (var texelIdx: u32 = 0u; texelIdx < texels; texelIdx++) {
        let meanIdx: u32 = probeBase + texelIdx;
        let varIdx: u32 = probeBase + texels + texelIdx;

        // No rays hit in this texel: keep history unchanged
        if (rayCount[texelIdx] == 0u) {
            depth_output[meanIdx] = depth_history[meanIdx];
            depth_output[varIdx] = depth_history[varIdx];
            continue;
        }

        let mean: f32 = sumDist[texelIdx] / f32(rayCount[texelIdx]);
        var variance: f32 = abs(sumDistSq[texelIdx] / f32(rayCount[texelIdx]) - mean * mean);

        // Few samples: inflate variance to avoid overconfidence
        if (rayCount[texelIdx] < 4u) {
            variance = max(variance, volume.ProbeSpacing * volume.ProbeSpacing * 0.1);
        }
        variance = max(variance, 0.001);

        // Exponential moving average
        depth_output[meanIdx] = mix(depth_history[meanIdx], mean, alpha);
        depth_output[varIdx] = mix(depth_history[varIdx], variance, alpha);
    }
}
