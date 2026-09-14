#ifndef LUMEN_ENCODE_LIB_GLSL
#define LUMEN_ENCODE_LIB_GLSL

// ============================================================================
// EncodeLib.glsl — shared encoding / direction-generation helpers for the
// Vulkan Lumen GLSL kernels.
//
// Purpose: SH9 evaluation, octahedral normal encode/decode, the Karis
// EnvBRDF approximation, and the Fibonacci-sphere + TBN orientation pair
// that every probe-tracing kernel duplicates.
//
// Source kernels:
//   - ScreenProbeTraceRays.comp     (fibonacciDirection / orientAroundNormal
//     — the most-duplicated pair; every SPGI kernel must generate the same
//     ray for the same global_id)
//   - ScreenProbeTraceSDF.comp / ScreenProbeTraceHZB.comp /
//     ScreenProbeTraceFinalize.comp (same pair)
//   - SurfaceCacheData.glsl         (octEncode / octDecode)
//   - DDGIGIGather.comp             (SH9 reconstruction formula, L0+L1+L2)
//   - FusionComposite.frag          (envBRDFApproxAB)
//
// Binding-free pure function library: no uniforms, no descriptor bindings.
// All inputs arrive via function arguments; requires no GLSL extensions.
//
// Include from a sibling shader (e.g. Lumen/*.comp):
//   #include "ShaderLib/EncodeLib.glsl"
// This file intentionally contains NO nested #include so it resolves under
// both glslangValidator's GL_GOOGLE_include_directive (paths relative to the
// including source file) and build_spv.sh's awk include inliner (paths
// relative to the consumer's directory).
// ============================================================================

// Fibonacci-sphere constants (float32-identical to the per-kernel copies:
// 3.14159265358979323846 == 3.14159265358979, 1.618033988749895 ==
// 1.61803398875 at float precision).
const float ENCODELIB_PI           = 3.14159265358979323846;
const float ENCODELIB_GOLDEN_RATIO = 1.618033988749895;

// ----------------------------------------------------------------------------
// SH (L0 + L1 + L2, 9 coefficients). Constants from the DDGIGIGather.comp
// reconstruction formula (float32-identical to its high-precision variants
// 0.28209479177 / 0.48860251190 / 1.09254843059 / 0.31539156525 /
// 0.54627421530). Prefixed SH9_* to avoid shadowing the local SH_C* consts
// some kernels declare inside function bodies.
// ----------------------------------------------------------------------------
const float SH9_C0   = 0.28209479;  // L0, Y00
const float SH9_C1   = 0.48860251;  // L1, Y1x/y/z
const float SH9_C2_0 = 1.09254843;  // L2, Y2xy/yz/zx
const float SH9_C2_1 = 0.31539157;  // L2, Y2zz (3zz - 1)
const float SH9_C2_2 = 0.54627421;  // L2, Y2xx-yy

// Evaluate a 9-coefficient SH irradiance field in direction d.
// Coefficient layout matches DDGIGIGather / DDGI probe buffers:
//   [0]=Y00, [1]=Y1y, [2]=Y1z, [3]=Y1x, [4]=Y2xy, [5]=Y2yz, [6]=Y2zx,
//   [7]=Y2zz, [8]=Y2xx-yy
vec3 evalSH9(vec3 sh[9], vec3 d) {
    float x = d.x, y = d.y, z = d.z;
    float xy = x * y, yz = y * z, zx = z * x;
    float xx = x * x, yy = y * y, zz = z * z;
    return sh[0] * SH9_C0
         + sh[1] * (SH9_C1 * y)
         + sh[2] * (SH9_C1 * z)
         + sh[3] * (SH9_C1 * x)
         + sh[4] * (SH9_C2_0 * xy)
         + sh[5] * (SH9_C2_0 * yz)
         + sh[6] * (SH9_C2_0 * zx)
         + sh[7] * (SH9_C2_1 * (3.0 * zz - 1.0))
         + sh[8] * (SH9_C2_2 * (xx - yy));
}

// ----------------------------------------------------------------------------
// Octahedral normal encoding — verbatim from SurfaceCacheData.glsl.
// When the consumer also includes SurfaceCacheData.glsl, that header's copy
// wins (identical bodies); this guard prevents a duplicate-definition error.
// Ordering: include SurfaceCacheData.glsl BEFORE EncodeLib.glsl.
// ----------------------------------------------------------------------------
// T1.10: octEncode/octDecode moved to the canonical MathLib.glsl — this
// header re-exports them for existing consumers.
#include "ShaderLib/MathLib.glsl"

// ----------------------------------------------------------------------------
// Karis analytic split-sum integral (mobile EnvBRDF approximation).
// Returns the (scale, bias) pair: specular = F0 * AB.x + AB.y.
// Verbatim from FusionComposite.frag.
// ----------------------------------------------------------------------------
vec2 envBRDFApproxAB(float roughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = vec4(roughness) * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// ----------------------------------------------------------------------------
// Fibonacci sphere direction generation + normal alignment — verbatim from
// ScreenProbeTraceRays.comp. Byte-for-byte identical across kernels is a
// HARD requirement: hit_buffer entries are indexed by ray, so every kernel
// must generate the same ray direction for the same (probeIdx, rayIdx).
// ----------------------------------------------------------------------------

vec3 fibonacciDirection(uint rayIndex, uint totalRays) {
    float theta = 2.0 * ENCODELIB_PI * float(rayIndex) / ENCODELIB_GOLDEN_RATIO;
    float phi = acos(1.0 - 2.0 * (float(rayIndex) + 0.5) / float(totalRays));
    return vec3(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
}

// Reorient a local-frame direction (z = up in the local hemisphere) onto an
// arbitrary surface normal via an orthonormal TBN basis.
vec3 orientAroundNormalTBN(vec3 dir, vec3 normal) {
    vec3 up = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * dir.x + bitangent * dir.y + normal * dir.z);
}

// ----------------------------------------------------------------------------
// F5 (M7, 2026-08-26): SPGI per-frame jitter. UE Lumen offsets probe placement
// and ray directions every frame and lets the temporal filter average over
// frames; a fixed grid + fixed Fibonacci set produces structured banding that
// EMA cannot smooth because every frame's input is identical.
//
// Seed delivery: camera_position.w carries the frame index as float, or a
// negative value when jitter is disabled. The Average kernel (no global UBO)
// receives the same seed through its AvgConstants pad slot instead.
//
// The rotation applies BEFORE orientAroundNormalTBN so the whole local
// direction set rotates as a rigid body — the set stays uniformly distributed
// and cosine/SH weights computed downstream from the actual rayDir stay
// unbiased. Byte-for-byte equivalent copies live in the Metal kernels
// (ScreenProbeTraceRays.metal / ScreenProbeAverage.metal); WGSL follows with
// the T5.4 port.
// ----------------------------------------------------------------------------

uint spgiJitterHash(uint v) {
    v ^= v >> 16u; v *= 0x7feb352du;
    v ^= v >> 15u; v *= 0x846ca68bu;
    v ^= v >> 16u;
    return v;
}

// Rz(a2) * Rx(90°) * Rz(a1): two-parameter rotation family. Not uniform over
// SO(3), but fully decorrelates consecutive frames' Fibonacci sets — enough
// for temporal averaging, at 2 hashes + 4 trig ops per probe.
mat3 spgiJitterRotation(uint probeIdx, float frameSeed) {
    if (frameSeed < 0.0) return mat3(1.0);
    uint fs = uint(frameSeed);
    uint h1 = spgiJitterHash(probeIdx * 0x9e3779b9u ^ fs);
    uint h2 = spgiJitterHash(h1 ^ 0x68bc21ebu);
    float a1 = float(h1) * (2.0 * ENCODELIB_PI / 4294967295.0);
    float a2 = float(h2) * (2.0 * ENCODELIB_PI / 4294967295.0);
    float c1 = cos(a1), s1 = sin(a1);
    float c2 = cos(a2), s2 = sin(a2);
    mat3 rz1 = mat3(c1, s1, 0.0, -s1, c1, 0.0, 0.0, 0.0, 1.0);
    mat3 rxf = mat3(1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 1.0, 0.0);  // fixed 90° X
    mat3 rz2 = mat3(c2, s2, 0.0, -s2, c2, 0.0, 0.0, 0.0, 1.0);
    return rz2 * rxf * rz1;
}

// Per-probe [-1,1]² screen offset (caller scales to pixels and applies the
// world-space amplitude clamp — see ScreenProbePlace).
vec2 spgiJitterOffset01(uint probeIdx, float frameSeed) {
    if (frameSeed < 0.0) return vec2(0.0);
    uint h = spgiJitterHash(uint(frameSeed) ^ (probeIdx * 0x9e3779b9u));
    return vec2(float(h & 0xffffu), float((h >> 16u) & 0xffffu))
         * (2.0 / 65535.0) - 1.0;
}

// ----------------------------------------------------------------------------
// F2 (M7) 形态 c — importance resampling: UE-style ray reallocation without a
// radiance map. The proposal pdf runs over the probe's rotated Fibonacci
// candidates (N = raysPerProbe, capped at 64):
//   q_i ∝ max(cosθ_i, 0.02) * (luminance(evalSH_L1(history, d_i)) + 0.05)
// Each ray samples one jittered CDF stratum. invNq=1/(N*q_c) corrects
// the discrete resampling of the full-sphere candidate set. Average projects
// radiance with (4pi/rayCount)*invNq; cosine convolution belongs in Gather.
//
// Deterministic in (probeIdx, frameSeed): every kernel that regenerates
// directions must pass the SAME history SH — probe_avg_radiance_[histIdx],
// i.e. the previous frame's post-temporal/spatial output. Zero history
// (first frame / reset) degrades to a cosine-only proposal. Byte-equivalent
// MSL copies live in ScreenProbeTraceRays.metal / ScreenProbeAverage.metal.
// ----------------------------------------------------------------------------
const uint SPGI_IS_MAX_RAYS = 64u;

vec3 spgiImportanceDirection(uint rayIdx, uint raysPerProbe,
                             uint probeIdx, float frameSeed,
                             vec3 probeNormal,
                             vec3 sh0, vec3 sh1, vec3 sh2, vec3 sh3,
                             out float invNq) {
    uint n = min(raysPerProbe, SPGI_IS_MAX_RAYS);
    mat3 rot = spgiJitterRotation(probeIdx, frameSeed);

    // TBN frame matching orientAroundNormalTBN — the pdf evaluates the SH in
    // world space; cosθ_i = local.z under either frame (orthonormal, z→N).
    vec3 up = abs(probeNormal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, probeNormal));
    vec3 bitangent = cross(probeNormal, tangent);

    float pdf[SPGI_IS_MAX_RAYS];
    float sum = 0.0;
    for (uint i = 0u; i < n; ++i) {
        vec3 local = rot * fibonacciDirection(i, n);
        vec3 world = tangent * local.x + bitangent * local.y + probeNormal * local.z;
        vec3 irr = sh0 * 0.282095
                 + sh1 * (-0.488603 * world.y)
                 + sh2 * (0.488603 * world.z)
                 + sh3 * (-0.488603 * world.x);
        float lum = max(dot(irr, vec3(0.2126, 0.7152, 0.0722)), 0.0);
        pdf[i] = max(local.z, 0.02) * (lum + 0.05);
        sum += pdf[i];
    }

    // Inverse-CDF resampling at this ray's stratified position.
    // Defensive mixture keeps every candidate reachable even when history
    // misses a bright lobe. q >= 0.25/N bounds invNq <= 4 without clipping
    // radiance or biasing the estimator; PDF correction uses this mixture.
    for (uint i = 0u; i < n; ++i)
        pdf[i] = 0.25 / float(n) + 0.75 * pdf[i] / max(sum, 1e-6);
    sum = 1.0;

    // Jitter within each CDF stratum. A fixed midpoint can permanently omit
    // small-probability candidates; the PDF correction needs random coverage.
    uint sampleSeed = frameSeed < 0.0 ? 0u : uint(frameSeed);
    uint sampleHash = spgiJitterHash(probeIdx * 0x9e3779b9u ^ sampleSeed ^ (rayIdx * 0x85ebca6bu));
    float jitter = (float(sampleHash & 0x00ffffffu) + 0.5) / 16777216.0;
    float u = (float(rayIdx) + jitter) / float(n) * sum;
    uint c = n - 1u;
    float acc = 0.0;
    for (uint i = 0u; i < n; ++i) {
        acc += pdf[i];
        if (u <= acc) { c = i; break; }
    }

    invNq = sum / (float(n) * max(pdf[c], 1e-6));
    return rot * fibonacciDirection(c, n);
}

#endif // LUMEN_ENCODE_LIB_GLSL
