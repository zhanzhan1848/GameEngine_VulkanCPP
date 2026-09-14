#include "RigidBody.h"
#include "Entity.h"

namespace primal::rigidbody {

namespace {
    utl::vector<u8>      exists_flags;
    utl::vector<u32>     modes;
    utl::vector<f32>     masses;
    utl::vector<f32>     restitutions;
    utl::vector<f32>     frictions;
    utl::vector<math::v3> velocities;
} // anonymous namespace

component create(init_info info, game_entity::entity entity)
{
    assert(entity.is_valid());
    const id::id_type eindex{ id::index(entity.get_id()) };

    if (exists_flags.size() <= eindex) {
        exists_flags.resize(eindex + 1, 0);
        modes.resize(eindex + 1, 1);
        masses.resize(eindex + 1, 1.0f);
        restitutions.resize(eindex + 1, 0.1f);
        frictions.resize(eindex + 1, 0.5f);
        velocities.resize(eindex + 1, math::v3{0.f, 0.f, 0.f});
    }

    modes[eindex] = info.mode;
    masses[eindex] = info.mass;
    restitutions[eindex] = info.restitution;
    frictions[eindex] = info.friction;
    velocities[eindex] = info.linear_velocity;
    exists_flags[eindex] = 1;

    return component{ rigidbody_component_id{ entity.get_id() } };
}

void remove(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    if (eindex < exists_flags.size()) {
        exists_flags[eindex] = 0;
    }
}

u32 mode(component c)
{
    assert(c.is_valid());
    return modes[id::index(c.get_id())];
}

f32 mass(component c)
{
    assert(c.is_valid());
    return masses[id::index(c.get_id())];
}

f32 restitution(component c)
{
    assert(c.is_valid());
    return restitutions[id::index(c.get_id())];
}

f32 friction(component c)
{
    assert(c.is_valid());
    return frictions[id::index(c.get_id())];
}

math::v3 linear_velocity(component c)
{
    assert(c.is_valid());
    return velocities[id::index(c.get_id())];
}

void set_mode(component c, u32 mode_v)
{
    assert(c.is_valid());
    modes[id::index(c.get_id())] = mode_v;
}

void set_mass(component c, f32 mass_v)
{
    assert(c.is_valid());
    masses[id::index(c.get_id())] = mass_v;
}

void set_restitution(component c, f32 restitution_v)
{
    assert(c.is_valid());
    restitutions[id::index(c.get_id())] = restitution_v;
}

void set_friction(component c, f32 friction_v)
{
    assert(c.is_valid());
    frictions[id::index(c.get_id())] = friction_v;
}

void set_linear_velocity(component c, math::v3 velocity)
{
    assert(c.is_valid());
    velocities[id::index(c.get_id())] = velocity;
}

// ============================================================================
// component 句柄成员转发（EngineAPI/RigidBodyComponent.h）
// ============================================================================

u32 component::mode() const { return rigidbody::mode(*this); }
f32 component::mass() const { return rigidbody::mass(*this); }
f32 component::restitution() const { return rigidbody::restitution(*this); }
f32 component::friction() const { return rigidbody::friction(*this); }
math::v3 component::linear_velocity() const { return rigidbody::linear_velocity(*this); }

void component::set_mode(u32 mode_v) { rigidbody::set_mode(*this, mode_v); }
void component::set_mass(f32 mass_v) { rigidbody::set_mass(*this, mass_v); }
void component::set_restitution(f32 restitution_v) { rigidbody::set_restitution(*this, restitution_v); }
void component::set_friction(f32 friction_v) { rigidbody::set_friction(*this, friction_v); }
void component::set_linear_velocity(math::v3 velocity) { rigidbody::set_linear_velocity(*this, velocity); }

} // namespace primal::rigidbody
