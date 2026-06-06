#pragma once

#include "Graphics/PCG/PCGTypes.h"
#include "Utilities/Math.h"
#include <vector>

namespace primal::graphics::pcg {

// Instance data for GPU draw calls. One per PCG point.
// model_matrix combines Scale and Translation from the point's attributes and position.
// mesh_index selects which mesh asset to draw (maps to ForwardSceneRenderer's mesh list).
struct PCGInstanceData {
    math::m4x4 model_matrix;
    u32 mesh_index{0};
};

// Converts a PCGPointSet into a vector of PCGInstanceData for rendering.
// Reads ScaleX/Y/Z attributes and positions to build model matrices.
// Reads MeshIndex attribute to assign mesh per instance.
//
// Usage:
//   PCGInstanceBuilder builder;
//   std::vector<PCGInstanceData> instances;
//   builder.Build(pointSet, instances);
//   forwardRenderer->SetPCGInstances(instances);
//
// Phase 1 limitation: rotation is not applied to model matrices.
// Only scale and translation are encoded.
class PCGInstanceBuilder {
public:
    void Build(const PCGPointSet& points, std::vector<PCGInstanceData>& out_instances) {
        out_instances.clear();
        out_instances.reserve(points.count);

        for (u32 i = 0; i < points.count; ++i) {
            PCGInstanceData inst;
            inst.mesh_index = static_cast<u32>(points.GetAttr(i, PCGAttr::MeshIndex));

            f32 sx = points.GetAttr(i, PCGAttr::ScaleX);
            f32 sy = points.GetAttr(i, PCGAttr::ScaleY);
            f32 sz = points.GetAttr(i, PCGAttr::ScaleZ);
            if (sx == 0.0f) sx = 1.0f;
            if (sy == 0.0f) sy = 1.0f;
            if (sz == 0.0f) sz = 1.0f;

            // Build model matrix: Scale * Translation (no rotation for Phase 1)
            const auto& pos = points.positions[i];
            simd::float4 col0 = {sx, 0, 0, 0};
            simd::float4 col1 = {0, sy, 0, 0};
            simd::float4 col2 = {0, 0, sz, 0};
            simd::float4 col3 = {pos.x, pos.y, pos.z, 1};
            inst.model_matrix = simd_matrix(col0, col1, col2, col3);

            out_instances.push_back(inst);
        }
    }
};

} // namespace primal::graphics::pcg
