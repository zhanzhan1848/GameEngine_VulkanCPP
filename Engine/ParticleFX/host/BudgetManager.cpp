// BudgetManager.cpp — 见 BudgetManager.h。

#include "BudgetManager.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::host {

f32 budget_manager::lod_factor(f32 distance_to_camera) const {
    const f32 d = distance_to_camera < 0.f ? 0.f : distance_to_camera;
    if (d <= _config.lod_near) return 1.f;
    if (d >= _config.lod_far) return _config.min_factor;
    const f32 t = (d - _config.lod_near) / (_config.lod_far - _config.lod_near);
    return 1.f - t * (1.f - _config.min_factor);
}

} // namespace primal::pfx::host

#endif // !DISABLE_PARTICLE_FX
