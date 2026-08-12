#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/PCGTypes.h"
#include "Geometry/GeometryFieldRasterizer.h"
#include <cstring>

namespace primal::graphics::pcg {

// Rasterizes geometry handles into a signed distance field consumed by downstream nodes.
// Bridges the Geometry subsystem into the PCG field-driven pipeline.
//
// Pin layout:
//   Outputs: [0] Field — PCGRasterizedField with trilinear sampling
//   Inputs:  (none)
//
// Parameters:
//   bounds_min/max — world-space AABB of the rasterization volume
//   resolution_x/y/z — voxel grid resolution per axis
//   band_width   — distance band around geometry surfaces
//   union_mode   — true: minimum unsigned distance; false: signed distance with band offset
//
// Usage:
//   RasterizedFieldNode node;
//   node.geometry_handles = {handle1, handle2};
//   node.bounds_min = {-10, -1, -10};
//   node.bounds_max = {10, 5, 10};
//   node.resolution_x = 32; node.resolution_y = 16; node.resolution_z = 32;
//   node.Execute();
//   // outputs[0].AsField() now contains the rasterized field
class RasterizedFieldNode : public PCGNode {
public:
    std::vector<primal::geometry::GeometryHandle> geometry_handles;
    primal::math::v3 bounds_min{};
    primal::math::v3 bounds_max{10.0f, 10.0f, 10.0f};
    u32 resolution_x{32};
    u32 resolution_y{32};
    u32 resolution_z{32};
    f32 band_width{5.0f};
    bool union_mode{false};

    RasterizedFieldNode() {
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::Field;
    }

    const char* TypeName() const override { return "RasterizedField"; }

    void Execute() override {
        if (geometry_handles.empty()) return;

        primal::geometry::FieldRasterizeParams params;
        params.bounds_min = bounds_min;
        params.bounds_max = bounds_max;
        params.resolution_x = resolution_x;
        params.resolution_y = resolution_y;
        params.resolution_z = resolution_z;
        params.band_width = band_width;
        params.union_mode = union_mode;

        auto output = primal::geometry::rasterize(geometry_handles, params);

        auto* field = CreateOutput<PCGRasterizedField>(0);
        field->data = std::move(output.data);
        field->dim[0] = output.dim[0];
        field->dim[1] = output.dim[1];
        field->dim[2] = output.dim[2];
        field->origin = output.origin;
        field->voxel_size = output.voxel_size;
    }

    // --- Reflection ---
    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount;
        return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount;
        return kPins;
    }
    bool SetParamByName(const char* name, f32 value) override {
        if (std::strcmp(name, "resolution_x") == 0) { resolution_x = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "resolution_y") == 0) { resolution_y = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "resolution_z") == 0) { resolution_z = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "band_width") == 0)   { band_width = value; return true; }
        if (std::strcmp(name, "union_mode") == 0)    { union_mode = value > 0.5f; return true; }
        return false;
    }
    bool SetParamByName(const char* name, primal::math::v3 value) override {
        if (std::strcmp(name, "bounds_min") == 0) { bounds_min = value; return true; }
        if (std::strcmp(name, "bounds_max") == 0) { bounds_max = value; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 7;
    static constexpr u32 kPinCount = 1;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor RasterizedFieldNode::kParams[] = {
    {"bounds_min",  "Volume", PCGParamType::Vec3,  {-100,100,0.1f},       PCG_OFFSETOF(RasterizedFieldNode, bounds_min),  sizeof(bounds_min),  nullptr},
    {"bounds_max",  "Volume", PCGParamType::Vec3,  {-100,100,0.1f},       PCG_OFFSETOF(RasterizedFieldNode, bounds_max),  sizeof(bounds_max),  nullptr},
    {"resolution_x","Volume", PCGParamType::UInt,  {4,128,4},             PCG_OFFSETOF(RasterizedFieldNode, resolution_x),sizeof(resolution_x),nullptr},
    {"resolution_y","Volume", PCGParamType::UInt,  {4,128,4},             PCG_OFFSETOF(RasterizedFieldNode, resolution_y),sizeof(resolution_y),nullptr},
    {"resolution_z","Volume", PCGParamType::UInt,  {4,128,4},             PCG_OFFSETOF(RasterizedFieldNode, resolution_z),sizeof(resolution_z),nullptr},
    {"band_width",  "Volume", PCGParamType::Float, {0.1f,50.0f,0.1f},    PCG_OFFSETOF(RasterizedFieldNode, band_width),  sizeof(band_width),  nullptr},
    {"union_mode",  "Volume", PCGParamType::Bool,  {0,1,1},              PCG_OFFSETOF(RasterizedFieldNode, union_mode),  sizeof(union_mode),  nullptr},
};
inline const PCGPinDescriptor RasterizedFieldNode::kPins[] = {
    {"rasterized_field", 0, PCGDataType::Field, false},
};

} // namespace primal::graphics::pcg
