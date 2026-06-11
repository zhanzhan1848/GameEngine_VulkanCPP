#pragma once

#include "ComponentsCommon.h"
#include "Geometry/GeometryTypes.h"
#include "EngineAPI/GameEntity.h"

namespace primal {

namespace geometry {
namespace component {

struct init_info {
    GeometryHandle handle;
};

struct geometry {
    constexpr static component_bit bit = component_bit::Geometry;

    geometry() = default;
    explicit geometry(game_entity::entity_id id) : entity_id_{id} {}

    bool is_valid() const { return entity_id_ != id::invalid_id; }

    GeometryHandle handle() const;
    const std::vector<math::v3>& tessellate(f32 tolerance = 0.01f) const;
    aabb                    bounding_box() const;
    f32                     total_length() const;
    f32                     distance_to(const math::v3& pos) const;
    void                    set_control_point(u32 idx, const math::v3& pt);
    u32                     control_point_count() const;

private:
    game_entity::entity_id entity_id_{ id::invalid_id };
};

geometry create(init_info info, game_entity::entity entity);
void     remove(game_entity::entity_id id);
geometry get(game_entity::entity_id id);

} // namespace component
} // namespace geometry

// Must be in primal namespace for component_init_info to find it
namespace geometry_comp = geometry::component;

} // namespace primal
