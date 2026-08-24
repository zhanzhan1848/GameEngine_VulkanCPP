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
#ifndef SURFACE_CACHE_DATA_GLSL
vec2 octEncode(vec3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    vec2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * mix(vec2(-1.0), vec2(1.0), step(vec2(0.0), result.xy));
    }
    return result * 0.5 + 0.5;
}

vec3 octDecode(vec2 f) {
    f = f * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.xy += mix(vec2(-t), vec2(t), step(vec2(0.0), n.xy));
    return normalize(n);
}
#endif // !SURFACE_CACHE_DATA_GLSL

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

#endif // LUMEN_ENCODE_LIB_GLSL
