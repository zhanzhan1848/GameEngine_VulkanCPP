#ifndef SURFACE_CACHE_DATA_GLSL
#define SURFACE_CACHE_DATA_GLSL

// The C-series query contract (scQueryCardCandidate) reads atlases through
// samplerless texelFetch; enable the extension here so every consumer gets it
// (duplicate enable in the including shader is legal).
#extension GL_EXT_samplerless_texture_functions : enable

// Shared structs + helpers for SurfaceCache Vulkan GLSL shaders.
// Ported from Engine/Graphics/Metal/shaders/Lumen/SurfaceCacheData.metal.
// All vec3 → vec4 for 16-byte alignment (matches Metal float4 / C++ math::v4 pattern).

// Matches C++ SurfaceCacheCard (64 bytes).
struct SurfaceCacheCard {
    vec4  center;          // xyz = center, w unused
    vec4  extent;          // xyz = half-extents, w unused
    uint  axis_direction;  // low byte = axis, high byte = direction
    uint  resolution;
    uint  atlas_offset_x;
    uint  atlas_offset_y;
    uint  mesh_instance_id;
    uint  _pad1;
    uint  _pad2;
    uint  _pad3;
};

// Matches C++ SurfaceCacheCardLookup (48 bytes).
struct SurfaceCacheCardLookup {
    vec4  aabb_min;    // xyz = min, w unused
    vec4  aabb_max;    // xyz = max, w unused
    uint  card_start;
    uint  card_count;
    uint  _pad0;
    uint  _pad1;
};

// Matches C++ SurfaceCacheParams (48 bytes).
struct SurfaceCacheParams {
    uint  atlas_size;
    uint  page_size;
    uint  capture_budget_pages;
    uint  max_cards;
    float update_distance;
    float importance_weight;
    uint  max_lights_per_tile;
    uint  indirect_rays_per_probe;
    float indirect_temporal_weight;
    float indirect_near_distance;
    uint  lookup_count;
    uint  _pad;
};

// Matches Metal LightInfo (48 bytes).
struct LightInfo {
    vec4  position;    // xyz = position, w = radius
    vec4  color;       // xyz = color, w = unused
    vec4  direction;   // xyz = direction, w = type (as float)
};

// Matches C++ CardDispatchInfo (32 bytes).
struct CardDispatchInfo {
    uint texel_offset;
    uint texel_count;
    uint resolution;
    uint atlas_offset_x;
    uint atlas_offset_y;
    uint card_index;
    uint _pad1;
    uint _pad2;
};

// Matches C++ FlattenedLightingParams (96 bytes; the trailing 32B probe-feed
// block is optional — shaders that ignore it still read the first 64B fine).
struct FlattenedLightingParams {
    SurfaceCacheParams sc_params;
    uint total_texels;
    uint card_count;
    uint light_count;
    uint _pad;
    vec4 ddgi_origin_spacing;   // xyz origin, w spacing
    uvec4 ddgi_counts_valid;    // xyz counts, w valid flag
    // T3.3: SC Radiosity gate (x = on) — see Metal twin.
    uvec4 radiosity_gate;       // x = indirect_radiosity_on, yzw reserved
};

// Reconstruct world position from card atlas UV + depth.
vec3 cardTexelToWorld(uvec2 atlas_uv, float depth, SurfaceCacheCard card) {    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5) / float(card.resolution);

    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8) & 0xFFu;

    vec3 local_pos;
    if (axis == 0u) {
        local_pos.x = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.y = (u - 0.5) * card.extent.y * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else if (axis == 1u) {
        local_pos.y = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else {
        local_pos.z = (dir > 0u ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.y = (v - 0.5) * card.extent.y * 2.0;
    }
    return local_pos + card.center.xyz;
}

// Find atlas UV for a world position given a card. Returns true if inside.
bool worldToCardUV(vec3 world_pos, SurfaceCacheCard card, out vec2 out_uv) {
    vec3 local = world_pos - card.center.xyz;
    float u, v, depth;

    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8) & 0xFFu;

    if (axis == 0u) {
        depth = local.x * (dir > 0u ? 1.0 : -1.0);
        u = (local.y / (card.extent.y * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else if (axis == 1u) {
        depth = local.y * (dir > 0u ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else {
        depth = local.z * (dir > 0u ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.y / (card.extent.y * 2.0)) + 0.5;
    }

    if (depth < 0.0 || u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) return false;   // C6: u/v==1.0 bleeds into the next card

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
    return true;
}

// ============================================================================
// C (plan §三.C): unified Surface Cache query contract.
//
// All SC consumers (SurfaceCacheIndirectResolve, DDGITraceFinalize,
// ScreenProbeTraceFinalize) validate candidates the same way:
//   1. bounds        — worldToCardUV (UV coverage on the card plane)
//   2. orientation   — the captured surface must face the QUERY side:
//                      dot(faceNormal, surfaceFacing) > 0 (surfaceFacing is
//                      -rayDir for ray queries, the receiver normal for tile
//                      queries). Two-sided walls get distinct cards per side;
//                      a bounds-passing card facing away describes the wrong
//                      face.
//   3. capture valid — depth texel finite and ≥ 0
//   4. decomposed error vs the reconstructed captured position
//                      (cardTexelToWorld): |along| (card axis depth mismatch —
//                      covers the caller's SDF hit band) and perp (lateral —
//                      must stay inside the texel footprint). The old
//                      extent-proportional single-sided tolerance accepted
//                      captures metres in front of interior hits on big AABB
//                      cards.
//   5. stable choice — min key = perp + 0.5·|along| across ALL candidates
//                      (never first-hit / card-list order dependent)
//   6. valid black   — accepted (a black texel is a measurement); only
//                      NaN/Inf candidate radiance is rejected.
// ============================================================================
vec3 scCardFaceNormal(SurfaceCacheCard card) {
    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8u) & 0xFFu;
    float s = (dir > 0u) ? 1.0 : -1.0;
    return (axis == 0u) ? vec3(s, 0.0, 0.0)
         : (axis == 1u) ? vec3(0.0, s, 0.0)
                        : vec3(0.0, 0.0, s);
}

// One-candidate evaluation. Returns false when the candidate is rejected;
// on true, outRadiance carries the texel radiance (may be black) and outKey
// the selection key. tolFloor covers the caller's query-position error band
// (SDF march threshold); footprint covers texel quantization.
// hitNormalHint（G2，2026-09-16）: 命中点 SDF 梯度（指向空气），长度² > 0.25
// 时启用朝向身份门。粗 SDF march 的 ±voxel×0.5 误差带内，平行近邻面（拱腹
// vs 受光外墙）位置不可分、朝向类可分——拱顶热点借光的治理手段。
// vec3(0) = 不启用（调用方无 SDF 法线来源时保持原行为）。
bool scQueryCardCandidate(SurfaceCacheCard card,
                          vec3 hitPos,
                          vec3 surfaceFacing,
                          float tolFloor,
                          texture2D depthAtlas,
                          texture2D lightingAtlas,
                          vec3 hitNormalHint,
                          out vec3 outRadiance,
                          out float outKey) {
    outRadiance = vec3(0.0);
    outKey = 1e30;

    vec2 atlasUV;
    if (!worldToCardUV(hitPos, card, atlasUV)) return false;

    // 2. orientation — the captured surface's VISIBLE side faces the card
    // plane (n_true = -faceNormal; the eye sits at the plane looking along
    // faceNormal). A query legitimately sees the surface only when it
    // arrives from that side: dot(-faceNormal, -surfaceFacing) < 0.
    // (The first draft had this comparison inverted — it rejected every
    // front-face lookup and starved the transport.)
    if (dot(scCardFaceNormal(card), surfaceFacing) >= 0.0) return false;
    // G2: n_true = -faceNormal 必须与 SDF 外向梯度一致（cos > 0.5，60° 吸收
    // 4m 体素梯度的模糊）；异面/平行邻面 dot ≤ 0 拒绝。
    if (dot(hitNormalHint, hitNormalHint) > 0.25 &&
        dot(scCardFaceNormal(card), hitNormalHint) > -0.5) {
        return false;
    }

    // 3. capture validity.
    uvec2 texel = uvec2(clamp(atlasUV, vec2(0.0), vec2(card.atlas_offset_x + card.resolution - 1u,
                                                       card.atlas_offset_y + card.resolution - 1u)));
    float depth = texelFetch(depthAtlas, ivec2(texel), 0).r;
    if (isnan(depth) || isinf(depth) || depth < 0.0) return false;

    // 4. decomposed position error.
    vec3 captured = cardTexelToWorld(texel, depth, card);
    vec3 delta = captured - hitPos;
    vec3 n = scCardFaceNormal(card);
    float along = dot(delta, n);
    vec3 perpV = delta - n * along;
    float perp = length(perpV);
    float footprint = length(card.extent.xyz) / float(max(card.resolution, 1u));
    float tolAlong = tolFloor + footprint;
    // The caller's uncertainty is a ball of radius ~tolFloor (the SDF hit
    // band projects LATERALLY onto grazing-oriented card planes), plus texel
    // quantization laterally. tolPerp floor of tolFloor — not a constant —
    // was measured starving the transport via grazing rejections.
    float tolPerp = max(footprint * 1.5, tolFloor);
    if (abs(along) > tolAlong || perp > tolPerp) return false;

    // 6. radiance fetch — valid black accepted.
    vec3 c = texelFetch(lightingAtlas, ivec2(texel), 0).rgb;
    if (any(isnan(c)) || any(isinf(c))) return false;
    outRadiance = c;
    outKey = perp + 0.5 * abs(along);
    return true;
}

// T1.10: octEncode/octDecode are canonical in ShaderLib/MathLib.glsl now.
// Textual include keeps this header self-contained for existing consumers.
#include "ShaderLib/MathLib.glsl"

// Hash function for reproducible random (ported from CommonFunction.metal::hash).
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// ============================================================================
// Uniform card grid (T1.3) — canonical query helpers.
//
// The grid_offsets SSBO starts with a 16-u32 header (SurfaceCacheCardGridHeader
// on the C++ side), followed by (cell_count + 1) prefix-sum u32s into the
// grid_cards SSBO. Consumers declare their own buffer blocks (binding numbers
// differ per pass) and use these helpers for decode + cell lookup.
// ============================================================================

struct SCGridParams {
    vec3  grid_min;
    vec3  grid_max;
    uvec3 dims;
    float cell_size;
    uint  card_count;
};

// Decode the 3 header uvec4s (words 0..11 of grid_offsets).
SCGridParams scGridDecode(uvec4 h0, uvec4 h1, uvec4 h2) {
    SCGridParams gp;
    gp.grid_min = uintBitsToFloat(h0.xyz);
    gp.card_count = h0.w;
    gp.grid_max = uintBitsToFloat(h1.xyz);
    gp.cell_size = uintBitsToFloat(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

// World position → linear cell index; 0xFFFFFFFF when outside the grid.
uint scGridCellIndex(SCGridParams gp, vec3 p) {
    vec3 g = (p - gp.grid_min) / gp.cell_size;
    if (g.x < 0.0 || g.y < 0.0 || g.z < 0.0) return 0xFFFFFFFFu;
    uvec3 c = uvec3(g);
    if (c.x >= gp.dims.x || c.y >= gp.dims.y || c.z >= gp.dims.z) return 0xFFFFFFFFu;
    return c.x + gp.dims.x * (c.y + gp.dims.y * c.z);
}

#endif // SURFACE_CACHE_DATA_GLSL
