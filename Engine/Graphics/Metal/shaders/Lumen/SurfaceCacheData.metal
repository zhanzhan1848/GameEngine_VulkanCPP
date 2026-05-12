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
    uint    _pad[2];
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

// Reconstruct world position from card atlas UV + depth
static float3 cardTexelToWorld(
    uint2 atlas_uv,
    float depth,
    constant SurfaceCacheCard& card)
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

// Find atlas UV for a world position given a card
static bool worldToCardUV(
    float3 world_pos,
    constant SurfaceCacheCard& card,
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

    if (depth < 0.0 || u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return false;

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
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
    n.xy += select(float2(-t), float2(t), n.xy >= 0.0);
    return normalize(n);
}
