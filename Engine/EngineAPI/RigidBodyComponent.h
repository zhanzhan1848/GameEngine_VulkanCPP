#pragma once

#include "Components/ComponentsCommon.h"

namespace primal::rigidbody {

DEFINE_TYPED_ID(rigidbody_component_id);

class component final
{
public:
    constexpr explicit component(rigidbody_component_id id) : _id{ id } {}
    constexpr component() : _id{ id::invalid_id } {}
    [[nodiscard]] constexpr rigidbody_component_id get_id() const { return _id; }
    [[nodiscard]] constexpr bool is_valid() const { return id::is_valid(_id); }

    u32 mode() const;                        // 0=static 1=dynamic 2=kinematic
    f32 mass() const;
    f32 restitution() const;
    f32 friction() const;
    math::v3 linear_velocity() const;

    void set_mode(u32 mode);
    void set_mass(f32 mass);
    void set_restitution(f32 restitution);
    void set_friction(f32 friction);
    void set_linear_velocity(math::v3 velocity);

private:
    rigidbody_component_id _id;
};

} // namespace primal::rigidbody
