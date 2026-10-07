#pragma once
#include "BVH.hpp"
#include "Graphics/RHI/Core/RHIMeshAsset.h"
#include <span>
#include <memory>
#include <vector>

namespace primal::graphics::ray_geometry {
using Node = utl::BVH::PackedNode;
using Triangle = utl::BVH::PackedTriangle;
struct MeshBLAS {
    u64 geometry_digest{};
    primal::utl::vector<Node> nodes;
    primal::utl::vector<Triangle> triangles;
};
// Reject malformed/nonfinite geometry before calling the existing CPU builder.
std::shared_ptr<const MeshBLAS> BuildMeshBLAS(const rhi::RHIMeshAsset& mesh);
struct InstanceInput {
    std::shared_ptr<const MeshBLAS> mesh;
    math::m4x4 world;
    u32 instance_id;
};
struct Instance {
    float inverse_rows[3][4]{};
    u32 root{}, instance_id{}, pad[2]{};
};
static_assert(sizeof(Instance)==64);
struct SceneBVH {
    std::vector<Node> top, bottom;
    std::vector<Triangle> triangles;
    std::vector<Instance> instances;
    // Complete scene or failure: incomplete geometry must never certify sky.
    bool Build(std::span<const InstanceInput> inputs);
};
struct Hit { float t{}, u{}, v{}; u32 instance_id{~0u}, triangle_id{~0u}; u64 geometry_digest{}; };
// 0 = segment miss, 1 = nearest opaque two-sided triangle, -1 = invalid/overflow.
int Trace(const SceneBVH&, const float origin[3], const float direction[3],
          float tmin, float tmax, Hit&);
}
