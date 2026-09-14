#pragma once

#include "Components/ComponentsCommon.h"

namespace primal::collider {

DEFINE_TYPED_ID(collider_component_id);

class component final
{
public:
    constexpr explicit component(collider_component_id id) : _id{ id } {}
    constexpr component() : _id{ id::invalid_id } {}
    [[nodiscard]] constexpr collider_component_id get_id() const { return _id; }
    [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    u32 shape() const;                       // 0=box 1=sphere
    math::v3 half_extents() const;
    math::v3 offset() const;
    bool is_trigger() const;
    u32 layer() const;

    void set_shape(u32 shape);
    void set_half_extents(math::v3 half);
    void set_offset(math::v3 offset);
    void set_is_trigger(bool trigger);
    void set_layer(u32 layer);

private:
    collider_component_id _id;
};

} // namespace primal::collider
