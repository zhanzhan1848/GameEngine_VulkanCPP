// EmissiveGI.cpp — 见 EmissiveGI.h。

#include "EmissiveGI.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>

namespace primal::pfx::interop {

bool emissive_gi_injector::aggregate(const std::vector<light_sample>& lights,
                                     math::v3& out_radiance) const {
    f32 r = 0.f, g = 0.f, b = 0.f;
    u32 alive = 0;
    for (const auto& l : lights) {
        const f32 inten = l.intensity > 0.f ? l.intensity : 0.f;
        r += l.color.x * inten;
        g += l.color.y * inten;
        b += l.color.z * inten;
        ++alive;
    }
    if (alive < _cfg.min_alive) return false;

    out_radiance = math::v3{ r * _cfg.radiance_scale, g * _cfg.radiance_scale, b * _cfg.radiance_scale };
    // T4.4 强度钳制（≤ 8.0 逐通道，与 LightEval 双重一致）
    if (out_radiance.x > kMaxEmissiveRadiance) out_radiance.x = kMaxEmissiveRadiance;
    if (out_radiance.y > kMaxEmissiveRadiance) out_radiance.y = kMaxEmissiveRadiance;
    if (out_radiance.z > kMaxEmissiveRadiance) out_radiance.z = kMaxEmissiveRadiance;
    return true;
}

bool emissive_gi_injector::tick(const std::vector<light_sample>& lights, emissive_writer& writer) const {
    math::v3 radiance;
    if (!aggregate(lights, radiance)) return false;
    writer.write(_cfg.reserved_instance_id, radiance);
    return true;
}

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
