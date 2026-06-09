#pragma once

#include "CommonHeaders.h"
#include "Utilities/MathTypes.h"

namespace primal::geometry {

// --- Inline math helpers for geometry algorithms ---
// These are needed because the RHI math namespace is not always available
// and we need simple standalone functions for algorithm function pointers.

inline f32 dot(const math::v3& a, const math::v3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline f32 length(const math::v3& v) {
    return std::sqrt(dot(v, v));
}

struct aabb {
    math::v3 min{};
    math::v3 max{};
};

// --- Geometry type enumerations ---

enum class GeometryType : u8 {
    Line,
    Arc,
    Spline,
    Polyline,
    Count
};

enum class SegmentType : u8 {
    Line,
    Arc,
};

// Generational ID for geometry objects
DEFINE_TYPED_ID(geometry_id);

struct GeometryHandle {
    geometry_id id{ id::invalid_id };
    GeometryType type{ GeometryType::Count };

    bool is_valid() const { return id != id::invalid_id; }
    bool operator==(const GeometryHandle& other) const { return id == other.id; }
    bool operator!=(const GeometryHandle& other) const { return id != other.id; }
};

} // namespace primal::geometry
