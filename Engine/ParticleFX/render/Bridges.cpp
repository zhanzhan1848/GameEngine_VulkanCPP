// Bridges.cpp — 见 Bridges.h。

#include "Bridges.h"
#include "Particles/ParticleTypes.h"

#ifndef DISABLE_PARTICLE_FX

#include <cstring>

namespace primal::pfx::render {

u32 pack_sprite_instances(const attribute_store& store, void* out, u32 cap_bytes) {
    const u32 alive = store.alive_count();
    const u32 bytes_needed = alive * (u32)sizeof(particles::particle_data);
    if (!out || cap_bytes < bytes_needed) return alive;

    auto* rows = (particles::particle_data*)out;
    const math::v4* pos = store.v4_channel(attr::position);
    const math::v4* vel = store.v4_channel(attr::velocity);
    const math::v4* col = store.v4_channel(attr::color);
    const math::v4* scl = store.v4_channel(attr::scale_rotation);
    const math::v4* uv = store.v4_channel(attr::uv_params);

    u32 n = 0;
    const u32 cap_rows = cap_bytes / (u32)sizeof(particles::particle_data);
    for (u32 row = 0; row < store.capacity() && n < cap_rows; ++row) {
        if (!store.is_row_alive(row)) continue;
        rows[n].position = pos[row];
        rows[n].velocity = vel[row];
        rows[n].color = col[row];
        rows[n].scale_rotation = scl[row];
        rows[n].uv_params = uv[row];
        ++n;
    }
    return n;
}

u32 pack_mesh_instances(const attribute_store& store, mesh_instance_data* out, u32 cap) {
    if (!out) return store.alive_count();
    const math::v4* pos = store.v4_channel(attr::position);
    const math::v4* scl = store.v4_channel(attr::scale_rotation);

    u32 n = 0;
    for (u32 row = 0; row < store.capacity() && n < cap; ++row) {
        if (!store.is_row_alive(row)) continue;
        mesh_instance_data& inst = out[n];
        inst.pos[0] = pos[row].x;
        inst.pos[1] = pos[row].y;
        inst.pos[2] = pos[row].z;
        inst.pad0 = 0.f;
        inst.rot[0] = 0.f;
        inst.rot[1] = 0.f;
        inst.rot[2] = 0.f;
        inst.rot[3] = 1.f;   // 单位四元数
        inst.scale[0] = scl[row].x;
        inst.scale[1] = scl[row].y;
        inst.pad1[0] = inst.pad1[1] = 0.f;
        ++n;
    }
    return n;
}

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
