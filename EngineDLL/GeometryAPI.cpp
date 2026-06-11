#include "Common.h"
#include "CommonHeaders.h"
#include "Id.h"
#include "Geometry/Geometry.h"
#include "Geometry/GeometryFieldRasterizer.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Geometry.h"
#include "EngineAPI/GameEntity.h"

using namespace primal;
using namespace primal::geometry;

namespace {
    bool g_geometry_initialized = false;

    void ensure_init() {
        if (!g_geometry_initialized) {
            geometry::init();
            g_geometry_initialized = true;
        }
    }
}

// ── Lifecycle ──

EDITOR_INTERFACE void GeometryInit() {
    ensure_init();
}

EDITOR_INTERFACE GeometryHandle GeometryCreateLine(const float* points, u32 point_count) {
    if (!points || point_count < 2) return {};
    ensure_init();
    std::vector<math::v3> pts(point_count);
    for (u32 i = 0; i < point_count; ++i)
        pts[i] = { points[i * 3], points[i * 3 + 1], points[i * 3 + 2] };
    return geometry::create(GeometryType::Line, pts);
}

EDITOR_INTERFACE GeometryHandle GeometryCreateArcThreePoint(const float* three_points) {
    if (!three_points) return {};
    ensure_init();
    std::vector<math::v3> pts(3);
    for (u32 i = 0; i < 3; ++i)
        pts[i] = { three_points[i * 3], three_points[i * 3 + 1], three_points[i * 3 + 2] };
    return geometry::create_arc_three_point(pts);
}

EDITOR_INTERFACE GeometryHandle GeometryCreateArcParametric(const float* endpoints,
                                                           float radius,
                                                           float start_angle,
                                                           float end_angle) {
    if (!endpoints) return {};
    ensure_init();
    std::vector<math::v3> pts(2);
    for (u32 i = 0; i < 2; ++i)
        pts[i] = { endpoints[i * 3], endpoints[i * 3 + 1], endpoints[i * 3 + 2] };
    return geometry::create_arc_parametric(pts, radius, start_angle, end_angle);
}

EDITOR_INTERFACE GeometryHandle GeometryCreateSpline(const float* points, u32 point_count,
                                                     int closed) {
    if (!points || point_count < 2) return {};
    ensure_init();
    std::vector<math::v3> pts(point_count);
    for (u32 i = 0; i < point_count; ++i)
        pts[i] = { points[i * 3], points[i * 3 + 1], points[i * 3 + 2] };
    return geometry::create_spline(pts, closed != 0);
}

EDITOR_INTERFACE GeometryHandle GeometryCreatePolyline(const float* points, u32 point_count,
                                                       const int* segment_types) {
    if (!points || point_count < 2) return {};
    ensure_init();
    std::vector<math::v3> pts(point_count);
    for (u32 i = 0; i < point_count; ++i)
        pts[i] = { points[i * 3], points[i * 3 + 1], points[i * 3 + 2] };
    std::vector<SegmentType> segs;
    if (segment_types) {
        segs.resize(point_count - 1);
        for (u32 i = 0; i < segs.size(); ++i)
            segs[i] = static_cast<SegmentType>(segment_types[i]);
    }
    return geometry::create_polyline(pts, segs);
}

EDITOR_INTERFACE void GeometryDestroy(GeometryHandle h) {
    geometry::destroy(h);
}

// ── Query ──

EDITOR_INTERFACE int GeometryGetType(GeometryHandle h) {
    return static_cast<int>(geometry::get_type(h));
}

EDITOR_INTERFACE u32 GeometryGetPointCount(GeometryHandle h) {
    return geometry::get_control_point_count(h);
}

EDITOR_INTERFACE void GeometryGetPoints(GeometryHandle h, float* out_points) {
    if (!out_points) return;
    const auto& pts = geometry::get_control_points(h);
    for (u32 i = 0; i < pts.size(); ++i) {
        out_points[i * 3]     = pts[i].x;
        out_points[i * 3 + 1] = pts[i].y;
        out_points[i * 3 + 2] = pts[i].z;
    }
}

EDITOR_INTERFACE void GeometryGetBoundingBox(GeometryHandle h, float* out_min, float* out_max) {
    auto bbox = geometry::compute_bounding_box(h);
    if (out_min) { out_min[0] = bbox.min.x; out_min[1] = bbox.min.y; out_min[2] = bbox.min.z; }
    if (out_max) { out_max[0] = bbox.max.x; out_max[1] = bbox.max.y; out_max[2] = bbox.max.z; }
}

EDITOR_INTERFACE float GeometryGetTotalLength(GeometryHandle h) {
    return geometry::compute_total_length(h);
}

// ── Modification ──

EDITOR_INTERFACE void GeometrySetPoint(GeometryHandle h, u32 index, const float* point) {
    if (!point) return;
    geometry::set_control_point(h, index, { point[0], point[1], point[2] });
}

EDITOR_INTERFACE void GeometrySetPoints(GeometryHandle h, const float* points, u32 count) {
    if (!points) return;
    std::vector<math::v3> pts(count);
    for (u32 i = 0; i < count; ++i)
        pts[i] = { points[i * 3], points[i * 3 + 1], points[i * 3 + 2] };
    geometry::set_control_points(h, pts);
}

EDITOR_INTERFACE void GeometryAppendPoint(GeometryHandle h, const float* point) {
    if (!point) return;
    geometry::append_point(h, { point[0], point[1], point[2] });
}

EDITOR_INTERFACE void GeometryInsertPoint(GeometryHandle h, u32 index, const float* point) {
    if (!point) return;
    geometry::insert_point(h, index, { point[0], point[1], point[2] });
}

EDITOR_INTERFACE void GeometryRemovePoint(GeometryHandle h, u32 index) {
    geometry::remove_point(h, index);
}

// ── Tessellation ──

EDITOR_INTERFACE u32 GeometryTessellate(GeometryHandle h, float tolerance, float* out_points) {
    const auto& segs = geometry::tessellate(h, tolerance);
    if (out_points) {
        for (u32 i = 0; i < segs.size(); ++i) {
            out_points[i * 3]     = segs[i].x;
            out_points[i * 3 + 1] = segs[i].y;
            out_points[i * 3 + 2] = segs[i].z;
        }
    }
    return static_cast<u32>(segs.size());
}

// ── Distance ──

EDITOR_INTERFACE float GeometryDistanceTo(GeometryHandle h, const float* world_pos) {
    if (!world_pos) return 3.4e38f; // FLT_MAX equivalent
    return geometry::distance_to(h, { world_pos[0], world_pos[1], world_pos[2] });
}

EDITOR_INTERFACE void GeometryPointAt(GeometryHandle h, float t, float* out_point) {
    if (!out_point) return;
    auto p = geometry::compute_point_at(h, t);
    out_point[0] = p.x; out_point[1] = p.y; out_point[2] = p.z;
}

EDITOR_INTERFACE void GeometryTangentAt(GeometryHandle h, float t, float* out_tangent) {
    if (!out_tangent) return;
    auto tv = geometry::compute_tangent_at(h, t);
    out_tangent[0] = tv.x; out_tangent[1] = tv.y; out_tangent[2] = tv.z;
}

// ── Field Rasterize ──

EDITOR_INTERFACE void GeometryRasterizeField(const GeometryHandle* handles, u32 handle_count,
                                              const float* bounds_min, const float* bounds_max,
                                              u32 res_x, u32 res_y, u32 res_z,
                                              float band_width,
                                              float* out_field_data) {
    if (!handles || !bounds_min || !bounds_max || !out_field_data) return;
    std::vector<GeometryHandle> hvec(handles, handles + handle_count);
    geometry::FieldRasterizeParams params;
    params.bounds_min = { bounds_min[0], bounds_min[1], bounds_min[2] };
    params.bounds_max = { bounds_max[0], bounds_max[1], bounds_max[2] };
    params.resolution_x = res_x;
    params.resolution_y = res_y;
    params.resolution_z = res_z;
    params.band_width = band_width;
    auto output = geometry::rasterize(hvec, params);
    memcpy(out_field_data, output.data.data(), output.data.size() * sizeof(float));
}

// ── Entity Mount ──

EDITOR_INTERFACE id::id_type GeometryCreateEntity(GeometryHandle h, const float* position,
                                                   const float* rotation, const float* scale) {
    if (!h.is_valid()) return id::invalid_id;
    ensure_init();
    using namespace primal::game_entity;
    entity_info info{};
    transform::init_info tf{};
    if (position) { tf.position[0] = position[0]; tf.position[1] = position[1]; tf.position[2] = position[2]; }
    if (rotation) { tf.rotation[0] = rotation[0]; tf.rotation[1] = rotation[1]; tf.rotation[2] = rotation[2]; tf.rotation[3] = rotation[3]; }
    if (scale)    { tf.scale[0] = scale[0]; tf.scale[1] = scale[1]; tf.scale[2] = scale[2]; }
    info.transform = &tf;

    geometry::component::init_info gi{};
    gi.handle = h;
    info.geometry = &gi;

    entity e = create(info);
    return e.get_id();
}

EDITOR_INTERFACE GeometryHandle GeometryGetEntityHandle(id::id_type eid) {
    auto geom = geometry::component::get(game_entity::entity_id{eid});
    return geom.handle();
}

EDITOR_INTERFACE void GeometryDestroyEntity(id::id_type eid) {
    game_entity::remove(game_entity::entity_id{eid});
}
