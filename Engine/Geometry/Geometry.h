#pragma once

#include "Geometry/GeometryTypes.h"
#include <vector>

namespace primal::geometry {

// Lifecycle
GeometryHandle create(GeometryType type, const std::vector<math::v3>& points);
GeometryHandle create_arc_three_point(const std::vector<math::v3>& three_points);
GeometryHandle create_arc_parametric(const std::vector<math::v3>& endpoints, f32 radius, f32 start_angle, f32 end_angle);
GeometryHandle create_spline(const std::vector<math::v3>& points, bool closed = false);
GeometryHandle create_polyline(const std::vector<math::v3>& points, const std::vector<SegmentType>& segment_types);
void           destroy(GeometryHandle h);

// Query
GeometryType            get_type(GeometryHandle h);
const std::vector<math::v3>& get_control_points(GeometryHandle h);
u32                     get_control_point_count(GeometryHandle h);
void                    set_control_point(GeometryHandle h, u32 index, const math::v3& pt);
void                    set_control_points(GeometryHandle h, const std::vector<math::v3>& pts);
void                    append_point(GeometryHandle h, const math::v3& pt);
void                    insert_point(GeometryHandle h, u32 index, const math::v3& pt);
void                    remove_point(GeometryHandle h, u32 index);

// Computed
aabb             compute_bounding_box(GeometryHandle h);
f32                    compute_total_length(GeometryHandle h);
math::v3               compute_point_at(GeometryHandle h, f32 t);
math::v3               compute_tangent_at(GeometryHandle h, f32 t);
f32                    distance_to(GeometryHandle h, const math::v3& world_pos);

// Tessellation (lazy cached)
const std::vector<math::v3>& tessellate(GeometryHandle h, f32 tolerance = 0.01f);

// Algorithm registration (call once at startup)
void init();

} // namespace primal::geometry
