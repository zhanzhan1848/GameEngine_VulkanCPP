#include "Geometry/GeometryFieldRasterizer.h"
#include "Geometry/Geometry.h"
#include "CommonHeaders.h"
#include <cfloat>

namespace primal::geometry {

FieldOutput rasterize(const std::vector<GeometryHandle>& handles,
                      const FieldRasterizeParams& params) {
    FieldOutput output;

    if (handles.empty()) return output;

    const u32 rx = params.resolution_x;
    const u32 ry = params.resolution_y;
    const u32 rz = params.resolution_z;

    if (rx == 0 || ry == 0 || rz == 0) return output;

    output.dim[0] = rx;
    output.dim[1] = ry;
    output.dim[2] = rz;

    const math::v3 extent = {
        params.bounds_max.x - params.bounds_min.x,
        params.bounds_max.y - params.bounds_min.y,
        params.bounds_max.z - params.bounds_min.z
    };

    output.origin = params.bounds_min;
    output.voxel_size = {
        rx > 1 ? extent.x / static_cast<f32>(rx) : 0.0f,
        ry > 1 ? extent.y / static_cast<f32>(ry) : 0.0f,
        rz > 1 ? extent.z / static_cast<f32>(rz) : 0.0f
    };

    const u32 total_voxels = rx * ry * rz;
    output.data.resize(total_voxels, FLT_MAX);

    for (u32 z = 0; z < rz; ++z) {
        for (u32 y = 0; y < ry; ++y) {
            for (u32 x = 0; x < rx; ++x) {
                math::v3 world_pos = {
                    params.bounds_min.x + (rx > 1 ? (static_cast<f32>(x) + 0.5f) * output.voxel_size.x : extent.x * 0.5f),
                    params.bounds_min.y + (ry > 1 ? (static_cast<f32>(y) + 0.5f) * output.voxel_size.y : extent.y * 0.5f),
                    params.bounds_min.z + (rz > 1 ? (static_cast<f32>(z) + 0.5f) * output.voxel_size.z : extent.z * 0.5f)
                };

                f32 min_dist = FLT_MAX;
                for (const GeometryHandle& h : handles) {
                    f32 d = geometry::distance_to(h, world_pos);
                    if (d < min_dist) min_dist = d;
                }

                const u32 voxel_index = z * rx * ry + y * rx + x;

                if (params.union_mode) {
                    // SDF union: store minimum distance
                    output.data[voxel_index] = min_dist;
                } else {
                    // Signed distance: negative inside band
                    output.data[voxel_index] = (min_dist < params.band_width)
                        ? (min_dist - params.band_width)
                        : min_dist;
                }
            }
        }
    }

    return output;
}

} // namespace primal::geometry
