// DDGITraceRays.wgsl — Dawn port of DDGITraceRays.metal (Mode 11 canonical)
//
// Each thread traces one ray from one probe through the GlobalSDF volume.
// At hit, computes canonical DDGI outgoing radiance using:
//   L_out = (albedo / PI) * (E_direct + E_sky + PI * L_i_prev)
// where:
//   E_direct  = LightColor.rgb * LightColor.w (intensity) * max(NdotL, 0)
//   E_sky     = SkyColor.rgb * max(N.y, 0)
//   L_i_prev  = tetrahedral 4-probe SH (L0+L1) sample of previous-frame probe
//               grid at the hit point — naturally yields multi-bounce GI
//               through temporal feedback.
//
// Apple Silicon scope cuts (per design plan):
//   - No shadow ray toward sun (would double SDF cost) — direct light leaks
//     through walls; follow-up can add single-step SDF shadow test.
//   - No sky occlusion ray — sky contributes under arches; same follow-up.
//   - shDot4 (L0+L1) at hit instead of shDot9 — saves buffer reads.
//
// Dispatch: (ProbeUpdateCount * RaysPerProbe, 1, 1) workgroups of (64, 1, 1).
//
// === Binding layout (engine namespace → WGPU after Dawn remap) ===
// Metal uses separate namespaces for texture(N) and buffer(N). Dawn detects
// the collision and remaps buffers to higher WGPU slots. With 4 textures
// (maxTextureBinding = 3), remapOffset = 4:
//   texture(0)  SDF cascade 0      → WGPU 0
//   texture(1)  SDF cascade 1      → WGPU 1
//   texture(2)  SDF cascade 2      → WGPU 2
//   texture(3)  prev_frame_color   → WGPU 3   (kept declared for layout
//                                              stability; never sampled in
//                                              Mode 11 canonical path)
//   buffer(0)   GlobalShaderData   → WGPU 4
//   buffer(1)   DDGIVolumeData     → WGPU 5
//   buffer(2)   ray_buffer (rw)    → WGPU 6
//   buffer(3)   probeUpdateList    → WGPU 7
//   buffer(4)   irradiance_history → WGPU 8   (NEW — prev-frame probe grid)

// ============================================================================
// Constants
// ============================================================================

const DDGI_MAX_SDF_STEPS: u32 = 128u;

// SH constants (mirror DDGIGIGather.wgsl). L0+L1 reconstruction only —
// L2 would add 5 more coeff reads per probe (4 probes × 5 = 20 extra buffer
// reads), pushing past the Apple Silicon ~32 buffer reads/thread budget.
const _C0: f32 = 0.282095;
const _C1: f32 = 0.488603;

// ============================================================================
// Struct definitions — must match C++ side
// ============================================================================

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

// DDGIVolumeData — matches LumenDDGIPass.h DDGIVolumeData layout byte-for-byte.
// SkyColor + Albedo at offsets 304/320 are Mode 11 additions.
struct DDGIVolumeData {
    ProbeOrigin: vec4<f32>,            //   0
    ProbeSpacing: f32,                 //  16
    _pad_before_counts_0: f32,         //  20
    _pad_before_counts_1: f32,         //  24
    _pad_before_counts_2: f32,         //  28
    ProbeCounts: vec4<u32>,            //  32
    RaysPerProbe: u32,                 //  48
    ProbeCountTotal: u32,              //  52
    IrradianceBlurSigma: f32,          //  56
    DepthBlurSigma: f32,               //  60
    DeltaTime: f32,                    //  64
    FrameIndex: u32,                   //  68
    RayMaxDistance: f32,               //  72
    ProbeHysteresis: f32,              //  76
    TemporalAlpha: f32,                //  80
    ProbeUpdateCount: u32,             //  84
    _pad_before_relocation: f32,       //  88
    ProbeRelocationShiftX: i32,        //  92
    ProbeRelocationShiftY: i32,        //  96
    ProbeRelocationShiftZ: i32,        // 100
    _pad_to_sdf_0: f32,                // 104
    _pad_to_sdf_1: f32,                // 108
    SdfOrigins: array<vec4<f32>, 3>,   // 112
    SdfVoxelSizes: array<vec4<f32>, 3>,// 160
    SdfExtents: array<vec4<f32>, 3>,   // 208
    SdfResolutionsAndCount: vec4<u32>, // 256
    LightDirection: vec4<f32>,         // 272
    LightColor: vec4<f32>,             // 288  (.w = intensity)
    SkyColor: vec4<f32>,               // 304  (Mode 11)
    Albedo: vec4<f32>,                 // 320  (Mode 11)
};

struct DDGIRayData {
    radiance_and_dist: vec4<f32>,
};

struct SDFHitResult {
    hit: u32,
    distance: f32,
    position: vec3<f32>,
    normal: vec3<f32>,
};

// ============================================================================
// Existing helpers (Fibonacci direction, probe grid coord, SDF sampling/trace)
// ============================================================================

fn ddgiGetProbeCounts(vol: ptr<uniform, DDGIVolumeData>) -> vec3<u32> {
    return vec3<u32>((*vol).ProbeCounts.xyz);
}

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

fn sdfCascadeCount(vol: ptr<uniform, DDGIVolumeData>) -> u32 {
    return (*vol).SdfResolutionsAndCount.w;
}

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
// Tetrahedral 4-probe SH interpolation (ported from DDGIGIGather.wgsl)
// Used here to sample previous-frame probe grid at SDF hit position for
// L_i_prev in the canonical DDGI radiance formula.
// ============================================================================

struct TetraResult {
    pi: array<u32, 4>,
    bw: array<f32, 4>,
};

fn tetrahedral(gp: vec3<f32>, gd: vec3<u32>) -> TetraResult {
    var r: TetraResult;
    let b0: vec3<u32> = clamp(vec3<u32>(floor(gp)), vec3<u32>(0u), gd - 1u);
    let b1: vec3<u32> = min(b0 + 1u, gd - 1u);
    let gx: u32 = gd.x;
    let gxy: u32 = gd.x * gd.y;

    var p: array<u32, 8>;
    p[0] = b0.x + b0.y * gx + b0.z * gxy;
    p[1] = b1.x + b0.y * gx + b0.z * gxy;
    p[2] = b0.x + b1.y * gx + b0.z * gxy;
    p[3] = b1.x + b1.y * gx + b0.z * gxy;
    p[4] = b0.x + b0.y * gx + b1.z * gxy;
    p[5] = b1.x + b0.y * gx + b1.z * gxy;
    p[6] = b0.x + b1.y * gx + b1.z * gxy;
    p[7] = b1.x + b1.y * gx + b1.z * gxy;

    let fx: f32 = fract(gp.x);
    let fy: f32 = fract(gp.y);
    let fz: f32 = fract(gp.z);

    if (fx >= fy && fy >= fz) {
        r.pi[0] = p[0]; r.pi[1] = p[1]; r.pi[2] = p[3]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fx; r.bw[1] = fx - fy; r.bw[2] = fy - fz; r.bw[3] = fz;
    } else if (fx >= fz && fz >= fy) {
        r.pi[0] = p[0]; r.pi[1] = p[1]; r.pi[2] = p[5]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fx; r.bw[1] = fx - fz; r.bw[2] = fz - fy; r.bw[3] = fy;
    } else if (fy >= fx && fx >= fz) {
        r.pi[0] = p[0]; r.pi[1] = p[2]; r.pi[2] = p[3]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fy; r.bw[1] = fy - fx; r.bw[2] = fx - fz; r.bw[3] = fz;
    } else if (fy >= fz && fz >= fx) {
        r.pi[0] = p[0]; r.pi[1] = p[2]; r.pi[2] = p[6]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fy; r.bw[1] = fy - fz; r.bw[2] = fz - fx; r.bw[3] = fx;
    } else if (fz >= fx && fx >= fy) {
        r.pi[0] = p[0]; r.pi[1] = p[4]; r.pi[2] = p[5]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fz; r.bw[1] = fz - fx; r.bw[2] = fx - fy; r.bw[3] = fy;
    } else {
        r.pi[0] = p[0]; r.pi[1] = p[4]; r.pi[2] = p[6]; r.pi[3] = p[7];
        r.bw[0] = 1.0 - fz; r.bw[1] = fz - fy; r.bw[2] = fy - fx; r.bw[3] = fx;
    }

    for (var i: u32 = 0u; i < 4u; i++) {
        r.bw[i] = max(r.bw[i], 0.0);
    }
    return r;
}

// L0+L1 SH dot product (4 coefficients). Same as DDGIGIGather.wgsl:86-94.
fn shDot4(c: array<vec3<f32>, 4>, d: vec3<f32>) -> vec3<f32> {
    return c[0] * _C0
         + c[1] * (-_C1 * d.y)
         + c[2] * ( _C1 * d.z)
         + c[3] * (-_C1 * d.x);
}

// ============================================================================
// Previous-frame probe grid sample at SDF hit position
// ============================================================================

// Tetrahedral 4-probe interpolation of prev-frame irradiance SH (L0+L1 only)
// folded into surface normal N. Returns the average incoming radiance L_i_avg
// at the hit point from the previous frame's probe data.
//
// Outside the probe grid: returns vec3(0) — no prev bounce contribution.
// This naturally handles hits outside the DDGI volume (e.g. far-away sky
// occluders); canonical DDGI simply has no prior bounce data there.
fn samplePrevProbeGrid(pos: vec3<f32>, N: vec3<f32>) -> vec3<f32> {
    let origin: vec3<f32> = volume.ProbeOrigin.xyz;
    let spacing: f32 = volume.ProbeSpacing;
    let counts: vec3<u32> = ddgiGetProbeCounts(&volume);
    let gp: vec3<f32> = (pos - origin) / spacing;
    let gridMax: vec3<f32> = vec3<f32>(
        f32(counts.x - 1u),
        f32(counts.y - 1u),
        f32(counts.z - 1u),
    );
    if (any(gp < vec3<f32>(0.0)) || any(gp > gridMax)) {
        return vec3<f32>(0.0);
    }

    let tet: TetraResult = tetrahedral(gp, counts);

    var result: vec3<f32> = vec3<f32>(0.0);
    var totalWeight: f32 = 0.0;

    // Packed float[] layout: 9 coeffs/probe * 3 floats/coeff = 27 floats/probe
    // (12-byte stride — array<vec3<f32>> would silently use 16-byte stride).
    for (var p: u32 = 0u; p < 4u; p++) {
        if (tet.bw[p] < 0.001) { continue; }
        let base: u32 = tet.pi[p] * 27u;
        var sh: array<vec3<f32>, 4>;
        for (var i: u32 = 0u; i < 4u; i++) {
            let cb_i: u32 = base + i * 3u;
            sh[i] = vec3<f32>(
                irradiance_history[cb_i + 0u],
                irradiance_history[cb_i + 1u],
                irradiance_history[cb_i + 2u]);
        }
        let irradiance: vec3<f32> = shDot4(sh, N);
        result = result + irradiance * tet.bw[p];
        totalWeight = totalWeight + tet.bw[p];
    }

    if (totalWeight > 0.0) {
        return result / totalWeight;
    }
    return vec3<f32>(0.0);
}

// ============================================================================
// Bindings
// ============================================================================

@group(0) @binding(0) var sdf_cascade_0: texture_3d<f32>;
@group(0) @binding(1) var sdf_cascade_1: texture_3d<f32>;
@group(0) @binding(2) var sdf_cascade_2: texture_3d<f32>;
// Binding 3 (prev_frame_color) is declared but never sampled in Mode 11.
// Removing it would drop maxTextureBinding from 3 to 2, shifting all buffer
// WGPU bindings down by 1 and breaking the irradiance_history layout.
@group(0) @binding(3) var prev_frame_color: texture_2d<f32>;

@group(0) @binding(4) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(5) var<uniform> volume: DDGIVolumeData;
@group(0) @binding(6) var<storage, read_write> ray_buffer: array<DDGIRayData>;
@group(0) @binding(7) var<storage, read> probeUpdateList: array<u32>;
// Mode 11 NEW: previous-frame irradiance for canonical L_i_prev lookup.
@group(0) @binding(8) var<storage, read> irradiance_history: array<f32>;
// Mode 11 per-vertex albedo: GBuffer albedo sampled at SDF hit position.
// Colored surfaces (fabric, painted walls) bounce colored light — this is
// what makes multi-bounce GI visibly distinct from single-bounce seed.
// Fallback to volume.Albedo when hit is outside camera frustum.
@group(0) @binding(9) var gbuffer_albedo: texture_2d<f32>;

// ============================================================================
// Hit-position → GBuffer albedo lookup (canonical per-surface albedo)
// ============================================================================

// Projects the SDF hit position back to camera screen space, samples the
// GBuffer albedo texture. Falls back to volume.Albedo when the hit is
// outside the camera frustum (e.g. behind camera, beyond far plane).
// This is what makes Mode 11 visibly distinct from Mode 10: colored
// surfaces (green/pink/blue fabric in Sponza) bounce colored light.
fn sampleHitAlbedo(hitPos: vec3<f32>) -> vec3<f32> {
    let clip: vec4<f32> = globalData.ViewProjection * vec4<f32>(hitPos, 1.0);
    if (clip.w <= 0.0) {
        return volume.Albedo.xyz;
    }
    let ndc: vec3<f32> = clip.xyz / clip.w;
    // Frustum cull: NDC x/y in [-1, 1], z in [0, 1] (WebGPU depth range).
    if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return volume.Albedo.xyz;
    }
    // V-flip: WebGPU NDC is Y-up, texture (0,0) is top-left. Matches
    // DDGIGIGather.wgsl:274 reverse projection convention.
    let uv: vec2<f32> = vec2<f32>(
        ndc.x * 0.5 + 0.5,
        1.0 - (ndc.y * 0.5 + 0.5),
    );
    let dims: vec2<u32> = textureDimensions(gbuffer_albedo);
    let tx: i32 = clamp(i32(uv.x * f32(dims.x)), 0, i32(dims.x) - 1);
    let ty: i32 = clamp(i32(uv.y * f32(dims.y)), 0, i32(dims.y) - 1);
    return textureLoad(gbuffer_albedo, vec2<i32>(tx, ty), 0).rgb;
}

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

    let probeIdx: u32 = probeUpdateList[localProbeIdx];

    let counts: vec3<u32> = ddgiGetProbeCounts(&volume);
    let gc: vec3<u32> = ddgiProbeGridCoord(probeIdx, counts);
    let probePos: vec3<f32> = ddgiProbeWorldPos(gc, volume.ProbeOrigin.xyz, volume.ProbeSpacing);

    let rayDir: vec3<f32> = ddgiRayDirection(localRayIdx, volume.RaysPerProbe, volume.FrameIndex);

    var hit: SDFHitResult = traceSDF(
        probePos, rayDir, volume.RayMaxDistance,
        sdf_cascade_0, sdf_cascade_1, sdf_cascade_2, &volume);

    var result: DDGIRayData;

    if (hit.hit != 0u) {
        let N: vec3<f32> = hit.normal;
        let hitPos: vec3<f32> = hit.position;
        let PI: f32 = 3.14159265358979;

        // E_direct: analytic sun irradiance — no shadow ray (Apple Silicon
        // scope cut). Direct light will leak through walls; documented as
        // known trade-off in the Mode 11 plan.
        let L: vec3<f32> = normalize(-volume.LightDirection.xyz);
        let NdotL: f32 = max(0.0, dot(N, L));
        let lightIntensity: f32 = volume.LightColor.w;
        let E_direct: vec3<f32> = volume.LightColor.xyz * lightIntensity * NdotL;

        // E_sky: hemispherical sky irradiance weighted by NdotUp — no
        // occlusion ray (same scope cut). Sky contributes even under arches.
        let NdotUp: f32 = max(0.0, N.y);
        let E_sky: vec3<f32> = volume.SkyColor.xyz * NdotUp;

        // L_i_prev: prev-frame probe grid sample — multi-bounce GI emerges
        // from temporal feedback through this lookup.
        let L_i_prev: vec3<f32> = samplePrevProbeGrid(hitPos, N);

        // Canonical DDGI Lambertian reflected radiance:
        //   L_out = (albedo / PI) * (E_direct + E_sky + PI * L_i_prev)
        // albedo is sampled per-hit from GBuffer (canonical per-surface albedo
        // → colored bounce light). Falls back to volume.Albedo when the hit is
        // outside the camera frustum (probes behind camera see no GBuffer).
        let albedo: vec3<f32> = sampleHitAlbedo(hitPos);
        let L_out: vec3<f32> = (albedo / PI) * (E_direct + E_sky + PI * L_i_prev);

        result.radiance_and_dist = vec4<f32>(L_out, hit.distance);
    } else {
        // Miss: ray escaped to sky.
        result.radiance_and_dist = vec4<f32>(volume.SkyColor.xyz, -1.0);
    }

    ray_buffer[probeIdx * volume.RaysPerProbe + localRayIdx] = result;
}
