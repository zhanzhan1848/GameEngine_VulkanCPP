#pragma once

#include "Graphics/Lumen/GICacheView.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace primal::graphics::lumen {

// Inspect IEEE-754 exponent bits: std::isfinite may be folded to true under
// the Engine target's -ffast-math flags.
inline bool IsScreenProbeFinite(const float& value) {
    static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

// SPGI samples the card grid, rather than just the lighting atlas.
inline bool IsScreenProbeSurfaceCacheValid(const SurfaceCacheView& view) {
    const auto invalid = rhi::handles::INVALID_RESOURCE;
    return view.lighting_atlas != invalid && view.card_data != invalid &&
           view.card_lookup != invalid && view.grid_offsets != invalid &&
           view.grid_cards != invalid && view.atlas_size > 0 && view.max_card_count > 0;
}

inline bool IsScreenProbeDDGIValid(const DDGIVolumeView& view) {
    if (view.irradiance == rhi::handles::INVALID_RESOURCE ||
        !IsScreenProbeFinite(view.spacing) || view.spacing <= 0.0f ||
        !IsScreenProbeFinite(view.origin.x) || !IsScreenProbeFinite(view.origin.y) ||
        !IsScreenProbeFinite(view.origin.z)) return false;
    double probes = 1.0;
    for (float count : {view.counts.x, view.counts.y, view.counts.z}) {
        if (!IsScreenProbeFinite(count) || count < 1.0f || std::floor(count) != count)
            return false;
        probes *= static_cast<double>(count);
    }
    // Shader addresses SH coefficients using uint (nine float4s per probe).
    return probes <= static_cast<double>(std::numeric_limits<uint32_t>::max()) / 9.0;
}

} // namespace primal::graphics::lumen
