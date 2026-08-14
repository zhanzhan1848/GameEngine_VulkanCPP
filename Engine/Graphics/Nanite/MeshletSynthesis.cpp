#include "MeshletSynthesis.h"
#include <cmath>
#include <cstring>

namespace primal::graphics::nanite {

namespace {
struct PackedV3 { float x, y, z; };
}

void SynthesizeMeshlets(const rhi::RHIMeshAsset& asset, SynthesizedMeshlets& out) {
    out.meshlets.clear();
    out.meshlet_vertices.clear();
    out.meshlet_triangles.clear();
    if (asset.num_indices == 0 || asset.index_buffer.empty() || asset.position_buffer.empty()) {
        return;
    }

    const u32 numVerts = asset.num_vertices;
    const u32 numIndices = asset.num_indices;
    const u32 numTriangles = numIndices / 3;

    out.meshlets.reserve((numTriangles + kMeshletMaxTriangles - 1) / kMeshletMaxTriangles);

    auto readIndex = [&](u32 i) -> u32 {
        const u8* base = asset.index_buffer.data();
        if (asset.index_size == 2) {
            u16 v;
            memcpy(&v, base + i * sizeof(u16), sizeof(u16));
            return v;
        }
        u32 v;
        memcpy(&v, base + i * sizeof(u32), sizeof(u32));
        return v;
    };

    const PackedV3* positions = reinterpret_cast<const PackedV3*>(asset.position_buffer.data());

    utl::vector<u32> globalToLocal(numVerts, u32(-1));
    utl::vector<u32> localVerts;
    localVerts.reserve(kMeshletMaxVertices);
    u32 triangleByteOffset = 0;
    u32 vertexU32Offset = 0;

    auto finalizeMeshlet = [&](u32 triCount) {
        rhi::RHIMeshlet m{};
        m.vertex_offset = vertexU32Offset;
        m.triangle_offset = triangleByteOffset;
        m.vertex_count = (u32)localVerts.size();
        m.triangle_count = triCount;

        // Bounding sphere from meshlet-local vertices.
        math::v3 center{0, 0, 0};
        for (u32 li = 0; li < localVerts.size(); ++li) {
            u32 gi = localVerts[li];
            center.x += positions[gi].x;
            center.y += positions[gi].y;
            center.z += positions[gi].z;
        }
        const f32 invN = 1.0f / std::max(1u, (u32)localVerts.size());
        center = center * invN;

        f32 maxDistSq = 0.0f;
        for (u32 li = 0; li < localVerts.size(); ++li) {
            u32 gi = localVerts[li];
            f32 dx = positions[gi].x - center.x;
            f32 dy = positions[gi].y - center.y;
            f32 dz = positions[gi].z - center.z;
            maxDistSq = std::max(maxDistSq, dx * dx + dy * dy + dz * dz);
        }
        m.center[0] = center.x; m.center[1] = center.y; m.center[2] = center.z;
        m.radius = std::sqrt(maxDistSq);

        // Conservative cone — never reject visible meshlets.
        m.cone_apex[0] = center.x; m.cone_apex[1] = center.y; m.cone_apex[2] = center.z;
        m.cone_axis[0] = 0.0f; m.cone_axis[1] = 0.0f; m.cone_axis[2] = 1.0f;
        m.cone_cutoff = 1.0f;
        m.padding = 0;

        out.meshlets.push_back(m);

        for (u32 gi : localVerts) {
            out.meshlet_vertices.push_back(gi);
        }
        vertexU32Offset += (u32)localVerts.size();

        for (u32 gi : localVerts) {
            globalToLocal[gi] = u32(-1);
        }
        localVerts.clear();
        triangleByteOffset += triCount * 3;
    };

    u32 trianglesInCurrentMeshlet = 0;
    u32 triIndex = 0;
    while (triIndex < numTriangles) {
        u32 i0 = readIndex(triIndex * 3 + 0);
        u32 i1 = readIndex(triIndex * 3 + 1);
        u32 i2 = readIndex(triIndex * 3 + 2);

        if (i0 >= numVerts || i1 >= numVerts || i2 >= numVerts) {
            triIndex++;
            continue;
        }

        u32 newVerts = 0;
        if (globalToLocal[i0] == u32(-1)) ++newVerts;
        if (globalToLocal[i1] == u32(-1)) ++newVerts;
        if (globalToLocal[i2] == u32(-1)) ++newVerts;

        if (localVerts.size() + newVerts > kMeshletMaxVertices || trianglesInCurrentMeshlet >= kMeshletMaxTriangles) {
            finalizeMeshlet(trianglesInCurrentMeshlet);
            trianglesInCurrentMeshlet = 0;
        }

        auto assignLocal = [&](u32 gi) -> u8 {
            if (globalToLocal[gi] == u32(-1)) {
                globalToLocal[gi] = (u32)localVerts.size();
                localVerts.push_back(gi);
            }
            return (u8)globalToLocal[gi];
        };
        u8 l0 = assignLocal(i0);
        u8 l1 = assignLocal(i1);
        u8 l2 = assignLocal(i2);
        out.meshlet_triangles.push_back(l0);
        out.meshlet_triangles.push_back(l1);
        out.meshlet_triangles.push_back(l2);
        trianglesInCurrentMeshlet++;
        triIndex++;
    }
    if (trianglesInCurrentMeshlet > 0) {
        finalizeMeshlet(trianglesInCurrentMeshlet);
    }
}

} // namespace primal::graphics::nanite
