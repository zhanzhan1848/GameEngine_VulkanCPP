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
//
// NOT consumed by DDGIUpdateDepth: the octahedral depth map needs a fixed
// frame-stable direction set (its texels ARE the directions).

#ifndef PROBE_SAMPLE_LIB_GLSL
#define PROBE_SAMPLE_LIB_GLSL

#define DDGI_RAY_SAMPLE_UNIFORM 0
#define DDGI_RAY_SAMPLE_JITTER  1
#define DDGI_RAY_SAMPLE_COSINE  2

// dir.xyz = sampled direction (unit), dir.w = pdf(dir).
// toLight: unit vector from surfaces toward the directional light
// (= normalize(-LightDirection.xyz), the same convention DDGITraceRays uses
// for its analytic NdotL term).
vec4 ddgiSampleRayDirection(uint rayIndex, uint rayCount, uint frameIndex,
                            uint mode, vec3 toLight) {
    const float INV_PHI = 0.6180339887498948482;
    const float TWO_PI  = 6.28318530717958647692;

    // Base stratified coordinates — the golden-ratio lattice all modes share,
    // so stratification quality is identical across modes.
    float u = fract((float(rayIndex) + 0.5) * INV_PHI);
    float v = fract((float(rayIndex) + 0.5) * INV_PHI * INV_PHI);

    if (mode != DDGI_RAY_SAMPLE_UNIFORM) {
        // Temporal rotation: golden-angle offsets per frame (never aligned
        // with the lattice, so the union of frame sets stays low-discrepancy).
        u = fract(u + float(frameIndex) * INV_PHI);
        v = fract(v + float(frameIndex) * INV_PHI * INV_PHI);
    }

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

    // Modes 0 and 1: uniform on the sphere, pdf = 1/(4π).
    float theta = TWO_PI * u;
    float phi   = acos(clamp(1.0 - 2.0 * v, -1.0, 1.0));
    float sinPhi = sin(phi);
    return vec4(sinPhi * cos(theta), sinPhi * sin(theta), cos(phi),
                0.25 / 3.14159265358979);
}

#endif  // PROBE_SAMPLE_LIB_GLSL
