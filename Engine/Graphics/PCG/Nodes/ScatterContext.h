#pragma once

#include "Graphics/PCG/PCGTypes.h"
#include <cmath>
#include <cstdlib>

namespace primal::graphics::pcg {

// Shared utility functions for scatter-related nodes.
// Called internally by FieldScatterNode and DensityFilterNode.
//
// ApplyJitter: adds horizontal noise to point positions
// ApplyDensityFilter: removes points outside a density range (compacts in-place)
// WriteDefaultAttributes: initializes Density=1, Scale=(1,1,1), MeshIndex=0 for all points
struct ScatterContext {
    u32 seed{0};
    math::v3 bounds_min{};
    math::v3 bounds_max{};

    // Add random horizontal displacement to point positions.
    // amount: maximum displacement in x/z (uniform random in [-amount/2, amount/2])
    static void ApplyJitter(PCGPointSet& points, f32 amount, u32 seed) {
        std::srand(seed);
        for (u32 i = 0; i < points.count; ++i) {
            f32 jx = (std::rand() / f32(RAND_MAX) - 0.5f) * amount;
            f32 jz = (std::rand() / f32(RAND_MAX) - 0.5f) * amount;
            points.positions[i].x += jx;
            points.positions[i].z += jz;
        }
    }

    // Remove points whose Density attribute falls outside [min_density, max_density].
    // Compacts positions and attrs arrays in-place.
    static void ApplyDensityFilter(PCGPointSet& points, f32 min_density, f32 max_density) {
        u32 write = 0;
        for (u32 i = 0; i < points.count; ++i) {
            f32 d = points.GetAttr(i, PCGAttr::Density);
            if (d >= min_density && d <= max_density) {
                if (write != i) {
                    points.positions[write] = points.positions[i];
                    for (u32 a = 0; a < points.attr_stride; ++a) {
                        points.attrs[write * points.attr_stride + a] =
                            points.attrs[i * points.attr_stride + a];
                    }
                }
                ++write;
            }
        }
        points.count = write;
        points.positions.resize(write);
        points.attrs.resize(static_cast<size_t>(write) * points.attr_stride);
    }

    // Initialize default attribute values for all points.
    static void WriteDefaultAttributes(PCGPointSet& points, u32 seed) {
        std::srand(seed);
        for (u32 i = 0; i < points.count; ++i) {
            points.SetAttr(i, PCGAttr::Density, 1.0f);
            points.SetAttr(i, PCGAttr::ScaleX, 1.0f);
            points.SetAttr(i, PCGAttr::ScaleY, 1.0f);
            points.SetAttr(i, PCGAttr::ScaleZ, 1.0f);
            points.SetAttr(i, PCGAttr::Seed, static_cast<f32>(std::rand()));
            points.SetAttr(i, PCGAttr::MeshIndex, 0.0f);
        }
    }
};

} // namespace primal::graphics::pcg
