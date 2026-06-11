#include "Geometry/Geometry.h"
#include "Geometry/GeometryAlgorithmRegistry.h"
#include "Geometry/Algorithms/LineAlgorithm.h"
#include "Geometry/Algorithms/ArcAlgorithm.h"
#include "Geometry/Algorithms/SplineAlgorithm.h"
#include "Geometry/Algorithms/PolylineAlgorithm.h"
#include "CommonHeaders.h"
#include "Utilities/Vector.h"
#include "Id.h"

namespace primal::geometry {

// ---------------------------------------------------------------------------
// Algorithm registry implementation (inline with static storage)
// ---------------------------------------------------------------------------
namespace {

CurveAlgorithmEntry algorithm_table[static_cast<u32>(GeometryType::Count)];

} // anonymous namespace

void register_algorithm(GeometryType type, const CurveAlgorithmEntry& entry) {
    const u32 idx = static_cast<u32>(type);
    assert(idx < static_cast<u32>(GeometryType::Count));
    algorithm_table[idx] = entry;
}

const CurveAlgorithmEntry* get_algorithm(GeometryType type) {
    const u32 idx = static_cast<u32>(type);
    if (idx >= static_cast<u32>(GeometryType::Count)) return nullptr;
    const CurveAlgorithmEntry* e = &algorithm_table[idx];
    return e->tessellate ? e : nullptr;
}

// ---------------------------------------------------------------------------
// SoA storage for geometry instances
// ---------------------------------------------------------------------------
namespace {

// Stable parameter storage -- pointers into these vectors are stored in params_ptrs
utl::vector<algorithms::arc::ArcParams>       arc_params_store;
utl::vector<algorithms::spline::SplineParams> spline_params_store;
utl::vector<algorithms::polyline::PolylineParams> polyline_params_store;
utl::vector<std::vector<SegmentType>>              polyline_seg_type_store;

utl::vector<id::generation_type>             generations;
utl::deque<geometry_id>                      free_ids;
utl::vector<GeometryType>                    types;
utl::vector<std::vector<math::v3>>           control_points;  // std::vector for algorithm compat
utl::vector<f32>                             arc_radii;
utl::vector<f32>                             arc_start_angles;
utl::vector<f32>                             arc_end_angles;
utl::vector<bool>                            arc_three_point;
utl::vector<bool>                            spline_closed;
utl::vector<std::vector<SegmentType>>        polyline_segment_types;
utl::vector<const void*>                     params_ptrs;
utl::vector<std::vector<math::v3>>           tessellated_cache;
utl::vector<bool>                            tessellated_dirty;

// Static empty vector for invalid-handle returns
static const std::vector<math::v3> empty_points;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

bool is_valid(GeometryHandle h) {
    if (!h.is_valid()) return false;
    const id::id_type index = id::index(h.id);
    if (index >= generations.size()) return false;
    return generations[index] == id::generation(h.id);
}

void invalidate_tessellation(u32 index) {
    tessellated_dirty[index] = true;
}

// Allocate a new geometry slot, returns the index
u32 allocate_slot(geometry_id& out_id) {
    if (free_ids.size() > id::min_deleted_elements) {
        out_id = free_ids.front();
        free_ids.pop_front();
        out_id = geometry_id{ id::new_generation(out_id) };
        const u32 index = static_cast<u32>(id::index(out_id));
        ++generations[index];
        // Reset per-slot data
        types[index] = GeometryType::Count;
        control_points[index].clear();
        params_ptrs[index] = nullptr;
        tessellated_cache[index].clear();
        tessellated_dirty[index] = true;
        return index;
    }

    out_id = geometry_id{ static_cast<id::id_type>(generations.size()) };
    const u32 index = static_cast<u32>(generations.size());

    generations.push_back(0);
    types.emplace_back(GeometryType::Count);
    control_points.emplace_back();
    arc_radii.emplace_back(0.0f);
    arc_start_angles.emplace_back(0.0f);
    arc_end_angles.emplace_back(0.0f);
    arc_three_point.emplace_back(false);
    spline_closed.emplace_back(false);
    polyline_segment_types.emplace_back();
    params_ptrs.emplace_back(nullptr);
    tessellated_cache.emplace_back();
    tessellated_dirty.emplace_back(true);

    return index;
}

void setup_params(u32 index, GeometryType type) {
    switch (type) {
    case GeometryType::Arc: {
        algorithms::arc::ArcParams ap;
        ap.radius        = arc_radii[index];
        ap.start_angle   = arc_start_angles[index];
        ap.end_angle     = arc_end_angles[index];
        ap.three_point   = arc_three_point[index];
        arc_params_store.push_back(ap);
        params_ptrs[index] = &arc_params_store.back();
    } break;
    case GeometryType::Spline: {
        algorithms::spline::SplineParams sp;
        sp.closed = spline_closed[index];
        spline_params_store.push_back(sp);
        params_ptrs[index] = &spline_params_store.back();
    } break;
    case GeometryType::Polyline: {
        algorithms::polyline::PolylineParams pp;
        if (!polyline_segment_types[index].empty()) {
            polyline_seg_type_store.push_back(polyline_segment_types[index]);
            pp.segment_types  = polyline_seg_type_store.back().data();
            pp.segment_count  = static_cast<u32>(polyline_segment_types[index].size());
        }
        polyline_params_store.push_back(pp);
        params_ptrs[index] = &polyline_params_store.back();
    } break;
    default:
        // Line has no params
        params_ptrs[index] = nullptr;
        break;
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
GeometryHandle create(GeometryType type, const std::vector<math::v3>& points) {
    geometry_id gid;
    const u32 index = allocate_slot(gid);
    types[index]            = type;
    control_points[index]   = points;
    setup_params(index, type);

    GeometryHandle h;
    h.id   = gid;
    h.type = type;
    return h;
}

GeometryHandle create_arc_three_point(const std::vector<math::v3>& three_points) {
    assert(three_points.size() >= 3);
    geometry_id gid;
    const u32 index = allocate_slot(gid);
    types[index]             = GeometryType::Arc;
    control_points[index]    = three_points;
    arc_three_point[index]   = true;
    setup_params(index, GeometryType::Arc);

    GeometryHandle h;
    h.id   = gid;
    h.type = GeometryType::Arc;
    return h;
}

GeometryHandle create_arc_parametric(const std::vector<math::v3>& endpoints, f32 radius, f32 start_angle, f32 end_angle) {
    assert(endpoints.size() >= 2);
    geometry_id gid;
    const u32 index = allocate_slot(gid);
    types[index]             = GeometryType::Arc;
    control_points[index]    = endpoints;
    arc_radii[index]         = radius;
    arc_start_angles[index]  = start_angle;
    arc_end_angles[index]    = end_angle;
    arc_three_point[index]   = false;
    setup_params(index, GeometryType::Arc);

    GeometryHandle h;
    h.id   = gid;
    h.type = GeometryType::Arc;
    return h;
}

GeometryHandle create_spline(const std::vector<math::v3>& points, bool closed) {
    geometry_id gid;
    const u32 index = allocate_slot(gid);
    types[index]          = GeometryType::Spline;
    control_points[index] = points;
    spline_closed[index]  = closed;
    setup_params(index, GeometryType::Spline);

    GeometryHandle h;
    h.id   = gid;
    h.type = GeometryType::Spline;
    return h;
}

GeometryHandle create_polyline(const std::vector<math::v3>& points, const std::vector<SegmentType>& segment_types) {
    geometry_id gid;
    const u32 index = allocate_slot(gid);
    types[index]                   = GeometryType::Polyline;
    control_points[index]          = points;
    polyline_segment_types[index]  = segment_types;
    setup_params(index, GeometryType::Polyline);

    GeometryHandle h;
    h.id   = gid;
    h.type = GeometryType::Polyline;
    return h;
}

void destroy(GeometryHandle h) {
    if (!is_valid(h)) return;
    const u32 index = static_cast<u32>(id::index(h.id));
    types[index]                 = GeometryType::Count;
    control_points[index].clear();
    params_ptrs[index]           = nullptr;
    tessellated_cache[index].clear();
    tessellated_dirty[index]     = true;
    free_ids.push_back(h.id);
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------
GeometryType get_type(GeometryHandle h) {
    assert(is_valid(h));
    return types[id::index(h.id)];
}

const std::vector<math::v3>& get_control_points(GeometryHandle h) {
    if (!is_valid(h)) return empty_points;
    return control_points[id::index(h.id)];
}

u32 get_control_point_count(GeometryHandle h) {
    if (!is_valid(h)) return 0;
    return static_cast<u32>(control_points[id::index(h.id)].size());
}

void set_control_point(GeometryHandle h, u32 index, const math::v3& pt) {
    if (!is_valid(h)) return;
    const u32 i = static_cast<u32>(id::index(h.id));
    assert(index < control_points[i].size());
    control_points[i][index] = pt;
    invalidate_tessellation(i);
}

void set_control_points(GeometryHandle h, const std::vector<math::v3>& pts) {
    if (!is_valid(h)) return;
    const u32 i = static_cast<u32>(id::index(h.id));
    control_points[i] = pts;
    invalidate_tessellation(i);
}

void append_point(GeometryHandle h, const math::v3& pt) {
    if (!is_valid(h)) return;
    const u32 i = static_cast<u32>(id::index(h.id));
    control_points[i].push_back(pt);
    invalidate_tessellation(i);
}

void insert_point(GeometryHandle h, u32 index, const math::v3& pt) {
    if (!is_valid(h)) return;
    const u32 i = static_cast<u32>(id::index(h.id));
    assert(index <= control_points[i].size());
    control_points[i].insert(control_points[i].begin() + index, pt);
    invalidate_tessellation(i);
}

void remove_point(GeometryHandle h, u32 index) {
    if (!is_valid(h)) return;
    const u32 i = static_cast<u32>(id::index(h.id));
    assert(index < control_points[i].size());
    control_points[i].erase(control_points[i].begin() + index);
    invalidate_tessellation(i);
}

// ---------------------------------------------------------------------------
// Computed queries -- dispatch through algorithm table
// ---------------------------------------------------------------------------
aabb compute_bounding_box(GeometryHandle h) {
    if (!is_valid(h)) return {};
    const u32 i = static_cast<u32>(id::index(h.id));
    const CurveAlgorithmEntry* algo = get_algorithm(types[i]);
    if (!algo || !algo->bounding_box) return {};
    return algo->bounding_box(control_points[i], params_ptrs[i]);
}

f32 compute_total_length(GeometryHandle h) {
    if (!is_valid(h)) return 0.0f;
    const u32 i = static_cast<u32>(id::index(h.id));
    const CurveAlgorithmEntry* algo = get_algorithm(types[i]);
    if (!algo || !algo->total_length) return 0.0f;
    return algo->total_length(control_points[i], params_ptrs[i]);
}

math::v3 compute_point_at(GeometryHandle h, f32 t) {
    if (!is_valid(h)) return {};
    const u32 i = static_cast<u32>(id::index(h.id));
    const CurveAlgorithmEntry* algo = get_algorithm(types[i]);
    if (!algo || !algo->point_at) return {};
    return algo->point_at(control_points[i], params_ptrs[i], t);
}

math::v3 compute_tangent_at(GeometryHandle h, f32 t) {
    if (!is_valid(h)) return {};
    const u32 i = static_cast<u32>(id::index(h.id));
    const CurveAlgorithmEntry* algo = get_algorithm(types[i]);
    if (!algo || !algo->tangent_at) return {};
    return algo->tangent_at(control_points[i], params_ptrs[i], t);
}

f32 distance_to(GeometryHandle h, const math::v3& world_pos) {
    if (!is_valid(h)) return FLT_MAX;
    const u32 i = static_cast<u32>(id::index(h.id));
    const CurveAlgorithmEntry* algo = get_algorithm(types[i]);
    if (!algo || !algo->distance_to) return FLT_MAX;
    return algo->distance_to(control_points[i], params_ptrs[i], world_pos);
}

// ---------------------------------------------------------------------------
// Tessellation (lazy cached)
// ---------------------------------------------------------------------------
const std::vector<math::v3>& tessellate(GeometryHandle h, f32 tolerance) {
    if (!is_valid(h)) return empty_points;
    const u32 i = static_cast<u32>(id::index(h.id));
    if (!tessellated_dirty[i]) return tessellated_cache[i];

    const CurveAlgorithmEntry* algo = get_algorithm(types[i]);
    if (!algo || !algo->tessellate) return empty_points;

    algo->tessellate(control_points[i], params_ptrs[i], tolerance, tessellated_cache[i]);
    tessellated_dirty[i] = false;
    return tessellated_cache[i];
}

// ---------------------------------------------------------------------------
// Algorithm registration -- call once at startup
// ---------------------------------------------------------------------------
void init() {
    register_algorithm(GeometryType::Line,     algorithms::line::make_entry());
    register_algorithm(GeometryType::Arc,      algorithms::arc::make_entry());
    register_algorithm(GeometryType::Spline,   algorithms::spline::make_entry());
    register_algorithm(GeometryType::Polyline, algorithms::polyline::make_entry());
}

} // namespace primal::geometry
