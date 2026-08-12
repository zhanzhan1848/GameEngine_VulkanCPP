#include <metal_stdlib>
using namespace metal;
#include "SurfaceCacheData.metal"

kernel void surfaceCacheDilate(
    uint2 global_id [[thread_position_in_grid]],
    texture2d<float, access::read>  depth_in    [[texture(0)]],
    texture2d<float, access::write> depth_out   [[texture(1)]],
    constant SurfaceCacheParams& params         [[buffer(1)]])
{
    if (global_id.x >= params.atlas_size || global_id.y >= params.atlas_size) return;

    float center = depth_in.read(global_id).r;
    if (center > 0.0) {
        depth_out.write(float4(center, 0, 0, 1), global_id);
        return;
    }

    float min_depth = 1e10;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2 nb = int2(global_id) + int2(dx, dy);
            if (nb.x < 0 || nb.y < 0 || nb.x >= int(params.atlas_size) || nb.y >= int(params.atlas_size)) continue;
            float d = depth_in.read(uint2(nb)).r;
            if (d > 0.0) min_depth = min(min_depth, d);
        }
    }
    depth_out.write(float4(min_depth < 1e10 ? min_depth : 0.0, 0, 0, 1), global_id);
}
