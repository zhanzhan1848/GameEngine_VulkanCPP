#ifndef SURFACE_CACHE_DATA_GLSL
#define SURFACE_CACHE_DATA_GLSL

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
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

// Matches C++ FlattenedLightingParams (64 bytes).
struct FlattenedLightingParams {
    SurfaceCacheParams sc_params;
    uint total_texels;
    uint card_count;
    uint light_count;
    uint _pad;
};

// Reconstruct world position from card atlas UV + depth.
vec3 cardTexelToWorld(uvec2 atlas_uv, float depth, SurfaceCacheCard card) {
    float u = (float(atlas_uv.x - card.atlas_offset_x) + 0.5) / float(card.resolution);
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

    if (depth < 0.0 || u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return false;

    out_uv.x = float(card.atlas_offset_x) + u * float(card.resolution);
    out_uv.y = float(card.atlas_offset_y) + v * float(card.resolution);
    return true;
}

// Octahedral normal encoding.
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

// Hash function for reproducible random (ported from CommonFunction.metal::hash).
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

#endif // SURFACE_CACHE_DATA_GLSL
