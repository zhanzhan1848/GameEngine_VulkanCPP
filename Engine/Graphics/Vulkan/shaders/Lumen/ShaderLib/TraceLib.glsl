#ifndef LUMEN_TRACE_LIB_GLSL
#define LUMEN_TRACE_LIB_GLSL

// ============================================================================
// TraceLib.glsl — shared ray-tracing helpers for the Vulkan Lumen GLSL kernels.
//
// Purpose: GlobalSDF cascade sampling + relaxed sphere tracing + HZB
// hierarchical screen-space marching, extracted verbatim from the per-kernel
// copies so every trace pass marches identically.
//
// Source kernels:
//   - ScreenProbeTraceRays.comp  (isInsideCascade / sampleSDFCascade /
//     sampleBestSDF / traceSDF — SDF part, verbatim semantics)
//   - ScreenProbeTraceSDF.comp   (same march, split-kernel variant)
//   - SSGITrace.comp             (NDCDepthToLinear / viewToScreen /
//     traceRayHZB — HZB part)
//
// Binding-free pure function library: no uniforms, no descriptor bindings,
// no push constants. Every texture/matrix/parameter arrives via function
// arguments — consumers keep their own bindings and pass them in.
//
// Include from a sibling shader (e.g. Lumen/*.comp):
//   #include "ShaderLib/TraceLib.glsl"
// This file intentionally contains NO nested #include so it resolves under
// both glslangValidator's GL_GOOGLE_include_directive (paths relative to the
// including source file) and build_spv.sh's awk include inliner (paths
// relative to the consumer's directory).
// ============================================================================

#extension GL_EXT_samplerless_texture_functions : enable

// Sphere-trace iteration caps (match the extracted kernels).
const uint LUMEN_MAX_SDF_STEPS = 32u;  // ScreenProbeTraceRays.comp / TraceSDF.comp
const uint LUMEN_MAX_HZB_STEPS = 64u;  // SSGITrace.comp MAX_STEPS

// Result of a GlobalSDF sphere trace. Field order fixed by convention:
// hit / distance / position.
struct SDFTraceResult {
    bool  hit;
    float distance;
    vec3  position;
};

// ----------------------------------------------------------------------------
// SDF cascade sampling — verbatim from ScreenProbeTraceRays.comp.
// Metal uses linear-filtered texture3D with clamp_to_edge; the Vulkan port
// uses nearest texelFetch (porting rule 17: nearest acceptable for SDF
// tracing). Cascade convention matches Metal / the C++ ScreenProbeGlobalData
// upload (FieldView::WriteSDFToScreenProbe): origin = sdf_origins[c].xyz
// (corner), extent = sdf_extents[c].xyz (full box size), so [0,1] spans the
// whole volume.
// ----------------------------------------------------------------------------

bool isInsideCascade(vec3 pos, vec3 origin, vec3 extent) {
    vec3 local = pos - origin;
    return local.x >= 0.0 && local.x < extent.x &&
           local.y >= 0.0 && local.y < extent.y &&
           local.z >= 0.0 && local.z < extent.z;
}

float sampleSDFCascade(texture3D sdfTexture, vec3 worldPos,
                       vec3 cascadeOrigin, vec3 cascadeExtent) {
    vec3 uvw = (worldPos - cascadeOrigin) / cascadeExtent;
    if (uvw.x < 0.0 || uvw.x > 1.0 ||
        uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0) {
        return 1e10;
    }
    // 2026-09-06 薄墙穿孔修复（与 DDGITraceSDF.comp 同步）：Metal 侧为
    // filter::linear 三线性；nearest 在 0.19m 体素下让 1-2 体素薄墙被
    // 1.5× 过冲步进整块跳过（采样点落到远侧大 d 体素 → 误逃逸）。8 fetch
    // 手写三线性，描述符布局不变。
    ivec3 size = textureSize(sdfTexture, 0);
    vec3 p = clamp(uvw, vec3(0.0), vec3(1.0)) * vec3(size) - 0.5;
    ivec3 base = ivec3(floor(p));
    vec3 f = p - vec3(base);
    ivec3 c0 = clamp(base, ivec3(0), size - ivec3(1));
    ivec3 c1 = clamp(base + ivec3(1), ivec3(0), size - ivec3(1));
    float d000 = texelFetch(sdfTexture, ivec3(c0.x, c0.y, c0.z), 0).r;
    float d100 = texelFetch(sdfTexture, ivec3(c1.x, c0.y, c0.z), 0).r;
    float d010 = texelFetch(sdfTexture, ivec3(c0.x, c1.y, c0.z), 0).r;
    float d110 = texelFetch(sdfTexture, ivec3(c1.x, c1.y, c0.z), 0).r;
    float d001 = texelFetch(sdfTexture, ivec3(c0.x, c0.y, c1.z), 0).r;
    float d101 = texelFetch(sdfTexture, ivec3(c1.x, c0.y, c1.z), 0).r;
    float d011 = texelFetch(sdfTexture, ivec3(c0.x, c1.y, c1.z), 0).r;
    float d111 = texelFetch(sdfTexture, ivec3(c1.x, c1.y, c1.z), 0).r;
    return mix(mix(mix(d000, d100, f.x), mix(d010, d110, f.x), f.y),
               mix(mix(d001, d101, f.x), mix(d011, d111, f.x), f.y), f.z);
}

// Scalar-extent convenience overload: originExt = vec4(origin.xyz, extent)
// for kernels that pack a cubic cascade extent into a single vec4.
// NOTE: the ScreenProbeGlobalData UBO uploads sdf_origins[c].w = 0 — pass
// vec4(origin.xyz, extentScalar) explicitly, never the raw UBO vec4.
float sampleSDFCascade(texture3D tex, vec4 originExt, vec3 pos) {
    return sampleSDFCascade(tex, pos, originExt.xyz, vec3(originExt.w));
}

// else-if cascade selection — 1 texture3D read per call (finest first).
// cascadeCount = global.sdf_resolutions.w; cascadeOrigins/cascadeExtents are
// the UBO vec4[3] arrays (.xyz used, matches ScreenProbeTraceRays.comp).
float sampleBestSDF(vec3 pos,
                    texture3D sdf0, texture3D sdf1, texture3D sdf2,
                    vec4 cascadeOrigins[3], vec4 cascadeExtents[3],
                    uint cascadeCount) {
    if (cascadeCount > 0u &&
        isInsideCascade(pos, cascadeOrigins[0].xyz, cascadeExtents[0].xyz)) {
        return sampleSDFCascade(sdf0, pos,
                                cascadeOrigins[0].xyz, cascadeExtents[0].xyz);
    } else if (cascadeCount > 1u &&
               isInsideCascade(pos, cascadeOrigins[1].xyz, cascadeExtents[1].xyz)) {
        return sampleSDFCascade(sdf1, pos,
                                cascadeOrigins[1].xyz, cascadeExtents[1].xyz);
    } else if (cascadeCount > 2u &&
               isInsideCascade(pos, cascadeOrigins[2].xyz, cascadeExtents[2].xyz)) {
        return sampleSDFCascade(sdf2, pos,
                                cascadeOrigins[2].xyz, cascadeExtents[2].xyz);
    }
    return 1e10;
}

// ----------------------------------------------------------------------------
// Relaxed sphere tracing through the GlobalSDF — verbatim march from
// ScreenProbeTraceRays.comp::traceSDF:
//   MAX_SDF_STEPS = 32, hitThreshold = voxel0 * 0.5, minStep = voxel0 * 0.25,
//   relaxed advancement: d * 1.2 when far (d > hitThreshold * 4),
//   conservative max(d, minStep) when close.
// voxelSizes = the sdf_voxel_sizes[3] UBO array (only [0].x is read).
// ----------------------------------------------------------------------------

SDFTraceResult traceSDF_relaxed(vec3 origin, vec3 direction, float maxDist,
                                texture3D sdf0, texture3D sdf1, texture3D sdf2,
                                vec4 cascadeOrigins[3], vec4 cascadeExtents[3],
                                vec4 voxelSizes[3], uint cascadeCount) {
    SDFTraceResult result;
    result.hit = false;
    result.position = origin;
    result.distance = 0.0;

    vec3 pos = origin;
    float remainingDist = maxDist;

    float hitThreshold = voxelSizes[0].x * 0.5;
    float minStep = voxelSizes[0].x * 0.25;

    for (uint step = 0u; step < LUMEN_MAX_SDF_STEPS; ++step) {
        float d = sampleBestSDF(pos, sdf0, sdf1, sdf2,
                                cascadeOrigins, cascadeExtents, cascadeCount);

        if (d < hitThreshold) {
            result.hit = true;
            result.position = pos;
            result.distance = length(pos - origin);
            return result;
        }

        // Relaxed advancement: overstep when far, conservative when close
        float advance;
        if (d > hitThreshold * 4.0) {
            advance = d * 1.2;
        } else {
            advance = max(d, minStep);
        }
        if (advance > remainingDist) break;

        pos += direction * advance;
        remainingDist -= advance;
    }

    result.distance = length(pos - origin);
    return result;
}

// ----------------------------------------------------------------------------
// HZB helpers + hierarchical screen-space march — verbatim from
// SSGITrace.comp (NDCDepthToLinear / viewToScreen / traceRayHZB), with the
// SSGIParams uniforms / gd.Projection turned into function parameters.
// The march runs in VIEW space (right-handed, negative Z forward) and
// projects each step through the projection matrix.
// ----------------------------------------------------------------------------

// Vulkan [0,1] NDC depth -> linear view depth.
// (The old OpenGL [-1,1] formula gives near-zero values for valid Vulkan
// depths — see SSGITrace.comp for the original debugging note.)
float NDCDepthToLinear(float ndcDepth, float nearPlane, float farPlane) {
    return nearPlane * farPlane / (farPlane - ndcDepth * (farPlane - nearPlane));
}

// View-space position -> vec3(uv, ndc.z). Vulkan: NDC Y up, texture Y down
// -> flip.
vec3 viewToScreen(vec3 viewPos, mat4 projection) {
    vec4 clipPos = projection * vec4(viewPos, 1.0);
    vec3 ndc = clipPos.xyz / clipPos.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    return vec3(uv, ndc.z);
}

// HZB hierarchical ray march (mip up/down walk, thickness-gated at mip 0).
// origin_uv is kept for signature parity with SSGITrace.comp::traceRayHZB;
// the march itself re-projects every step, so it is unused here.
bool traceRayHZB(texture2D hzb, vec2 origin_uv, vec3 ray_origin_view,
                 vec3 ray_dir_view, mat4 projection,
                 float nearPlane, float farPlane,
                 uint hzbMipLevels, float thickness, float maxDist,
                 uint maxSteps,
                 out vec2 hit_uv, out float hit_depth) {
    float baseStep = maxDist / float(maxSteps);
    float t = 0.0;
    uint mip = 1u;

    for (uint step = 0u; step < maxSteps; ++step) {
        float mipScale = float(1u << mip);
        t += baseStep * mipScale;
        if (t > maxDist) break;

        vec3 ray_pos = ray_origin_view + ray_dir_view * t;
        vec3 screen = viewToScreen(ray_pos, projection);
        vec2 sample_uv = screen.xy;

        if (sample_uv.x < 0.0 || sample_uv.x > 1.0 ||
            sample_uv.y < 0.0 || sample_uv.y > 1.0) break;
        if (ray_pos.z > -nearPlane) continue;

        // Sample HZB at current mip
        ivec2 mipSize = textureSize(hzb, int(mip));
        if (mipSize.x == 0 || mipSize.y == 0) break;

        vec2 mipCoordF = sample_uv * vec2(mipSize);
        ivec2 mipCoord = clamp(ivec2(mipCoordF), ivec2(0), mipSize - ivec2(1));

        float sceneDepthNDC = texelFetch(hzb, mipCoord, int(mip)).r;
        float sceneDepthLinear = NDCDepthToLinear(sceneDepthNDC, nearPlane, farPlane);
        float rayDepthLinear = -ray_pos.z;

        if (rayDepthLinear > sceneDepthLinear) {
            if (mip == 0u) {
                if (rayDepthLinear <= sceneDepthLinear + thickness) {
                    hit_uv = sample_uv;
                    hit_depth = sceneDepthLinear;
                    return true;
                }
            } else {
                t -= baseStep * mipScale;
                mip--;
            }
        } else {
            mip = min(mip + 1u, hzbMipLevels - 1u);
        }
    }
    return false;
}

#endif // LUMEN_TRACE_LIB_GLSL
