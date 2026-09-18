#ifndef SURFACE_CACHE_DATA_METAL
#define SURFACE_CACHE_DATA_METAL

#include <metal_stdlib>
using namespace metal;

// Uses float4 for 16-byte alignment (matches DDGIVolumeData pattern)
struct SurfaceCacheCard {
    float4   center;         // xyz = center, w unused
    float4   extent;         // xyz = half-extents, w unused
    uint     axis_direction; // low byte = axis, high byte = direction
    uint16_t resolution;
    uint16_t _pad0;
    uint     atlas_offset_x;
    uint     atlas_offset_y;
    uint     mesh_instance_id;
    uint     _pad1[3];
};

struct SurfaceCacheCardLookup {
    float4   aabb_min;      // xyz = min, w unused
    float4   aabb_max;      // xyz = max, w unused
    uint     card_start;
    uint     card_count;
    uint     _pad[2];
};

struct SurfaceCacheParams {
    uint    atlas_size;
    uint    page_size;
    uint    capture_budget_pages;
    uint    max_cards;
    float   update_distance;
    float   importance_weight;
    uint    max_lights_per_tile;
    uint    indirect_rays_per_probe;
    float   indirect_temporal_weight;
    float   indirect_near_distance;
    uint    lookup_count;     // number of CardLookup entries (mesh instances)
    uint    _pad;
};

// Shared light info for LightCull and LightEval shaders
// Uses float4 (not float3) to guarantee 16-byte alignment matches C++ layout.
// Metal float3 has alignment=16 but size varies (12 or 16 depending on context),
// causing field offsets to diverge from C++ float[3] — same bug as DDGIVolumeData.
struct LightInfo {
    float4  position;     // xyz = position, w = radius
    float4  color;        // xyz = color, w = unused
    float4  direction;    // xyz = direction, w = type (as float, cast to uint when needed)
};

// Per-card dispatch info for flattened texel dispatch (matches C++ CardDispatchInfo).
struct CardDispatchInfo {
    uint texel_offset;     // prefix-sum: starting texel index
    uint texel_count;      // resolution * resolution
    uint resolution;       // card texel resolution (square)
    uint atlas_offset_x;   // atlas pixel origin X
    uint atlas_offset_y;   // atlas pixel origin Y
    uint card_index;       // original index in the full SurfaceCacheCard buffer
    uint _pad[2];
};

// Params for merged lighting pass (matches C++ FlattenedLightingParams).
struct FlattenedLightingParams {
    SurfaceCacheParams sc_params;  // 48 bytes
    uint total_texels;
    uint card_count;
    uint light_count;
    uint _pad;
    // T8.13 sync: probe-fed indirect (Lumen hierarchy) — near-volume grid so
    // LightEval can gather incoming irradiance from the DDGI probes. Appended
    // at the end exactly like the C++/GLSL structs; Metal shaders predating
    // this field kept reading the first 64B.
    float4 ddgi_origin_spacing;    // xyz origin, w spacing
    uint4  ddgi_counts_valid;      // xyz counts, w valid flag
    // T3.3: SC Radiosity gate (x = on) — indirect_out contains valid data
    // only when SC_INDIRECT=1 dispatched the trace/resolve this session.
    uint4  radiosity_gate;         // x = indirect_radiosity_on, y = radiosity weight 档,
                                   // z = tile size, w = openSky 分类开关（GI 根治 Phase 3.2）
};

// Reconstruct world position from card atlas UV + depth
// The card is passed BY VALUE: MSL forbids address-space-agnostic
// references, and callers hold cards in constant/device space.
static float3 cardTexelToWorld(
    uint2 atlas_uv,
    float depth,
    SurfaceCacheCard card)
{
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
    float v = (float(atlas_uv.y - card.atlas_offset_y) + 0.5) / float(card.resolution);

    uint axis = card.axis_direction & 0xFF;
    uint dir = (card.axis_direction >> 8) & 0xFF;

    float3 local_pos;
    if (axis == 0) {
        local_pos.x = (dir ? 1.0 : -1.0) * depth;
        local_pos.y = (u - 0.5) * card.extent.y * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else if (axis == 1) {
        local_pos.y = (dir ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.z = (v - 0.5) * card.extent.z * 2.0;
    } else {
        local_pos.z = (dir ? 1.0 : -1.0) * depth;
        local_pos.x = (u - 0.5) * card.extent.x * 2.0;
        local_pos.y = (v - 0.5) * card.extent.y * 2.0;
    }
    return local_pos + card.center.xyz;
}

// Find atlas UV for a world position given a card (card by value — see
// cardTexelToWorld's address-space note).
static bool worldToCardUV(
    float3 world_pos,
    SurfaceCacheCard card,
    thread float2& out_uv)
{
    float3 local = world_pos - card.center.xyz;
    float u, v, depth;

    uint axis = card.axis_direction & 0xFF;
    uint dir = (card.axis_direction >> 8) & 0xFF;

    if (axis == 0) {
        depth = local.x * (dir ? 1.0 : -1.0);
        u = (local.y / (card.extent.y * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else if (axis == 1) {
        depth = local.y * (dir ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else {
        depth = local.z * (dir ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.y / (card.extent.y * 2.0)) + 0.5;
    }

    if (depth < 0.0 || u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) return false;   // C6: u/v==1.0 bleeds into the next card

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
    return true;
}

// ============================================================================
// C (plan §三.C): unified Surface Cache query contract — MSL twin of
// SurfaceCacheData.glsl::scQueryCardCandidate. Bounds → side orientation →
// capture validity → decomposed along/perp error → min-key selection;
// valid black is a measurement, only NaN/Inf candidate radiance rejects.
// ============================================================================
static float3 scCardFaceNormal(SurfaceCacheCard card) {
    uint axis = card.axis_direction & 0xFFu;
    uint dir  = (card.axis_direction >> 8u) & 0xFFu;
    float s = (dir > 0u) ? 1.0f : -1.0f;
    return (axis == 0u) ? float3(s, 0.0f, 0.0f)
         : (axis == 1u) ? float3(0.0f, s, 0.0f)
                        : float3(0.0f, 0.0f, s);
}

// Card by value (64B — MSL forbids address-space-agnostic references) and
// texture params templated: callers hold atlases with different access
// qualifiers (sample in DDGI, read in resolve/SPGI); .read() works for both.
template <typename TEXD, typename TEXL>
// hitNormalHint（G2，2026-09-16，与 GLSL 孪生同步）: 命中点 SDF 梯度，
// length² > 0.25 时启用朝向身份门；float3(0) = 不启用。
static bool scQueryCardCandidate(SurfaceCacheCard card,
                                 float3 hitPos,
                                 float3 surfaceFacing,
                                 float tolFloor,
                                 TEXD depthAtlas,
                                 TEXL lightingAtlas,
                                 float3 hitNormalHint,
                                 thread float3& outRadiance,
                                 thread float& outKey) {
    outRadiance = float3(0.0f);
    outKey = 1e30f;

    float2 atlasUV;
    if (!worldToCardUV(hitPos, card, atlasUV)) return false;

    // n_true = -faceNormal (faces the card plane); see GLSL twin for sign.
    if (dot(scCardFaceNormal(card), surfaceFacing) >= 0.0f) return false;
    // G2: 朝向身份门（cos > 0.5 容差，吸收粗 SDF 梯度模糊）
    if (dot(hitNormalHint, hitNormalHint) > 0.25f &&
        dot(scCardFaceNormal(card), hitNormalHint) > -0.5f) return false;

    uint2 texel = uint2(clamp(atlasUV, float2(0.0f),
                              float2(card.atlas_offset_x + card.resolution - 1u,
                                     card.atlas_offset_y + card.resolution - 1u)));
    float depth = depthAtlas.read(texel).r;
    if (isinf(depth) || isnan(depth) || depth < 0.0f) return false;

    float3 captured = cardTexelToWorld(texel, depth, card);
    float3 n = scCardFaceNormal(card);
    float3 delta = captured - hitPos;
    float along = dot(delta, n);
    float perp = length(delta - n * along);
    float footprint = length(card.extent.xyz) / float(max(uint(card.resolution), 1u));
    float tolAlong = tolFloor + footprint;
    // Uncertainty ball: the SDF band projects laterally on grazing cards.
    float tolPerp = max(footprint * 1.5f, tolFloor);
    if (fabs(along) > tolAlong || perp > tolPerp) return false;

    float3 c = lightingAtlas.read(texel).xyz;
    if (any(isnan(c)) || any(isinf(c))) return false;
    outRadiance = c;
    outKey = perp + 0.5f * fabs(along);
    return true;
}

// Octahedral normal encoding (matches DDGI pattern)
static float2 octEncode(float3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    float2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * select(float2(-1.0), float2(1.0), result.xy >= 0.0);
    }
    return result * 0.5 + 0.5;
}

static float3 octDecode(float2 f) {
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    // Inverse of the replace-fold in octEncode: n.xy >= 0 subtracts t, else adds.
    n.xy += select(float2(t), float2(-t), n.xy >= 0.0);
    return normalize(n);
}

// Device-addressable variant of worldToCardUV for use in compute shaders
// where card data is in device (not constant) address space.
static bool worldToCardUVDevice(float3 world_pos,
                                 device const SurfaceCacheCard& card,
                                 thread float2& out_uv);

// ============================================================================
// Uniform card grid (T1.3) — mirrors SurfaceCacheData.glsl helpers.
// grid_offsets buffer: 16-u32 header + (cell_count + 1) prefix sums into
// grid_cards. Buffers may exceed constant-address limits → device space.
// ============================================================================

struct SCGridParams {
    float3 grid_min;
    float3 grid_max;
    uint3  dims;
    float  cell_size;
    uint   card_count;
};

static SCGridParams scGridDecode(uint4 h0, uint4 h1, uint4 h2) {
    SCGridParams gp;
    gp.grid_min = as_type<float3>(uint3(h0.x, h0.y, h0.z));
    gp.card_count = h0.w;
    gp.grid_max = as_type<float3>(uint3(h1.x, h1.y, h1.z));
    gp.cell_size = as_type<float>(h1.w);
    gp.dims = h2.xyz;
    return gp;
}

// World position → linear cell index; 0xFFFFFFFF when outside the grid.
static uint scGridCellIndex(SCGridParams gp, float3 p) {
    float3 g = (p - gp.grid_min) / gp.cell_size;
    if (g.x < 0.0f || g.y < 0.0f || g.z < 0.0f) return 0xFFFFFFFFu;
    uint3 c = uint3(g);
    if (c.x >= gp.dims.x || c.y >= gp.dims.y || c.z >= gp.dims.z) return 0xFFFFFFFFu;
    return c.x + gp.dims.x * (c.y + gp.dims.y * c.z);
}

// Device-addressable variant of worldToCardUV for use in compute shaders
// where card data is in device (not constant) address space.
static bool worldToCardUVDevice(float3 world_pos,
                                 device const SurfaceCacheCard& card,
                                 thread float2& out_uv)
{
    float3 local = world_pos - card.center.xyz;
    float u, v, depth;
    uint axis = card.axis_direction & 0xFF;
    uint dir = (card.axis_direction >> 8) & 0xFF;

    if (axis == 0) {
        depth = local.x * (dir ? 1.0 : -1.0);
        u = (local.y / (card.extent.y * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else if (axis == 1) {
        depth = local.y * (dir ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.z / (card.extent.z * 2.0)) + 0.5;
    } else {
        depth = local.z * (dir ? 1.0 : -1.0);
        u = (local.x / (card.extent.x * 2.0)) + 0.5;
        v = (local.y / (card.extent.y * 2.0)) + 0.5;
    }

    if (depth < 0.0 || u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0) return false;   // C6: u/v==1.0 bleeds into the next card

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
    return true;
}

#endif // SURFACE_CACHE_DATA_METAL
