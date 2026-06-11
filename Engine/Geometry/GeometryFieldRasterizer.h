#pragma once

#include "Geometry/GeometryTypes.h"
#include <vector>

namespace primal::geometry {

struct FieldRasterizeParams {
    math::v3 bounds_min{};
    math::v3 bounds_max{};
    u32 resolution_x{64};
    u32 resolution_y{64};
    u32 resolution_z{1};
    f32 band_width{5.0f};
    bool union_mode{true};
};

struct FieldOutput {
    std::vector<f32> data;
    u32 dim[3]{0, 0, 0};
    math::v3 origin{};
    math::v3 voxel_size{};
};

FieldOutput rasterize(const std::vector<GeometryHandle>& handles,
                      const FieldRasterizeParams& params);

} // namespace primal::geometry
