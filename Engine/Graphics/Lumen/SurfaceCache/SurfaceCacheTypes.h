#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::lumen {

// Uses math::v4 instead of math::v3 to guarantee 16-byte alignment (matches DDGIVolumeData pattern)
struct SurfaceCacheCard {
    math::v4  center;         // xyz = card center in mesh local space, w unused
    math::v4  extent;         // xyz = half-extents from AABB, w unused
    uint32_t  axis_direction; // low byte = axis (0=X,1=Y,2=Z), high byte = direction (0/1)
    uint16_t  resolution;     // Texel resolution in atlas
    uint16_t  _pad0;
    uint32_t  atlas_offset_x;
    uint32_t  atlas_offset_y;
    uint32_t  mesh_instance_id;
    uint32_t  _pad1[3];
};
static_assert(sizeof(SurfaceCacheCard) == 64, "SurfaceCacheCard must be 64 bytes");

struct SurfaceCacheCardLookup {
    math::v4  aabb_min;      // xyz = min, w unused
    math::v4  aabb_max;      // xyz = max, w unused
    uint32_t  card_start;
    uint32_t  card_count;
    uint32_t  _pad[2];
};
static_assert(sizeof(SurfaceCacheCardLookup) == 48, "SurfaceCacheCardLookup must be 48 bytes");

struct SurfaceCacheParams {
    uint32_t  atlas_size;
    uint32_t  page_size;
    uint32_t  capture_budget_pages;
    uint32_t  max_cards;
    float     update_distance;
    float     importance_weight;
    uint32_t  max_lights_per_tile;
    uint32_t  indirect_rays_per_probe;
    float     indirect_temporal_weight;
    float     indirect_near_distance;
    uint32_t  _pad[2];
};

struct SurfaceCacheFrameData {
    math::v3   camera_position;
    float      _pad;
    uint32_t   frame_index;
    uint32_t   light_count;
    float      _pad2[2];
};

struct SurfaceCacheOutput {
    rendergraph::RGResourceHandle lighting_atlas;
};

} // namespace
