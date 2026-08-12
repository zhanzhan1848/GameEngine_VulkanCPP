#include <metal_stdlib>
using namespace metal;
#include "CommonTypes.metal"
#include "Lumen/SurfaceCacheData.metal"

struct LightCullParams {
    SurfaceCacheParams sc_params;
    uint tiles_x;
    uint tile_size;
    uint light_count;
    uint tiles_y;
};

kernel void surfaceCacheLightCull(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read> depth_atlas  [[texture(0)]],
    texture2d<float, access::read> normal_atlas [[texture(1)]],
    constant LightCullParams& params            [[buffer(1)]],
    constant LightInfo* lights                  [[buffer(2)]],
    device uint4* tile_light_assignment         [[buffer(3)]])
{
    uint tile_x = global_id.x;
    uint tile_y = global_id.y;
    if (tile_x >= params.tiles_x || tile_y >= params.tiles_y) return;

    uint tile_idx = tile_y * params.tiles_x + tile_x;
    uint tile_size = params.tile_size;
    uint2 tile_origin = uint2(tile_x * tile_size, tile_y * tile_size);

    // Apple Silicon bandwidth optimization:
    // Full 8x8 scan = 64 depth reads/thread × 65K threads = 4.2M pixel reads (over ~2M limit).
    // Instead, use sparse 4×4 subsampling (16 reads) for occupancy check,
    // then full scan only for occupied tiles that need normal data for point lights.
    float min_depth = 1e10;
    float max_depth = 0.0;

    // Phase 1: Sparse depth scan (every other texel = 4×4 = 16 reads)
    for (uint dy = 0; dy < tile_size; dy += 2) {
        for (uint dx = 0; dx < tile_size; dx += 2) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size || texel.y >= params.sc_params.atlas_size) continue;
            float d = depth_atlas.read(texel).r;
            if (d > 0.0) {
                min_depth = min(min_depth, d);
                max_depth = max(max_depth, d);
            }
        }
    }

    // Skip tiles with no valid depth data
    if (max_depth <= 0.0) {
        tile_light_assignment[tile_idx] = uint4(0xFFFFFFFF);
        return;
    }

    // Phase 2: Sparse normal scan for occupied tiles (point/spot light culling).
    // Use same 4×4 sparse pattern instead of full 8×8 to keep bandwidth bounded.
    // Full scan at high occupancy: 65K × 128 reads = 8.3M pixel reads (way over ~2M limit).
    // Sparse scan: 65K × 16 reads = 1.05M (within limit).
    float3 avg_normal = float3(0.0);
    uint valid_count = 0;

    for (uint dy = 0; dy < tile_size; dy += 2) {
        for (uint dx = 0; dx < tile_size; dx += 2) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size || texel.y >= params.sc_params.atlas_size) continue;
            float d = depth_atlas.read(texel).r;
            if (d > 0.0) {
                float2 enc = normal_atlas.read(texel).rg;
                avg_normal += octDecode(enc);
                valid_count++;
            }
        }
    }

    uint4 assigned = uint4(0xFFFFFFFF);
    uint assign_count = 0;

    if (valid_count > 0) {
        avg_normal = normalize(avg_normal);
        for (uint li = 0; li < params.light_count && assign_count < 4; ++li) {
            bool visible = false;
            uint lightType = uint(lights[li].direction.w);
            if (lightType == 1) {
                visible = true;
            } else {
                float light_range = lights[li].position.w;
                if (max_depth < light_range) visible = true;
            }
            if (visible) {
                switch (assign_count) {
                    case 0: assigned.x = li; break;
                    case 1: assigned.y = li; break;
                    case 2: assigned.z = li; break;
                    case 3: assigned.w = li; break;
                }
                assign_count++;
            }
        }
    }
    tile_light_assignment[tile_idx] = assigned;
}
