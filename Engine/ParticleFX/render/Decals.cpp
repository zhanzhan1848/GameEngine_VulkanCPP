// Decals.cpp — 见 Decals.h。

#include "Decals.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

u32 pack_decal_instances(const attribute_store& store, f32 radius, f32 intensity,
                         decal_instance_data* out, u32 cap) {
    if (!out) return store.alive_count();
    const math::v4* pos = store.v4_channel(attr::position);
    const math::v4* col = store.v4_channel(attr::color);

    u32 n = 0;
    for (u32 row = 0; row < store.capacity() && n < cap; ++row) {
        if (!store.is_row_alive(row)) continue;
        decal_instance_data& d = out[n];
        d.center_radius[0] = pos[row].x;
        d.center_radius[1] = pos[row].y;
        d.center_radius[2] = pos[row].z;
        d.center_radius[3] = radius;
        d.color[0] = col[row].x;
        d.color[1] = col[row].y;
        d.color[2] = col[row].z;
        d.color[3] = intensity;
        d.params[0] = 0.6f;   // 软边比例
        d.params[1] = d.params[2] = d.params[3] = 0.f;
        ++n;
    }
    return n;
}

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
