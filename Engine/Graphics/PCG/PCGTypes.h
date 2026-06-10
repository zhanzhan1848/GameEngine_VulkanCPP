#pragma once

#include "CommonHeaders.h"
#include "Graphics/Field/FieldDescriptor.h"
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/PCG/Noise/SimplexNoise.h"
#include "Graphics/PCG/Noise/WorleyNoise.h"
#include "Graphics/PCG/PCGSDFReadbackManager.h"
#include <vector>
#include <memory>
#include <cassert>

namespace primal::graphics::pcg {

// PCG data type discriminant — identifies what a PCGData-derived object contains.
enum class PCGDataType : u8 {
    Field,       // PCGField — scalar field sampled at world positions
    PointSet,    // PCGPointSet — collection of points with attributes
    Geometry,    // Reserved for future use
    AttributeSet // Reserved for future use
};

// Base class for all PCG data objects passed between nodes via pins.
struct PCGData {
    PCGDataType type;
    virtual ~PCGData() = default;
protected:
    PCGData() = default;
};

// Predefined per-point attribute slots. Used as indices into PCGPointSet::attrs.
// Density is set by FieldScatterNode; ScaleX/Y/Z by TransformNode; MeshIndex by MeshAssignNode.
enum class PCGAttr : u8 {
    Density,    // [0,1] probability-weighted density from scatter
    ScaleX,     // Random scale per axis
    ScaleY,
    ScaleZ,
    NormalX,    // Reserved for future surface normal
    NormalY,
    NormalZ,
    Seed,       // Per-point random seed
    RotationY,  // Y-axis rotation in radians
    MeshIndex,  // Assigned mesh slot index (0-based)
    Count       // Total number of attributes (used as default stride)
};

// Scalar field base class. Nodes produce or consume fields via SampleFloat.
struct PCGField : PCGData {
    field::FieldDescriptor desc;

    // Sample the field value at a world-space position.
    virtual f32 SampleFloat(math::v3 world_pos) const = 0;
    virtual ~PCGField() = default;

protected:
    PCGField() { type = PCGDataType::Field; }
};

// On-the-fly FBM noise field. Created by NoiseFieldNode.
// SampleFloat computes FBM2D at runtime — no precomputation or GPU dependency.
//
// Parameters:
//   frequency   — spatial scale of the noise (lower = smoother)
//   octaves     — number of FBM octaves (more = finer detail)
//   lacunarity  — frequency multiplier per octave (typically 2.0)
//   persistence — amplitude multiplier per octave (typically 0.5)
//   seed        — random seed for permutation table
//
// Phase 1 limitation: Only NoiseType::Simplex is implemented. Worley/Ridged are declared but not functional.
struct PCGNoiseField : PCGField {
    enum class NoiseType : u8 { Simplex, Worley, Ridged };
    NoiseType noise_type{NoiseType::Simplex};
    f32 frequency{0.02f};
    f32 lacunarity{2.0f};
    f32 persistence{0.5f};
    u32 octaves{6};
    u32 seed{0};

    f32 SampleFloat(math::v3 world_pos) const override {
        f32 sx = world_pos.x * frequency;
        f32 sz = world_pos.z * frequency;
        switch (noise_type) {
        case NoiseType::Worley:
            return WorleyF1_2D(sx, sz, seed);
        case NoiseType::Ridged:
            return RidgedFBM2D(sx, sz, octaves, lacunarity, persistence, seed);
        default:
            return FBM2D(sx, sz, octaves, lacunarity, persistence, seed);
        }
    }
};

// Reference field backed by FieldRegistry (e.g., GlobalSDF).
// Created by ReferenceFieldNode.
//
// When a PCGSDFReadbackManager is provided (Phase 2), samples from the real GlobalSDF
// via trilinear interpolation on CPU-side readback data. Otherwise falls back to
// analytic ground plane SDF (world_pos.y).
//
// SDF convention: positive = outside (above ground), negative = inside (below ground).
struct PCGReferenceField : PCGField {
    field::FieldSemantic semantic{field::FieldSemantic::GlobalSDF};
    const field::FieldDescriptor* ref_desc{nullptr};
    class PCGSDFReadbackManager* readback_mgr{nullptr};

    // Grid parameters for readback sampling (set by ReferenceFieldNode)
    math::v3 grid_origin{};
    f32 voxel_size{1.0f};

    f32 SampleFloat(math::v3 world_pos) const override {
        if (readback_mgr && readback_mgr->IsDataReady()) {
            return readback_mgr->SampleSDF(world_pos, grid_origin, voxel_size);
        }
        // Fallback: ground plane SDF at y=0
        return world_pos.y;
    }

    void BindRegistry() {
        ref_desc = field::FieldRegistry::Get().Find(semantic);
        if (ref_desc) desc = *ref_desc;
    }
};

// Rasterized geometry distance field. Created by RasterizedFieldNode.
// Wraps the CPU-side FieldOutput from geometry::rasterize() with trilinear sampling.
//
// Convention: values are distances from the nearest geometry surface.
// union_mode=true: minimum unsigned distance (SDF-like).
// union_mode=false: signed distance with band_width offset.
struct PCGRasterizedField : PCGField {
    std::vector<f32> data;
    u32 dim[3]{0, 0, 0};
    math::v3 origin{};
    math::v3 voxel_size{};

    f32 SampleFloat(math::v3 world_pos) const override {
        if (data.empty()) return 0.0f;

        f32 gx = (voxel_size.x > 0.0f) ? (world_pos.x - origin.x) / voxel_size.x : 0.0f;
        f32 gy = (voxel_size.y > 0.0f) ? (world_pos.y - origin.y) / voxel_size.y : 0.0f;
        f32 gz = (voxel_size.z > 0.0f) ? (world_pos.z - origin.z) / voxel_size.z : 0.0f;

        f32 max_x = static_cast<f32>(dim[0] > 1 ? dim[0] - 2 : 0);
        f32 max_y = static_cast<f32>(dim[1] > 1 ? dim[1] - 2 : 0);
        f32 max_z = static_cast<f32>(dim[2] > 1 ? dim[2] - 2 : 0);
        gx = std::clamp(gx - 0.5f, 0.0f, std::max(0.0f, max_x));
        gy = std::clamp(gy - 0.5f, 0.0f, std::max(0.0f, max_y));
        gz = std::clamp(gz - 0.5f, 0.0f, std::max(0.0f, max_z));

        u32 x0 = static_cast<u32>(gx), y0 = static_cast<u32>(gy), z0 = static_cast<u32>(gz);
        u32 x1 = std::min(x0 + 1, dim[0] - 1);
        u32 y1 = std::min(y0 + 1, dim[1] - 1);
        u32 z1 = std::min(z0 + 1, dim[2] - 1);
        f32 fx = gx - x0, fy = gy - y0, fz = gz - z0;

        auto at = [&](u32 x, u32 y, u32 z) -> f32 {
            return data[z * dim[0] * dim[1] + y * dim[0] + x];
        };

        f32 c000 = at(x0,y0,z0), c100 = at(x1,y0,z0);
        f32 c010 = at(x0,y1,z0), c110 = at(x1,y1,z0);
        f32 c001 = at(x0,y0,z1), c101 = at(x1,y0,z1);
        f32 c011 = at(x0,y1,z1), c111 = at(x1,y1,z1);

        f32 x00 = c000 + fx * (c100 - c000);
        f32 x10 = c010 + fx * (c110 - c010);
        f32 x01 = c001 + fx * (c101 - c001);
        f32 x11 = c011 + fx * (c111 - c011);
        f32 y0v = x00 + fy * (x10 - x00);
        f32 y1v = x01 + fy * (x11 - x01);
        return y0v + fz * (y1v - y0v);
    }
};

// Point set with SoA layout — the primary data flowing through scatter/constraint/filter nodes.
// Positions and attributes are stored in parallel arrays for cache-friendly iteration.
//
// Usage:
//   PCGPointSet set;
//   set.Init(100);                    // 100 points, stride = PCGAttr::Count
//   set.positions[0] = {1, 2, 3};
//   set.SetAttr(0, PCGAttr::Density, 0.8f);
struct PCGPointSet : PCGData {
    std::vector<math::v3> positions;  // World-space positions
    std::vector<f32> attrs;           // Flat attribute array: attrs[point * stride + attr_id]
    u32 attr_stride{0};               // Number of f32 per point in attrs
    u32 count{0};                     // Number of points

    PCGPointSet() { type = PCGDataType::PointSet; }

    // Allocate storage for num_points with the given attribute stride.
    // Default stride covers all PCGAttr slots.
    void Init(u32 num_points, u32 stride = static_cast<u32>(PCGAttr::Count)) {
        count = num_points;
        attr_stride = stride;
        positions.resize(num_points);
        attrs.resize(static_cast<size_t>(num_points) * stride, 0.0f);
    }

    f32 GetAttr(u32 idx, PCGAttr attr) const {
        return attrs[idx * attr_stride + static_cast<u32>(attr)];
    }
    void SetAttr(u32 idx, PCGAttr attr, f32 value) {
        attrs[idx * attr_stride + static_cast<u32>(attr)] = value;
    }
};

// Type-safe pin for node connections. Each pin has an expected data type.
// During graph execution, upstream output pins are wired to downstream input pins.
// Use AsField() / AsPointSet() to retrieve typed data (asserts on type mismatch).
struct PCGPin {
    PCGDataType expected_type{PCGDataType::Field};
    PCGData* data{nullptr};           // Set by PCGGraph::Execute during propagation
    u32 index{0};                     // Pin index within the node

    PCGField* AsField() const {
        if (data) {
            assert(data->type == PCGDataType::Field && "Pin type mismatch: expected Field");
            return static_cast<PCGField*>(data);
        }
        return nullptr;
    }
    PCGPointSet* AsPointSet() const {
        if (data) {
            assert(data->type == PCGDataType::PointSet && "Pin type mismatch: expected PointSet");
            return static_cast<PCGPointSet*>(data);
        }
        return nullptr;
    }
};

} // namespace primal::graphics::pcg
