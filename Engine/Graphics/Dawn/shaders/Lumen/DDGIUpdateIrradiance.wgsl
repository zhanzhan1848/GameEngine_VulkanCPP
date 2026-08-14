// DDGIUpdateIrradiance.wgsl — Dawn port of DDGIUpdateIrradiance.metal
//
// Projects ray radiance into SH9 (9 coefficients per probe) and blends
// temporally with a history buffer using a hysteresis EMA.
//
// Dispatch: ((ProbeUpdateCount + 63) / 64, 1, 1) workgroups of (64, 1, 1) threads
//
// Each thread handles one probe end-to-end: accumulates 9 SH coefficients
// in registers, then writes the temporally-blended result. There is no
// cooperative SH projection and no threadgroup shared memory. Workgroup size
// 64 matches LumenDDGIPass.cpp:355 (pipeDesc.threadGroupSize = {64, 1, 1})
// and the dispatch divisor at LumenDDGIPass.cpp:867. The Metal source's
// header comment claims (1,1,1), but Metal's workgroup size is set by the
// C++ dispatch API, not the shader — WGSL requires it in the shader.
//
// === Binding layout ===
// Buffers only (no textures), so engine bindings map 1:1 to WGPU bindings.
//
//   binding 0  uniform   GlobalShaderData
//   binding 1  uniform   DDGIVolumeData
//   binding 2  storage   ray_buffer (read)
//   binding 3  storage   irradiance_history (read)
//   binding 4  storage   irradiance_output (read_write)
//   binding 5  storage   probe_update_list (read)

// ============================================================================
// Constants
// ============================================================================

const DDGI_SKY_COLOR: vec3f = vec3f(0.3, 0.3, 0.35);

// SH9 constants (from DDGIVolumeData.metal:25-30)
const DDGI_SH_C0:   f32 = 0.282095;
const DDGI_SH_C1:   f32 = 0.488603;
const DDGI_SH_C2_0: f32 = 1.092548;
const DDGI_SH_C2_1: f32 = 0.315392;
const DDGI_SH_C2_2: f32 = 0.546274;

// ============================================================================
// Struct definitions (verbatim from DDGITraceRays.wgsl for layout parity)
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
// Helpers (verbatim from DDGITraceRays.wgsl)
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

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(1) var<uniform> volume: DDGIVolumeData;
@group(0) @binding(2) var<storage, read> ray_buffer: array<DDGIRayData>;
// Packed float[] layout: 9 coeffs/probe * 3 floats/coeff = 27 floats/probe (12-byte stride).
// Using array<vec3<f32>> here would silently use 16-byte stride (WGSL host-shareable
// alignment) and corrupt/overflow the buffer the C++ side allocated at 12-byte stride.
@group(0) @binding(3) var<storage, read> irradiance_history: array<f32>;
@group(0) @binding(4) var<storage, read_write> irradiance_output: array<f32>;
@group(0) @binding(5) var<storage, read> probe_update_list: array<u32>;

// ============================================================================
// Main Kernel: ddgi_update_irradiance
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn ddgi_update_irradiance(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let gid: u32 = gid_vec.x;

    if (gid >= volume.ProbeUpdateCount) {
        return;
    }

    // Map sparse update index to real probe index
    let probeIdx: u32 = probe_update_list[gid];
    let rayOffset: u32 = probeIdx * volume.RaysPerProbe;

    // Accumulate L0+L1+L2 (9 coefficients)
    var shAccum: array<vec3<f32>, 9>;
    for (var i: u32 = 0u; i < 9u; i++) {
        shAccum[i] = vec3<f32>(0.0);
    }

    for (var r: u32 = 0u; r < volume.RaysPerProbe; r++) {
        let rayIdx: u32 = rayOffset + r;
        var radiance: vec3<f32> = ray_buffer[rayIdx].radiance_and_dist.xyz;
        let hitDist: f32 = ray_buffer[rayIdx].radiance_and_dist.w;

        if (hitDist < 0.0) {
            radiance = DDGI_SKY_COLOR;
        } else {
            // Gentler distance falloff — only attenuate near max distance
            let distWeight: f32 = 1.0 - smoothstep(volume.RayMaxDistance * 0.8, volume.RayMaxDistance, hitDist);
            radiance = radiance * max(distWeight, 0.2);
        }

        let d: vec3<f32> = ddgiRayDirection(r, volume.RaysPerProbe, volume.FrameIndex);
        let x: f32 = d.x;
        let y: f32 = d.y;
        let z: f32 = d.z;
        let x2: f32 = x * x;
        let y2: f32 = y * y;
        let z2: f32 = z * z;

        // L0+L1+L2 SH basis
        var basis: array<f32, 9>;
        basis[0] =  DDGI_SH_C0;
        basis[1] = -DDGI_SH_C1 * y;
        basis[2] =  DDGI_SH_C1 * z;
        basis[3] = -DDGI_SH_C1 * x;
        basis[4] =  DDGI_SH_C2_0 * y * x;
        basis[5] = -DDGI_SH_C2_0 * y * z;
        basis[6] =  DDGI_SH_C2_1 * (3.0 * z2 - 1.0);
        basis[7] = -DDGI_SH_C2_0 * x * z;
        basis[8] =  DDGI_SH_C2_2 * (x2 - y2);

        let mcWeight: f32 = 4.0 * 3.14159265 / f32(volume.RaysPerProbe);

        for (var i: u32 = 0u; i < 9u; i++) {
            shAccum[i] = shAccum[i] + radiance * basis[i] * mcWeight;
        }
    }

    // Temporal blend with history
    // 9 coeffs * 3 floats = 27 float slots per probe in the packed layout.
    let probeBase: u32 = probeIdx * 27u;

    // Smooth alpha ramp: 1.0 → ProbeHysteresis over ~60 frames
    let rampFrames: f32 = 60.0;
    let t: f32 = clamp(f32(max(volume.FrameIndex, 1u) - 1u) / rampFrames, 0.0, 1.0);
    let alpha: f32 = mix(1.0, volume.ProbeHysteresis, t);

    // Energy clamping limits
    let sh0: vec3<f32> = shAccum[0];
    let l1Limit: f32 = max(length(sh0) * 3.0, 0.1);
    let l2Limit: f32 = max(length(sh0) * 2.0, 0.05);

    for (var i: u32 = 0u; i < 9u; i++) {
        // Read history (packed float[3] per coeff) with NaN guard.
        // (Tint rejects isnan/isinf; inspect exponent bits manually.)
        // bitcast<u32> is the WGSL equivalent of Metal's as_type<uint>.
        let coeffBase: u32 = probeBase + i * 3u;
        var history: vec3<f32> = vec3<f32>(
            irradiance_history[coeffBase + 0u],
            irradiance_history[coeffBase + 1u],
            irradiance_history[coeffBase + 2u]);
        let bits_x: u32 = bitcast<u32>(history.x);
        let bits_y: u32 = bitcast<u32>(history.y);
        let bits_z: u32 = bitcast<u32>(history.z);
        if (((bits_x | bits_y | bits_z) & 0x7F800000u) == 0x7F800000u) {
            history = vec3<f32>(0.0);
        }

        var filtered: vec3<f32> = mix(history, shAccum[i], alpha);

        // Energy-aware clamp per band
        if (i == 0u) {
            // L0: absolute limit
            let mag: f32 = length(filtered);
            if (mag > 10.0) {
                filtered = filtered * (10.0 / mag);
            }
        } else if (i < 4u) {
            // L1: energy proportional to L0
            let mag: f32 = length(filtered);
            if (mag > l1Limit) {
                filtered = filtered * (l1Limit / mag);
            }
        } else {
            // L2: tighter clamp (higher order = more noise)
            let mag: f32 = length(filtered);
            if (mag > l2Limit) {
                filtered = filtered * (l2Limit / mag);
            }
        }

        // Packed write: 3 floats per coeff.
        irradiance_output[coeffBase + 0u] = filtered.x;
        irradiance_output[coeffBase + 1u] = filtered.y;
        irradiance_output[coeffBase + 2u] = filtered.z;
    }
}
