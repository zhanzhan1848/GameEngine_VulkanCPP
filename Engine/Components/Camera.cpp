#include "Camera.h"
#include "Entity.h"

namespace primal::camera {

namespace {
    utl::vector<u8>                         exists_flags;
    utl::vector<graphics::camera::type>     types;
    utl::vector<math::v3>                   ups;
    utl::vector<f32>                        fovs;        // also view_width
    utl::vector<f32>                        aspects;     // also view_height
    utl::vector<f32>                        near_zs;
    utl::vector<f32>                        far_zs;
} // anonymous namespace

component create(init_info info, game_entity::entity entity)
{
    assert(entity.is_valid());
    const id::id_type eindex{ id::index(entity.get_id()) };

    if (exists_flags.size() <= eindex) {
        exists_flags.resize(eindex + 1, 0);
        types.resize(eindex + 1, graphics::camera::perspective);
        ups.resize(eindex + 1, math::v3{0.0f, 1.0f, 0.0f});
        fovs.resize(eindex + 1, 0.25f);
        aspects.resize(eindex + 1, 16.0f / 9.0f);
        near_zs.resize(eindex + 1, 0.1f);
        far_zs.resize(eindex + 1, 64.0f);
    }

    types[eindex] = info.type;
    ups[eindex] = info.up;
    fovs[eindex] = info.type == graphics::camera::perspective ? info.field_of_view : info.view_width;
    aspects[eindex] = info.type == graphics::camera::perspective ? info.aspect_ratio : info.view_height;
    near_zs[eindex] = info.near_z;
    far_zs[eindex] = info.far_z;
    exists_flags[eindex] = 1;

    return component{ camera_component_id{ entity.get_id() } };
}

void remove(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    if (eindex < exists_flags.size()) {
        exists_flags[eindex] = 0;
    }
}

math::v3 up_vector(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return ups[eindex];
}

f32 field_of_view(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return fovs[eindex];
}

f32 aspect_ratio(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return aspects[eindex];
}

f32 view_width(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return fovs[eindex];
}

f32 view_height(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return aspects[eindex];
}

f32 near_z(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return near_zs[eindex];
}

f32 far_z(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return far_zs[eindex];
}

graphics::camera::type projection_type(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    return types[eindex];
}

math::m4x4 view(component c)
{
    assert(c.is_valid());
    // ECS camera component does not store a graphics::camera handle.
    // Matrix computation requires the graphics subsystem; return identity as placeholder.
    return math::m4x4{ };
}

math::m4x4 projection(component c)
{
    assert(c.is_valid());
    return math::m4x4{ };
}

math::m4x4 inverse_projection(component c)
{
    assert(c.is_valid());
    return math::m4x4{ };
}

math::m4x4 view_projection(component c)
{
    assert(c.is_valid());
    return math::m4x4{ };
}

math::m4x4 inverse_view_projection(component c)
{
    assert(c.is_valid());
    return math::m4x4{ };
}

void set_up_vector(component c, math::v3 up)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    ups[eindex] = up;
}

void set_field_of_view(component c, f32 fov)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    fovs[eindex] = fov;
}

void set_aspect_ratio(component c, f32 aspect)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    aspects[eindex] = aspect;
}

void set_view_width(component c, f32 width)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    fovs[eindex] = width;
}

void set_view_height(component c, f32 height)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    aspects[eindex] = height;
}

void set_range(component c, f32 nz, f32 fz)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    assert(eindex < exists_flags.size() && exists_flags[eindex]);
    near_zs[eindex] = nz;
    far_zs[eindex] = fz;
}

// ============================================================================
// component member methods
// ============================================================================

math::v3 component::up_vector() const { return camera::up_vector(*this); }
f32 component::field_of_view() const { return camera::field_of_view(*this); }
f32 component::aspect_ratio() const { return camera::aspect_ratio(*this); }
f32 component::view_width() const { return camera::view_width(*this); }
f32 component::view_height() const { return camera::view_height(*this); }
f32 component::near_z() const { return camera::near_z(*this); }
f32 component::far_z() const { return camera::far_z(*this); }
graphics::camera::type component::projection_type() const { return camera::projection_type(*this); }

math::m4x4 component::view() const { return camera::view(*this); }
math::m4x4 component::projection() const { return camera::projection(*this); }
math::m4x4 component::inverse_projection() const { return camera::inverse_projection(*this); }
math::m4x4 component::view_projection() const { return camera::view_projection(*this); }
math::m4x4 component::inverse_view_projection() const { return camera::inverse_view_projection(*this); }

void component::set_up_vector(math::v3 up) { camera::set_up_vector(*this, up); }
void component::set_field_of_view(f32 fov) { camera::set_field_of_view(*this, fov); }
void component::set_aspect_ratio(f32 aspect) { camera::set_aspect_ratio(*this, aspect); }
void component::set_view_width(f32 width) { camera::set_view_width(*this, width); }
void component::set_view_height(f32 height) { camera::set_view_height(*this, height); }
void component::set_range(f32 nz, f32 fz) { camera::set_range(*this, nz, fz); }

} // namespace primal::camera
