#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/Nodes/MeshSurfaceSampler.h"
#include "Graphics/PCG/Nodes/ScatterContext.h"
#include "Content/ContentToEngine.h"
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>

namespace primal::graphics::pcg {

class SurfaceScatterNode : public PCGNode {
public:
    id::id_type geometry_content_id{id::invalid_id};
    u32 target_count{100};
    f32 normal_offset{0.0f};
    u32 seed{0};

    SurfaceScatterNode() {
        inputs.resize(0);
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "SurfaceScatter"; }

    void Execute() override {
        auto* out = CreateOutput<PCGPointSet>(0);
        if (geometry_content_id == id::invalid_id || target_count == 0) {
            out->Init(0, 0);
            return;
        }

        // Fetch RHIMeshAsset from content system
        id::id_type rhi_id = content::get_rhi_mesh_id(geometry_content_id);
        rhi::RHIMeshAsset asset;
        if (!content::get_rhi_mesh_asset(rhi_id, asset)) {
            out->Init(0, 0);
            return;
        }

        // Build surface data
        MeshSurfaceData surface = MeshSurfaceData::FromRHIMeshAsset(asset);
        if (surface.triangles.empty()) {
            std::cerr << "[SurfaceScatter] No triangles in mesh" << std::endl;
            out->Init(0, 0);
            return;
        }

        out->Init(target_count);
        ScatterContext::WriteDefaultAttributes(*out, seed);

        std::srand(seed);
        for (u32 i = 0; i < target_count; ++i) {
            f32 r1 = std::rand() / f32(RAND_MAX);
            f32 r2 = std::rand() / f32(RAND_MAX);
            f32 rTri = std::rand() / f32(RAND_MAX);

            u32 triIdx = surface.SelectTriangle(rTri);
            math::v3 pos = surface.SamplePoint(triIdx, r1, r2);
            math::v3 norm = surface.SampleNormal(triIdx, r1, r2);

            // Apply normal offset
            if (normal_offset != 0.0f) {
                pos.x += norm.x * normal_offset;
                pos.y += norm.y * normal_offset;
                pos.z += norm.z * normal_offset;
            }

            out->positions[i] = pos;

            // Encode normal direction as RotationY (atan2 of projected tangent)
            // For surface-aligned scatter, use the normal's horizontal direction
            f32 angle = std::atan2(-norm.z, norm.x);
            out->SetAttr(i, PCGAttr::RotationY, angle);

            // Density from triangle area (normalized)
            f32 totalArea = surface.cum_area.back();
            f32 relArea = totalArea > 0 ? surface.triangles[triIdx].area / totalArea : 1.0f;
            out->SetAttr(i, PCGAttr::Density, relArea);
        }
    }

    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount; return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount; return kPins;
    }
    bool SetParamByName(const char* n, f32 v) override {
        if (std::strcmp(n, "target_count") == 0) { target_count = static_cast<u32>(v); return true; }
        if (std::strcmp(n, "normal_offset") == 0) { normal_offset = v; return true; }
        if (std::strcmp(n, "seed") == 0) { seed = static_cast<u32>(v); return true; }
        if (std::strcmp(n, "geometry_content_id") == 0) { geometry_content_id = static_cast<id::id_type>((u64)v); return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 4;
    static constexpr u32 kPinCount = 1;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor SurfaceScatterNode::kParams[] = {
    {"geometry_content_id", "SurfaceScatter", PCGParamType::UInt,  {0,999999,1},    PCG_OFFSETOF(SurfaceScatterNode, geometry_content_id), sizeof(geometry_content_id), nullptr},
    {"target_count",        "SurfaceScatter", PCGParamType::UInt,  {1,100000,1},    PCG_OFFSETOF(SurfaceScatterNode, target_count),        sizeof(target_count),        nullptr},
    {"normal_offset",       "SurfaceScatter", PCGParamType::Float, {-10.0f,10.0f,0.01f}, PCG_OFFSETOF(SurfaceScatterNode, normal_offset),  sizeof(normal_offset),       nullptr},
    {"seed",                "SurfaceScatter", PCGParamType::UInt,  {0,9999,1},      PCG_OFFSETOF(SurfaceScatterNode, seed),                 sizeof(seed),                nullptr},
};
inline const PCGPinDescriptor SurfaceScatterNode::kPins[] = {
    {"points", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
