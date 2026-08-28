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
    uint32_t  lookup_count;     // number of CardLookup entries (mesh instances)
    uint32_t  _pad;
};

// Per-card dispatch info for flattened texel dispatch.
// CPU builds prefix-sum array, GPU binary-searches to map thread_id → card → atlas UV.
struct CardDispatchInfo {
    uint32_t texel_offset;     // prefix-sum: starting texel index for this card
    uint32_t texel_count;      // resolution * resolution
    uint32_t resolution;       // card texel resolution (square)
    uint32_t atlas_offset_x;   // atlas pixel origin X
    uint32_t atlas_offset_y;   // atlas pixel origin Y
    uint32_t card_index;       // original index in the full SurfaceCacheCard buffer
    uint32_t _pad[2];
};
static_assert(sizeof(CardDispatchInfo) == 32, "CardDispatchInfo must be 32 bytes");

// Params for merged lighting pass (replaces SurfaceCacheLightingParams).
// Eliminates tile-based dispatch; uses flattened per-card texel dispatch instead.
struct FlattenedLightingParams {
    SurfaceCacheParams sc_params;   // 48 bytes
    uint32_t           total_texels; // sum of all card resolutions squared
    uint32_t           card_count;   // number of active cards in dispatch
    uint32_t           light_count;
    uint32_t           _pad;
    // Probe-fed indirect (Lumen hierarchy): near-volume grid so LightEval can
    // gather incoming irradiance from the DDGI probes (appended at the end —
    // backends whose shaders predate this field keep reading the first 64B).
    float              ddgi_origin_spacing[4];  // xyz origin, w spacing
    uint32_t           ddgi_counts_valid[4];    // xyz counts, w valid flag
};

struct SurfaceCacheFrameData {
    math::v3   camera_position;
    float      _pad;
    uint32_t   frame_index;
    uint32_t   light_count;
    float      _pad2[2];

    // T3.2: light injected by the caller (pipeline passes settings_.lighting);
    // defaults preserve the old hardcoded directional light.
    math::v3   light_direction{0.707f, -1.0f, 0.408f};
    float      _pad3;
    math::v3   light_color{5.0f, 5.0f, 5.0f};
    float      _pad4;

    // GBuffer-gather capture inputs (Vulkan Capture path): main-pass GBuffer
    // handles + inverse view-projection for world-position reconstruction.
    math::m4x4 inv_view_projection;
    uint32_t   render_width  = 0;
    uint32_t   render_height = 0;
    rhi::ResourceHandle gbuffer_depth  { rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_albedo { rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle gbuffer_normal { rhi::handles::INVALID_RESOURCE };

    // Probe-fed indirect (Lumen hierarchy): near-volume irradiance (9 vec4 per
    // probe, previous-frame slot — SC runs before the DDGI update) + grid
    // params. ddgi_valid = 0 leaves LightEval on the legacy self-feedback.
    rhi::ResourceHandle ddgi_irradiance{ rhi::handles::INVALID_RESOURCE };
    math::v3   ddgi_origin{};
    float      ddgi_spacing = 0.0f;
    uint32_t   ddgi_counts[3] = {0, 0, 0};
    uint32_t   ddgi_valid = 0;
};

// ============================================================================
// Uniform card grid (T1.3): world-space cell → card index list.
//
// Layout: a single u32 buffer whose first 16 words are this header, followed
// by (cell_count + 1) prefix-sum offsets into a second buffer of card indices.
// Floats are bit-cast into u32 so shaders need no extra CB/UBO plumbing —
// every consumer binds just the two SSBOs and self-describes from the header.
// ============================================================================

struct SurfaceCacheCardGridHeader {
    uint32_t min_bits[3];      // float bits of grid_min.xyz
    uint32_t card_count;       // total cards indexed by the grid
    uint32_t max_bits[3];      // float bits of grid_max.xyz
    uint32_t cell_size_bits;   // float bits of cell_size (world units)
    uint32_t dims[3];          // cells per axis
    uint32_t header_words;     // = 16 (u32 words before the offsets array)
    uint32_t reserved[4];
};
static_assert(sizeof(SurfaceCacheCardGridHeader) == 64, "Header must stay 16 u32 words");

constexpr uint32_t kMaxCardsPerGridCell = 8;   // build-side cap; shaders mirror this

struct SurfaceCacheOutput {
    rendergraph::RGResourceHandle lighting_atlas;
};

} // namespace
