// ProbeSampleLib.glsl — DDGI probe ray-direction samplers (single source).
//
// Consumed by BOTH the trace side (DDGITraceRays / DDGITraceSDF) and the
// projection side (DDGIUpdateIrradiance): the projection re-derives directions
// from ray indices instead of storing them, so every direction change must go
// through this shared function or the SH projection desynchronizes from the
// traced radiance.
//
// Every sampler returns vec4(dir, pdf). The SH projection forms the Monte
// Carlo weight as 1/(N·pdf), which makes any pdf-correct sampler unbiased for
// the radiance-SH integral  sh_i = ∫ L(ω)·Y_i(ω) dΩ:
//     E[ Σ_k L(ω_k)·Y_i(ω_k) / (N·p(ω_k)) ] = sh_i
//
// Modes (DDGIVolumeData.RaySampleMode, UBO offset 108; select via the
// DDGI_RAY_SAMPLE env var, default 0 = legacy, zero behavior change):
//   0 UNIFORM — legacy fixed golden-ratio spiral. pdf = 1/(4π), so the
//         projection's legacy 4π/N weight is reproduced bit-exactly. Same set
//         for every probe, every frame (pre-2026-08 behavior).
//   1 JITTER — the spiral rotates by a golden angle each frame (SPGI F5
//         precedent). pdf stays 1/(4π); the probe EMA (α≈0.02) integrates the
//         sequence so every direction is visited within ~N frames instead of
//         a probe being blind to whatever its fixed 64 directions miss.
//   2 COSINE — 50% cosine lobe around the directional light + 50% uniform,
//         per-sample mixture pdf. Variance concentrates where radiance does
//         (sun-facing surfaces); the uniform half keeps the lower hemisphere
//         (and shadowed lobes) covered, and the 1/(8π) mixture floor bounds
//         the MC weight. Jittered like mode 1.
//   3 STRATIFIED — fixed golden-angle azimuth, stratified latitude. The
//         legacy two golden-ratio coordinates hit only 22/64 octahedral
//         depth texels at 128 rays; this mode reaches 62/64. Experimental
//         A/B only until the whole-image GI response has been validated.
//   4 DEPTH_FILL — identical rays/pdf to mode 0; Vulkan depth update fills
//         never-sampled octahedral texels from the nearest observed texel.
//
//   7 OCT_STRATIFIED — equal 8x8 UV strata, frame-dependent 2D hashed
//         jitter. At 128 rays every cell gets two; pdf = length(q)^3/4.
//         SH uses the returned spherical PDF. Depth uses Jacobian-weighted
//         within-cell moments (a finite-sample self-normalized ratio).
//
// Also consumed by DDGIUpdateDepth, which bins the same traced directions.

#ifndef PROBE_SAMPLE_LIB_GLSL
#define PROBE_SAMPLE_LIB_GLSL

#define DDGI_RAY_SAMPLE_UNIFORM 0
#define DDGI_RAY_SAMPLE_JITTER  1
#define DDGI_RAY_SAMPLE_COSINE  2
#define DDGI_RAY_SAMPLE_STRATIFIED 3
#define DDGI_RAY_SAMPLE_DEPTH_FILL 4
#define DDGI_RAY_SAMPLE_DEPTH_ONLY 5
#define DDGI_RAY_SAMPLE_OCT_STRATIFIED 7

// Separate integer streams for the two UV dimensions; no correlated golden
// ratio coordinates. uint operations intentionally wrap modulo 2^32.
uint ddgiOctSampleHash(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

// Equal UV strata, not equal solid angle: dOmega/dUV = 4/length(q)^3.
// For N divisible by 64 each cell receives N/64 samples (128 => two).
vec4 ddgiOctStratifiedSample(uint rayIndex, uint rayCount, uint frameIndex) {
    uint cell = rayIndex / (rayCount / 64u);
    uint seed = ddgiOctSampleHash(frameIndex ^ 0x9e3779b9u) ^
                ddgiOctSampleHash(rayIndex ^ 0x85ebca6bu);
    // 18-bit open midpoints survive float32 cell addition without landing
    // exactly on the next cell boundary (including UV=1).
    vec2 jitter = (vec2(ddgiOctSampleHash(seed ^ 0x68bc21ebu) >> 14u,
                        ddgiOctSampleHash(seed ^ 0x02e5be93u) >> 14u) + 0.5) / 262144.0;
    vec2 uv = (vec2(cell % 8u, cell / 8u) + jitter) / 8.0;
    vec2 oct = uv * 2.0 - 1.0;
    vec3 q = vec3(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (q.z < 0.0)
        q.xy = (1.0 - abs(q.yx)) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(q.xy, vec2(0.0)));
    float qLength = length(q);
    return vec4(q / qLength, qLength * qLength * qLength * 0.25);
}

// dir.xyz = sampled direction (unit), dir.w = pdf(dir).
// toLight: unit vector from surfaces toward the directional light
// (= normalize(-LightDirection.xyz), the same convention DDGITraceRays uses
// for its analytic NdotL term).
vec4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex,
                            uint mode, vec3 toLight) {
    if (mode == DDGI_RAY_SAMPLE_OCT_STRATIFIED) {
        if (rayCount >= 64u && rayCount % 64u == 0u)
            return ddgiOctStratifiedSample(rayIndex, rayCount, frameIndex);
        // A partial oct grid is not the advertised global PDF. Keep a valid
        // uniform fallback if a diagnostic caller uses a non-grid ray count.
        mode = DDGI_RAY_SAMPLE_UNIFORM;
    }
    const float INV_PHI = 0.6180339887498948482;
    const float TWO_PI  = 6.28318530717958647692;

    // Legacy golden-ratio coordinates; mode 3 replaces the correlated
    // latitude coordinate below while retaining this fixed azimuth.
    float u = fract((float(rayIndex) + 0.5) * INV_PHI);
    float v = fract((float(rayIndex) + 0.5) * INV_PHI * INV_PHI);

    if (mode != DDGI_RAY_SAMPLE_UNIFORM && mode != DDGI_RAY_SAMPLE_STRATIFIED &&
        mode != DDGI_RAY_SAMPLE_DEPTH_FILL && mode != DDGI_RAY_SAMPLE_DEPTH_ONLY) {
        // Temporal rotation: golden-angle offsets per frame (never aligned
        // with the lattice, so the union of frame sets stays low-discrepancy).
        u = fract(u + float(frameIndex) * INV_PHI);
        v = fract(v + float(frameIndex) * INV_PHI * INV_PHI);
    }

    if (mode == DDGI_RAY_SAMPLE_STRATIFIED)
        v = (float(rayIndex) + 0.5) / max(float(rayCount), 1.0);

    if (mode == DDGI_RAY_SAMPLE_COSINE) {
        vec3 w = normalize(toLight);
        // Minimal-rotation ONB from +Z to w (Duff et al. 2017).
        float s = w.z >= 0.0 ? 1.0 : -1.0;
        float a = -1.0 / (s + w.z);
        float b = w.x * w.y * a;
        vec3 tangent   = vec3(1.0 + s * w.x * w.x * a, s * b, -s * w.x);
        vec3 bitangent = vec3(b, s + w.y * w.y * a, -w.y);

        vec3 dir;
        if (v < 0.5) {
            // Cosine sub-stratum: v∈[0,0.5) → ξ=2v through the cosine CDF⁻¹.
            float xi   = v * 2.0;
            float cosT = sqrt(max(1.0 - xi, 0.0));
            float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
            float th   = TWO_PI * u;
            dir = w * cosT
                + tangent * (sinT * cos(th))
                + bitangent * (sinT * sin(th));
        } else {
            // Uniform sub-stratum: v∈[0.5,1) remapped to [0,1).
            float vu  = (v - 0.5) * 2.0;
            float ph  = acos(clamp(1.0 - 2.0 * vu, -1.0, 1.0));
            float sp  = sin(ph);
            float th  = TWO_PI * u;
            dir = vec3(sp * cos(th), sp * sin(th), cos(ph));
        }
        float cosL = max(dot(dir, w), 0.0);
        // Mixture pdf: ½·cosine + ½·uniform(1/(4π)); floor for MC-weight safety.
        float pdf = max(0.5 * cosL / 3.14159265358979 + 0.5 / (4.0 * 3.14159265358979), 1e-5);
        return vec4(dir, pdf);
    }

    // Modes 0, 1, 3, 4 and 5: uniform on the sphere, pdf = 1/(4π).
    // Mode 5, like 4, MUST keep the exact fixed mode-0 radiance directions.
    float theta = TWO_PI * u;
    float phi   = acos(clamp(1.0 - 2.0 * v, -1.0, 1.0));
    float sinPhi = sin(phi);
    return vec4(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi),
                0.25 / 3.14159265358979);
}

// Depth-only octahedral strata: exactly two measured directions per 8x8
// texel. These are NOT radiance samples and must never enter SH projection.
vec3 ddgiDepthRayDirection(uint rayIndex) {
    uint cell = rayIndex / 2u;
    float offset = (rayIndex & 1u) == 0u ? 0.25 : 0.75;
    vec2 uv = (vec2(cell % 8u, cell / 8u) + offset) / 8.0 * 2.0 - 1.0;
    vec3 d = vec3(uv, 1.0 - abs(uv.x) - abs(uv.y));
    if (d.z < 0.0)
        d.xy = (1.0 - abs(d.yx)) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(d.xy, vec2(0.0)));
    return normalize(d);
}

#endif  // PROBE_SAMPLE_LIB_GLSL
