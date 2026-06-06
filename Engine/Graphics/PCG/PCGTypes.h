#pragma once

#include "CommonHeaders.h"
#include "Graphics/Field/FieldDescriptor.h"
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/PCG/Noise/SimplexNoise.h"
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
        return FBM2D(world_pos.x * frequency, world_pos.z * frequency,
                     octaves, lacunarity, persistence, seed);
    }
};

// Reference field backed by FieldRegistry (e.g., GlobalSDF).
// Created by ReferenceFieldNode.
//
// Phase 1 limitation: Always returns ground plane SDF (world_pos.y) regardless of
// semantic or ref_desc. Real FieldRegistry sampling requires GPU readback (Phase 2+).
//
// SDF convention: positive = outside (above ground), negative = inside (below ground).
struct PCGReferenceField : PCGField {
    field::FieldSemantic semantic{field::FieldSemantic::GlobalSDF};
    const field::FieldDescriptor* ref_desc{nullptr};

    f32 SampleFloat(math::v3 world_pos) const override {
        // Phase 1: ground plane SDF at y=0
        // Standard SDF convention: positive = outside (above), negative = inside (below)
        return world_pos.y;
    }

    void BindRegistry() {
        ref_desc = field::FieldRegistry::Get().Find(semantic);
        if (ref_desc) desc = *ref_desc;
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
