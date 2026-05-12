#include <metal_stdlib>
using namespace metal;
#include "CommonTypes.metal"
#include "Lumen/SurfaceCacheData.metal"

struct LightCullParams {
    SurfaceCacheParams sc_params;
    uint light_count;
    uint tile_size;
    uint tiles_x;
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

    float min_depth = 1e10;
    float max_depth = 0.0;
    float3 avg_normal = float3(0.0);
    uint valid_count = 0;

    for (uint dy = 0; dy < tile_size; ++dy) {
        for (uint dx = 0; dx < tile_size; ++dx) {
            uint2 texel = tile_origin + uint2(dx, dy);
            if (texel.x >= params.sc_params.atlas_size || texel.y >= params.sc_params.atlas_size) continue;
            float d = depth_atlas.read(texel).r;
            if (d > 0.0) {
                min_depth = min(min_depth, d);
                max_depth = max(max_depth, d);
                float2 enc = normal_atlas.read(texel).rg;
                avg_normal += octDecode(enc);
                valid_count++;
            }
        }
    }

    uint4 assigned = uint4(0xFFFFFFFF);
    uint assign_count = 0;

    if (valid_count > 0 && max_depth > 0.0) {
        avg_normal = normalize(avg_normal);
        for (uint li = 0; li < params.light_count && assign_count < 4; ++li) {
            bool visible = false;
            uint lightType = uint(lights[li].direction.w);
            if (lightType == 1) {
                visible = true;
            } else {
                float light_range = lights[li].position.w; // radius
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
