// DDGITraceRays.wgsl — Dawn port of DDGITraceRays.metal
//
// Each thread traces one ray from one probe through the GlobalSDF volume.
// On hit, projects the hit point to screen space and samples the previous
// frame's lit scene color (direct + shadow + albedo) as radiance.
// Falls back to analytical sky color for off-screen hits.
// On miss, uses sky color.
//
// Dispatch: (ProbeUpdateCount * RaysPerProbe, 1, 1)
// WorkgroupSize: (64, 1, 1)
//
// === Binding layout ===
// The Metal descriptor set uses dual namespaces: texture(0..3) and buffer(0..3).
// DawnDescriptorSetLayout detects the collision and remaps buffer engine bindings
// to higher WGPU slots (remapOffset = maxBinding+1 = 4).
//
//   Engine binding   Type            WGPU binding
//   texture(0)       SampledImage    0   SDF cascade 0
//   texture(1)       SampledImage    1   SDF cascade 1
//   texture(2)       SampledImage    2   SDF cascade 2
//   texture(3)       SampledImage    3   prev frame lit color
//   buffer(0)        UniformBuffer   4   GlobalShaderData   (remapped)
//   buffer(1)        UniformBuffer   5   DDGIVolumeData     (remapped)
//   buffer(2)        StorageBuffer   6   ray data (output)  (remapped)
//   buffer(3)        StorageBuffer   7   probe update list  (remapped)
//
// No sampler binding is declared — consistent with the project convention for
// Dawn compute shaders (see SSGITrace.wgsl, TAA.wgsl). textureLoad is used
// instead of textureSampleLevel. Metal uses filter::linear; the Dawn port
// uses nearest-neighbour textureLoad for Phase A. Manual trilinear/bilinear
// interpolation can be added in a follow-up if fidelity requires it.

// ============================================================================
// Constants
// ============================================================================

const DDGI_MAX_SDF_STEPS: u32 = 128u;
const DDGI_SKY_COLOR: vec3f = vec3f(0.3, 0.3, 0.35);

// ============================================================================
// Struct definitions (inlined from DDGIVolumeData.metal / CommonTypes.metal)
// ============================================================================

// GlobalShaderData — must match CommonTypes.wgsl / C++ GlobalShaderData.
// Only PreviousViewProjection is used by the trace kernel.
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

// DDGIVolumeData — must match C++ DDGIVolumeData struct in LumenDDGIPass.cpp
// (LumenDDGIPass.h lines 70-111). Every offset is reproduced below.
// WGSL uniform layout rules (std140-equivalent) require care:
//   - vec3/vec4 fields force 16-byte alignment on the next field.
//   - array<T,N> has stride roundUp(16, sizeof(T)) — so array<u32,3> would
//     balloon to 48 bytes. We avoid arrays of scalars and pack the three
//     SdfResolutions + SdfCascadeCount into a single vec4<u32>.
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
    // ProbeRelocationShift is int[3] in C++ at offset 92 (12 bytes).
    // Use three i32 scalars to avoid vec3 16-byte alignment pushing SdfOrigins.
    ProbeRelocationShiftX: i32,       //  92
    ProbeRelocationShiftY: i32,       //  96
    ProbeRelocationShiftZ: i32,       // 100
    _pad_to_sdf_0: f32,               // 104  8-byte pad → SdfOrigins at 112
    _pad_to_sdf_1: f32,               // 108
    // GlobalSDF cascade data (3 cascades)
    SdfOrigins: array<vec4<f32>, 3>,    // 112  (stride 16, matches v4)
    SdfVoxelSizes: array<vec4<f32>, 3>, // 160
    SdfExtents: array<vec4<f32>, 3>,    // 208
    // C++ has u32 SdfResolutions[3] at 256 + u32 SdfCascadeCount at 268.
    // WGSL array<u32,3> would use stride 16 (48 bytes) in a uniform — wrong.
    // Pack [res0, res1, res2, cascadeCount] into one vec4<u32>.
    SdfResolutionsAndCount: vec4<u32>,  // 256: xyz = resolutions, w = cascade count
    LightDirection: vec4<f32>,          // 272: xyz = light dir, w unused
    LightColor: vec4<f32>,              // 288: xyz = light color, w unused
};

// DDGIRayData — storage buffer element.
// Layout: ray_buffer[probeIdx * RaysPerProbe + rayIdx]
struct DDGIRayData {
    radiance_and_dist: vec4<f32>,  // xyz = radiance, w = hit_distance
};

// SDFHitResult — internal trace result (function-local, never in a buffer,
// so field ordering is chosen for clarity rather than packing).
struct SDFHitResult {
    hit: u32,         // 0 = miss, 1 = hit
    distance: f32,
    position: vec3<f32>,
    normal: vec3<f32>,
};

// ============================================================================
// Helpers (from DDGIVolumeData.metal)
// ============================================================================

// Extract probe counts as vec3 from the packed vec4<u32>
fn ddgiGetProbeCounts(vol: ptr<uniform, DDGIVolumeData>) -> vec3<u32> {
    return vec3<u32>((*vol).ProbeCounts.xyz);
}

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

fn ddgiProbeGridCoord(probeIdx: u32, counts: vec3<u32>) -> vec3<u32> {
    let pz: u32 = probeIdx / (counts.x * counts.y);
    let rem: u32 = probeIdx % (counts.x * counts.y);
    let py: u32 = rem / counts.x;
    let px: u32 = rem % counts.x;
    return vec3<u32>(px, py, pz);
}

fn ddgiProbeWorldPos(gc: vec3<u32>, origin: vec3<f32>, spacing: f32) -> vec3<f32> {
    return origin + vec3<f32>(f32(gc.x), f32(gc.y), f32(gc.z)) * spacing;
}

// Convenience accessor for SdfCascadeCount (packed in SdfResolutionsAndCount.w)
fn sdfCascadeCount(vol: ptr<uniform, DDGIVolumeData>) -> u32 {
    return (*vol).SdfResolutionsAndCount.w;
}

// ============================================================================
// SDF Sampling Helpers (ported from DDGITraceRays.metal)
// ============================================================================

// Nearest-neighbour SDF cascade sample via textureLoad.
// Metal uses filter::linear; Phase A Dawn port uses nearest for simplicity.
// Manual trilinear interpolation can be added later if fidelity requires it.
fn sampleSDFCascade(sdfTexture: texture_3d<f32>,
                    worldPos: vec3<f32>,
                    cascadeOrigin: vec3<f32>,
                    cascadeExtent: vec3<f32>,
                    resolution: u32) -> f32 {
    let uvw: vec3<f32> = (worldPos - cascadeOrigin) / cascadeExtent;

    if (uvw.x < 0.0 || uvw.x > 1.0 ||
        uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0) {
        return 1.0e10;
    }

    // Convert normalized [0,1] to nearest texel coordinate.
    // Clamp to [0, resolution-1] to avoid OOB reads.
    let resI: i32 = i32(resolution);
    let tx: i32 = clamp(i32(uvw.x * f32(resI)), 0, resI - 1);
    let ty: i32 = clamp(i32(uvw.y * f32(resI)), 0, resI - 1);
    let tz: i32 = clamp(i32(uvw.z * f32(resI)), 0, resI - 1);

    return textureLoad(sdfTexture, vec3<i32>(tx, ty, tz), 0).r;
}

fn isInsideCascade(pos: vec3<f32>, origin: vec3<f32>, extent: vec3<f32>) -> bool {
    let local: vec3<f32> = pos - origin;
    return local.x >= 0.0 && local.x < extent.x &&
           local.y >= 0.0 && local.y < extent.y &&
           local.z >= 0.0 && local.z < extent.z;
}

// Sample the best available SDF cascade at a position
fn sampleBestSDF(pos: vec3<f32>,
                 sdf0: texture_3d<f32>,
                 sdf1: texture_3d<f32>,
                 sdf2: texture_3d<f32>,
                 vol: ptr<uniform, DDGIVolumeData>) -> f32 {
    var d: f32 = 1.0e10;
    let cc: u32 = sdfCascadeCount(vol);
    if (cc > 0u &&
        isInsideCascade(pos, (*vol).SdfOrigins[0].xyz, (*vol).SdfExtents[0].xyz)) {
        d = min(d, sampleSDFCascade(sdf0, pos, (*vol).SdfOrigins[0].xyz,
                                     (*vol).SdfExtents[0].xyz,
                                     (*vol).SdfResolutionsAndCount.x));
    }
    if (cc > 1u &&
        isInsideCascade(pos, (*vol).SdfOrigins[1].xyz, (*vol).SdfExtents[1].xyz)) {
        d = min(d, sampleSDFCascade(sdf1, pos, (*vol).SdfOrigins[1].xyz,
                                     (*vol).SdfExtents[1].xyz,
                                     (*vol).SdfResolutionsAndCount.y));
    }
    if (cc > 2u &&
        isInsideCascade(pos, (*vol).SdfOrigins[2].xyz, (*vol).SdfExtents[2].xyz)) {
        d = min(d, sampleSDFCascade(sdf2, pos, (*vol).SdfOrigins[2].xyz,
                                     (*vol).SdfExtents[2].xyz,
                                     (*vol).SdfResolutionsAndCount.z));
    }
    return d;
}

// ============================================================================
// Sphere Tracing
// ============================================================================

fn traceSDF(rayOrigin: vec3<f32>,
            rayDir: vec3<f32>,
            maxDist: f32,
            sdf0: texture_3d<f32>,
            sdf1: texture_3d<f32>,
            sdf2: texture_3d<f32>,
            vol: ptr<uniform, DDGIVolumeData>) -> SDFHitResult {
    var result: SDFHitResult;
    result.hit = 0u;
    result.position = rayOrigin;
    result.distance = maxDist;
    result.normal = vec3<f32>(0.0);

    var t: f32 = 0.0;
    let minStep: f32 = 0.01;
    let cc: u32 = sdfCascadeCount(vol);

    for (var step: u32 = 0u; step < DDGI_MAX_SDF_STEPS; step++) {
        let pos: vec3<f32> = rayOrigin + rayDir * t;

        var minDist: f32 = 1.0e10;
        var insideAny: bool = false;

        if (cc > 0u &&
            isInsideCascade(pos, (*vol).SdfOrigins[0].xyz, (*vol).SdfExtents[0].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf0, pos,
                (*vol).SdfOrigins[0].xyz, (*vol).SdfExtents[0].xyz,
                (*vol).SdfResolutionsAndCount.x));
            insideAny = true;
        }
        if (cc > 1u &&
            isInsideCascade(pos, (*vol).SdfOrigins[1].xyz, (*vol).SdfExtents[1].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf1, pos,
                (*vol).SdfOrigins[1].xyz, (*vol).SdfExtents[1].xyz,
                (*vol).SdfResolutionsAndCount.y));
            insideAny = true;
        }
        if (cc > 2u &&
            isInsideCascade(pos, (*vol).SdfOrigins[2].xyz, (*vol).SdfExtents[2].xyz)) {
            minDist = min(minDist, sampleSDFCascade(sdf2, pos,
                (*vol).SdfOrigins[2].xyz, (*vol).SdfExtents[2].xyz,
                (*vol).SdfResolutionsAndCount.z));
            insideAny = true;
        }

        if (!insideAny) {
            let coarsest: u32 = min(2u, cc - 1u);
            t += (*vol).SdfExtents[coarsest].x * 0.1;
            if (t > maxDist) { break; }
            continue;
        }

        let hitThreshold: f32 = (*vol).SdfVoxelSizes[0].x * 0.5;
        if (minDist < hitThreshold) {
            result.hit = 1u;
            result.position = pos;
            result.distance = t;

            // Compute surface normal from SDF gradient (central differences)
            let eps: f32 = max((*vol).SdfVoxelSizes[0].x, 0.01);
            let gradient: vec3<f32> = vec3<f32>(
                sampleBestSDF(pos + vec3<f32>(eps, 0.0, 0.0), sdf0, sdf1, sdf2, vol) -
                sampleBestSDF(pos - vec3<f32>(eps, 0.0, 0.0), sdf0, sdf1, sdf2, vol),
                sampleBestSDF(pos + vec3<f32>(0.0, eps, 0.0), sdf0, sdf1, sdf2, vol) -
                sampleBestSDF(pos - vec3<f32>(0.0, eps, 0.0), sdf0, sdf1, sdf2, vol),
                sampleBestSDF(pos + vec3<f32>(0.0, 0.0, eps), sdf0, sdf1, sdf2, vol) -
                sampleBestSDF(pos - vec3<f32>(0.0, 0.0, eps), sdf0, sdf1, sdf2, vol)
            );
            result.normal = normalize(gradient);
            return result;
        }

        t += max(minDist, minStep);
        if (t > maxDist) { break; }
    }

    return result;
}

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var sdf_cascade_0: texture_3d<f32>;
@group(0) @binding(1) var sdf_cascade_1: texture_3d<f32>;
@group(0) @binding(2) var sdf_cascade_2: texture_3d<f32>;
@group(0) @binding(3) var prev_frame_color: texture_2d<f32>;

@group(0) @binding(4) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(5) var<uniform> volume: DDGIVolumeData;
@group(0) @binding(6) var<storage, read_write> ray_buffer: array<DDGIRayData>;
@group(0) @binding(7) var<storage, read> probeUpdateList: array<u32>;

// ============================================================================
// Main Kernel: ddgi_trace_rays
// ============================================================================

@compute @workgroup_size(64, 1, 1)
fn ddgi_trace_rays(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let gid: u32 = gid_vec.x;
    let totalRays: u32 = volume.ProbeUpdateCount * volume.RaysPerProbe;
    if (gid >= totalRays) {
        return;
    }

    let localProbeIdx: u32 = gid / volume.RaysPerProbe;
    let localRayIdx: u32 = gid % volume.RaysPerProbe;

    // Map sparse update index to real probe index
    let probeIdx: u32 = probeUpdateList[localProbeIdx];

    let counts: vec3<u32> = ddgiGetProbeCounts(&volume);
    let gc: vec3<u32> = ddgiProbeGridCoord(probeIdx, counts);
    let probePos: vec3<f32> = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);

    let rayDir: vec3<f32> = ddgiRayDirection(localRayIdx, volume.RaysPerProbe, volume.FrameIndex);

    // Trace ray through SDF
    var hit: SDFHitResult = traceSDF(
        probePos, rayDir, volume.RayMaxDistance,
        sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, &volume);

    var result: DDGIRayData;

    if (hit.hit != 0u) {
        result.radiance_and_dist.w = hit.distance;

        // Off-screen fallback: conservative sky color only.
        // Surface Cache will replace this with proper off-screen radiance.
        let analyticalRadiance: vec3<f32> = DDGI_SKY_COLOR;

        // Project hit position to previous frame screen space
        let prevClip: vec4<f32> = globalData.PreviousViewProjection * vec4<f32>(hit.position, 1.0);
        if (prevClip.w > 0.0) {
            var prevUV: vec2<f32> = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
            prevUV.y = 1.0 - prevUV.y;

            // Smooth fade: full screen-space at center, blend to analytical at edges
            let edgeDist: vec2<f32> = abs(prevUV - vec2<f32>(0.5)) * 2.0;
            let edgeFade: f32 = clamp(1.0 - (max(edgeDist.x, edgeDist.y) - 0.85) / 0.15, 0.0, 1.0);

            if (edgeFade > 0.0) {
                // Nearest-neighbour textureLoad (Phase A).
                // Manual bilinear can be added later if fidelity requires it.
                let dims: vec2<u32> = textureDimensions(prev_frame_color);
                let clampedUV: vec2<f32> = clamp(prevUV, vec2<f32>(0.0), vec2<f32>(1.0));
                let tx: i32 = clamp(i32(clampedUV.x * f32(dims.x)), 0, i32(dims.x) - 1);
                let ty: i32 = clamp(i32(clampedUV.y * f32(dims.y)), 0, i32(dims.y) - 1);
                let screenRadiance: vec3<f32> = textureLoad(prev_frame_color, vec2<i32>(tx, ty), 0).xyz;
                result.radiance_and_dist.xyz = mix(analyticalRadiance, screenRadiance, vec3<f32>(edgeFade));
            } else {
                result.radiance_and_dist.xyz = analyticalRadiance;
            }
        } else {
            result.radiance_and_dist.xyz = analyticalRadiance;
        }
    } else {
        // Miss: negative distance signals miss
        result.radiance_and_dist.w = -1.0;
        result.radiance_and_dist.xyz = DDGI_SKY_COLOR;
    }

    ray_buffer[probeIdx * volume.RaysPerProbe + localRayIdx] = result;
}
