#include "Light.h"
#include "Entity.h"

namespace primal::light {

namespace {
    utl::vector<u8>                         exists_flags;
    utl::vector<u64>                        light_set_keys;
    utl::vector<graphics::light::type>      types;
    utl::vector<f32>                        intensities;
    utl::vector<math::v3>                   colors;
    utl::vector<math::v3>                   attenuations;
    utl::vector<f32>                        ranges;
    utl::vector<f32>                        umbras;
    utl::vector<f32>                        penumbras;
    utl::vector<u8>                         enabled_flags;
} // anonymous namespace

component create(init_info info, game_entity::entity entity)
{
    assert(entity.is_valid());
    const id::id_type eindex{ id::index(entity.get_id()) };

    if (exists_flags.size() <= eindex) {
        exists_flags.resize(eindex + 1, 0);
        light_set_keys.resize(eindex + 1, 0);
        types.resize(eindex + 1, graphics::light::directional);
        intensities.resize(eindex + 1, 1.0f);
        colors.resize(eindex + 1, math::v3{1.0f, 1.0f, 1.0f});
        attenuations.resize(eindex + 1, math::v3{1.0f, 0.0f, 0.0f});
        ranges.resize(eindex + 1, 10.0f);
        umbras.resize(eindex + 1, 0.0f);
        penumbras.resize(eindex + 1, 0.0f);
        enabled_flags.resize(eindex + 1, 1);
    }

    light_set_keys[eindex] = info.light_set_key;
    types[eindex] = info.type;
    intensities[eindex] = info.intensity;
    colors[eindex] = info.color;
    attenuations[eindex] = info.attenuation;
    ranges[eindex] = info.range;
    umbras[eindex] = info.umbra;
    penumbras[eindex] = info.penumbra;
    enabled_flags[eindex] = info.is_enabled ? 1 : 0;
    exists_flags[eindex] = 1;

    return component{ light_component_id{ entity.get_id() } };
}

void remove(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    if (eindex < exists_flags.size()) {
        exists_flags[eindex] = 0;
    }
}

bool is_enabled(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return enabled_flags[eindex] != 0;
}

f32 intensity(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return intensities[eindex];
}

math::v3 color(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return colors[eindex];
}

math::v3 attenuation(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return attenuations[eindex];
}

f32 range(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return ranges[eindex];
}

f32 umbra(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return umbras[eindex];
}

f32 penumbra(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return penumbras[eindex];
}

graphics::light::type light_type(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return types[eindex];
}

void set_enabled(component c, bool value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    enabled_flags[eindex] = value ? 1 : 0;
}

void set_intensity(component c, f32 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    intensities[eindex] = value;
}

void set_color(component c, math::v3 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    colors[eindex] = value;
}

void set_attenuation(component c, math::v3 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    attenuations[eindex] = value;
}

void set_range(component c, f32 value)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    ranges[eindex] = value;
}

void set_cone_angles(component c, f32 u, f32 p)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    umbras[eindex] = u;
    penumbras[eindex] = p;
}

void set_light_type(component c, graphics::light::type type)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    types[eindex] = type;
}

// ============================================================================
// component member methods
// ============================================================================

bool component::is_enabled() const { return light::is_enabled(*this); }
f32 component::intensity() const { return light::intensity(*this); }
math::v3 component::color() const { return light::color(*this); }
math::v3 component::attenuation() const { return light::attenuation(*this); }
f32 component::range() const { return light::range(*this); }
f32 component::umbra() const { return light::umbra(*this); }
f32 component::penumbra() const { return light::penumbra(*this); }
graphics::light::type component::light_type() const { return light::light_type(*this); }

void component::set_enabled(bool value) { light::set_enabled(*this, value); }
void component::set_intensity(f32 value) { light::set_intensity(*this, value); }
void component::set_color(math::v3 value) { light::set_color(*this, value); }
void component::set_attenuation(math::v3 value) { light::set_attenuation(*this, value); }
void component::set_range(f32 value) { light::set_range(*this, value); }
void component::set_cone_angles(f32 u, f32 p) { light::set_cone_angles(*this, u, p); }
void component::set_light_type(graphics::light::type type) { light::set_light_type(*this, type); }

} // namespace primal::light
