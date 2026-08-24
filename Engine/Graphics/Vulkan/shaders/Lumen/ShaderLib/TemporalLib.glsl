#ifndef LUMEN_TEMPORAL_LIB_GLSL
#define LUMEN_TEMPORAL_LIB_GLSL

// ============================================================================
// TemporalLib.glsl — shared temporal-accumulation helpers for the Vulkan
// Lumen GLSL kernels.
//
// Purpose: EMA blending, neighborhood variance clamping (history AABB), and
// previous-frame reprojection — the three primitives every denoiser /
// temporal pass reimplements inline.
//
// Source kernels:
//   - SSGITemporal.metal (Engine/Graphics/Metal/shaders/Lumen/) lines
//     131-152: variance clipping AABB = mean +/- 2*sigma + floor
//   - SSGITemporal.comp (GLSL port) lines 137-143: same clamp with
//     abs(mean) in the floor term
//   - ScreenProbeTemporal.comp lines 115-118 (T5.2): world -> prev screen
//     UV reprojection (clip -> ndc -> Y-flipped uv)
//
// Binding-free pure function library: no uniforms, no descriptor bindings,
// no push constants. Matrices and scalars arrive via function arguments.
// Requires no GLSL extensions.
//
// Include from a sibling shader (e.g. Lumen/*.comp):
//   #include "ShaderLib/TemporalLib.glsl"
// This file intentionally contains NO nested #include so it resolves under
// both glslangValidator's GL_GOOGLE_include_directive (paths relative to the
// including source file) and build_spv.sh's awk include inliner (paths
// relative to the consumer's directory).
// ============================================================================

// ----------------------------------------------------------------------------
// Exponential moving average. alpha is the weight of CURRENT:
//   alpha = 1.0 -> current (no history), alpha = 0.0 -> frozen history.
// Same convention as the DDGI / SPGI / SSGI temporal kernels' blend:
// result = mix(history, current, alpha).
// ----------------------------------------------------------------------------
vec3 temporalEMA(vec3 current, vec3 history, float alpha) {
    return mix(history, current, alpha);
}

// ----------------------------------------------------------------------------
// Neighborhood variance clamp: clamp `history` into the AABB spanned by the
// current frame's 3x3-neighborhood statistics (mean / sigma from
// sum & sum-of-squares in the caller).
//   AABB = mean +/- scale * sigma + sigmaFloor, with scale = 2.0 capturing
//   ~95% of the distribution (Metal SSGITemporal reference) and
//   sigmaFloor = max(abs(mean) * 0.1, 0.01) keeping the box open around
//   zero-energy channels (abs() per the GLSL SSGITemporal.comp port so
//   negative means do not shrink the floor below the 0.01 minimum).
// ----------------------------------------------------------------------------
void varianceClampAABB(vec3 history, vec3 mean, vec3 sigma, float scale,
                       out vec3 clamped) {
    vec3 sigmaFloor = max(abs(mean) * 0.1, vec3(0.01));
    vec3 aabbMin = mean - sigma * scale - sigmaFloor;
    vec3 aabbMax = mean + sigma * scale + sigmaFloor;
    clamped = clamp(history, aabbMin, aabbMax);
}

// ----------------------------------------------------------------------------
// Reproject a world position into the PREVIOUS frame's screen UV.
// clip -> ndc -> Y-flipped uv (Vulkan: NDC Y up, texture Y down), verbatim
// from ScreenProbeTemporal.comp (T5.2 velocity reprojection).
//
// Returns vec4(uv.x, uv.y, clip.w, valid):
//   xy = previous-frame screen UV
//   z  = clip.w (positive when the point is in front of the camera)
//   w  = 1.0 when the projection is usable (clip.w > 0), else 0.0
// The UV is NOT bounds-checked — test xy against [0,1] at the call site.
// ----------------------------------------------------------------------------
vec4 reprojectPrevUV(vec3 worldPos, mat4 prevVP) {
    vec4 clip = prevVP * vec4(worldPos, 1.0);
    if (clip.w <= 0.0) return vec4(0.0, 0.0, clip.w, 0.0);
    vec2 ndc = clip.xy / clip.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    return vec4(uv, clip.w, 1.0);
}

#endif // LUMEN_TEMPORAL_LIB_GLSL
