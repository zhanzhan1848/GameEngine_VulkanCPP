#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMeshAsset.h"
#include <vector>
#include <cmath>

namespace primal::graphics::pcg {

struct MeshSurfaceData {
    struct Triangle {
        math::v3 v0, v1, v2;
        math::v3 n0, n1, n2;
        f32 area;
    };

    std::vector<Triangle> triangles;
    std::vector<f32> cum_area; // Cumulative area CDF

    static MeshSurfaceData FromRHIMeshAsset(const rhi::RHIMeshAsset& asset) {
        MeshSurfaceData data;
        if (asset.num_vertices == 0 || asset.num_indices < 3) return data;

        const u8* pos = asset.position_buffer.data();
        const u8* elem = asset.element_buffer.data();
        const u32 vertCount = asset.num_vertices;
        const u32 elemStride = elem ? (u32)(asset.element_buffer.size() / vertCount) : 0;

        // Position accessor: packed float3, 12B stride
        auto getPos = [&](u32 idx) -> math::v3 {
            if (idx >= vertCount) return {0, 0, 0};
            f32 p[3];
            memcpy(p, pos + idx * 12, 12);
            return {p[0], p[1], p[2]};
        };

        // Normal accessor: first 12B of element (3 floats)
        auto getNorm = [&](u32 idx) -> math::v3 {
            if (idx >= vertCount || elemStride < 12) return {0, 1, 0};
            f32 n[3];
            memcpy(n, elem + idx * elemStride, 12);
            // Normalize
            f32 len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-6f) { n[0]/=len; n[1]/=len; n[2]/=len; }
            else { n[0]=0; n[1]=1; n[2]=0; }
            return {n[0], n[1], n[2]};
        };

        // Index accessor
        auto getIndex = [&](u32 idx) -> u32 {
            if (asset.index_size == 2) {
                return reinterpret_cast<const u16*>(asset.index_buffer.data())[idx];
            }
            return reinterpret_cast<const u32*>(asset.index_buffer.data())[idx];
        };

        u32 triCount = asset.num_indices / 3;
        data.triangles.reserve(triCount);
        f32 cumArea = 0;

        for (u32 t = 0; t < triCount; ++t) {
            u32 i0 = getIndex(t * 3);
            u32 i1 = getIndex(t * 3 + 1);
            u32 i2 = getIndex(t * 3 + 2);

            Triangle tri;
            tri.v0 = getPos(i0); tri.v1 = getPos(i1); tri.v2 = getPos(i2);
            tri.n0 = getNorm(i0); tri.n1 = getNorm(i1); tri.n2 = getNorm(i2);

            // Area = 0.5 * |cross(v1-v0, v2-v0)|
            math::v3 e1{tri.v1.x-tri.v0.x, tri.v1.y-tri.v0.y, tri.v1.z-tri.v0.z};
            math::v3 e2{tri.v2.x-tri.v0.x, tri.v2.y-tri.v0.y, tri.v2.z-tri.v0.z};
            math::v3 cross{e1.y*e2.z-e1.z*e2.y, e1.z*e2.x-e1.x*e2.z, e1.x*e2.y-e1.y*e2.x};
            tri.area = 0.5f * std::sqrt(cross.x*cross.x + cross.y*cross.y + cross.z*cross.z);

            cumArea += tri.area;
            data.cum_area.push_back(cumArea);
            data.triangles.push_back(tri);
        }

        return data;
    }

    u32 SelectTriangle(f32 random_value) const {
        if (cum_area.empty()) return 0;
        f32 target = random_value * cum_area.back();
        // Binary search
        u32 lo = 0, hi = (u32)cum_area.size() - 1;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;
            if (cum_area[mid] < target) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    math::v3 SamplePoint(u32 tri_idx, f32 r1, f32 r2) const {
        if (tri_idx >= triangles.size()) return {0, 0, 0};
        auto& t = triangles[tri_idx];
        f32 sqR1 = std::sqrt(r1);
        f32 b0 = 1.0f - sqR1;
        f32 b1 = sqR1 * (1.0f - r2);
        f32 b2 = sqR1 * r2;
        return {
            b0*t.v0.x + b1*t.v1.x + b2*t.v2.x,
            b0*t.v0.y + b1*t.v1.y + b2*t.v2.y,
            b0*t.v0.z + b1*t.v1.z + b2*t.v2.z
        };
    }

    math::v3 SampleNormal(u32 tri_idx, f32 r1, f32 r2) const {
        if (tri_idx >= triangles.size()) return {0, 1, 0};
        auto& t = triangles[tri_idx];
        f32 sqR1 = std::sqrt(r1);
        f32 b0 = 1.0f - sqR1;
        f32 b1 = sqR1 * (1.0f - r2);
        f32 b2 = sqR1 * r2;
        math::v3 n{
            b0*t.n0.x + b1*t.n1.x + b2*t.n2.x,
            b0*t.n0.y + b1*t.n1.y + b2*t.n2.y,
            b0*t.n0.z + b1*t.n1.z + b2*t.n2.z
        };
        f32 len = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
        if (len > 1e-6f) { n.x/=len; n.y/=len; n.z/=len; }
        return n;
    }
};

} // namespace primal::graphics::pcg
