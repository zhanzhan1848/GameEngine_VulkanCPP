#include "Collider.h"
#include "Entity.h"

namespace primal::collider {

namespace {
    utl::vector<u8>      exists_flags;
    utl::vector<u32>     shapes;
    utl::vector<math::v3> half_extents_arr;
    utl::vector<math::v3> offsets;
    utl::vector<u8>      trigger_flags;
    utl::vector<u32>     layers;
} // anonymous namespace

component create(init_info info, game_entity::entity entity)
{
    assert(entity.is_valid());
    const id::id_type eindex{ id::index(entity.get_id()) };

    if (exists_flags.size() <= eindex) {
        exists_flags.resize(eindex + 1, 0);
        shapes.resize(eindex + 1, 0);
        half_extents_arr.resize(eindex + 1, math::v3{0.5f, 0.5f, 0.5f});
        offsets.resize(eindex + 1, math::v3{0.f, 0.f, 0.f});
        trigger_flags.resize(eindex + 1, 0);
        layers.resize(eindex + 1, 1);
    }

    shapes[eindex] = info.shape;
    half_extents_arr[eindex] = info.half_extents;
    offsets[eindex] = info.offset;
    trigger_flags[eindex] = info.is_trigger ? 1 : 0;
    layers[eindex] = info.layer;
    exists_flags[eindex] = 1;

    return component{ collider_component_id{ entity.get_id() } };
}

void remove(component c)
{
    assert(c.is_valid());
    const id::id_type eindex{ id::index(c.get_id()) };
    if (eindex < exists_flags.size()) {
        exists_flags[eindex] = 0;
    }
}

u32 shape(component c)
{
    assert(c.is_valid());
    return shapes[id::index(c.get_id())];
}

math::v3 half_extents(component c)
{
    assert(c.is_valid());
    return half_extents_arr[id::index(c.get_id())];
}

math::v3 offset(component c)
{
    assert(c.is_valid());
    return offsets[id::index(c.get_id())];
}

bool is_trigger(component c)
{
    assert(c.is_valid());
    return trigger_flags[id::index(c.get_id())] != 0;
}

u32 layer(component c)
{
    assert(c.is_valid());
    return layers[id::index(c.get_id())];
}

void set_shape(component c, u32 shape)
{
    assert(c.is_valid());
    shapes[id::index(c.get_id())] = shape;
}

void set_half_extents(component c, math::v3 half)
{
    assert(c.is_valid());
    half_extents_arr[id::index(c.get_id())] = half;
}

void set_offset(component c, math::v3 offset_v)
{
    assert(c.is_valid());
    offsets[id::index(c.get_id())] = offset_v;
}

void set_is_trigger(component c, bool trigger)
{
    assert(c.is_valid());
    trigger_flags[id::index(c.get_id())] = trigger ? 1 : 0;
}

void set_layer(component c, u32 layer)
{
    assert(c.is_valid());
    layers[id::index(c.get_id())] = layer;
}

// ============================================================================
// component 句柄成员转发（EngineAPI/ColliderComponent.h）
// ============================================================================

u32 component::shape() const { return collider::shape(*this); }
math::v3 component::half_extents() const { return collider::half_extents(*this); }
math::v3 component::offset() const { return collider::offset(*this); }
bool component::is_trigger() const { return collider::is_trigger(*this); }
u32 component::layer() const { return collider::layer(*this); }

void component::set_shape(u32 shape_v) { collider::set_shape(*this, shape_v); }
void component::set_half_extents(math::v3 half) { collider::set_half_extents(*this, half); }
void component::set_offset(math::v3 offset_v) { collider::set_offset(*this, offset_v); }
void component::set_is_trigger(bool trigger) { collider::set_is_trigger(*this, trigger); }
void component::set_layer(u32 layer_v) { collider::set_layer(*this, layer_v); }

} // namespace primal::collider
