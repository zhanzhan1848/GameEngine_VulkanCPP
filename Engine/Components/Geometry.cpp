#include "Geometry.h"
#include "Geometry/Geometry.h"
#include <cfloat>

namespace primal::geometry::component {

namespace {
    utl::vector<id::generation_type>  generations;
    utl::vector<u8>                   exists_flags;
    utl::vector<GeometryHandle>       handles;
}

geometry create(init_info info, game_entity::entity entity) {
    assert(info.handle.is_valid());
    const game_entity::entity_id id = entity.get_id();
    const id::id_type index = id::index(id);

    if (index >= exists_flags.size()) {
        generations.resize(index + 1, 0);
        exists_flags.resize(index + 1, 0);
        handles.resize(index + 1);
    }
    generations[index] = id::generation(id);
    handles[index] = info.handle;
    exists_flags[index] = 1;

    return geometry{ id };
}

void remove(game_entity::entity_id id) {
    const id::id_type index = id::index(id);
    if (index < exists_flags.size() && exists_flags[index]) {
        if (handles[index].is_valid()) {
            primal::geometry::destroy(handles[index]);
        }
        handles[index] = {};
        exists_flags[index] = 0;
    }
}

geometry get(game_entity::entity_id id) {
    return geometry{ id };
}

GeometryHandle geometry::handle() const {
    const id::id_type index = id::index(entity_id_);
    if (index < exists_flags.size() && exists_flags[index]) {
        return handles[index];
    }
    return {};
}

// Proxy implementations that delegate to geometry namespace
const std::vector<math::v3>& geometry::tessellate(f32 tolerance) const {
    static const std::vector<math::v3> empty;
    GeometryHandle h = handle();
    return h.is_valid() ? primal::geometry::tessellate(h, tolerance) : empty;
}

aabb geometry::bounding_box() const {
    GeometryHandle h = handle();
    return h.is_valid() ? primal::geometry::compute_bounding_box(h) : aabb{};
}

f32 geometry::total_length() const {
    GeometryHandle h = handle();
    return h.is_valid() ? primal::geometry::compute_total_length(h) : 0.0f;
}

f32 geometry::distance_to(const math::v3& pos) const {
    GeometryHandle h = handle();
    return h.is_valid() ? primal::geometry::distance_to(h, pos) : FLT_MAX;
}

void geometry::set_control_point(u32 idx, const math::v3& pt) {
    GeometryHandle h = handle();
    if (h.is_valid()) primal::geometry::set_control_point(h, idx, pt);
}

u32 geometry::control_point_count() const {
    GeometryHandle h = handle();
    return h.is_valid() ? primal::geometry::get_control_point_count(h) : 0;
}

} // namespace primal::geometry::component
