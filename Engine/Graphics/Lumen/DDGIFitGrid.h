#pragma once

#include "../../Utilities/MathTypes.h"
#include <algorithm>
#include <cmath>

namespace primal::graphics::lumen {

// ============================================================================
// DDGI scene-fit probe grid derivation (pure arithmetic — shared by
// StandardRenderPipeline::InitializeLumenPasses and the unit tests).
// Contract: Docs/completed/2026-09-18-ddgi-scene-fit-probe-grid-plan.md §3
//   N_i    = clamp(ceil(E_i / S) + 1, min_count, max_count)   per axis
//   origin = fit_min (probe[0] sits on the coverage box's min corner —
//            RTXGI (N-1)×S coverage convention, reference doc §2.1)
//   covered_i = (N_i - 1) × S  >=  E_i + 2·margin
// ============================================================================

struct DDGIFitGrid {
    u32      count_x{4};
    u32      count_y{4};
    u32      count_z{4};
    math::v3 origin{};    // probe[0] world position = fit_min
    math::v3 covered{};   // (N-1)*S per axis — actual world coverage
};

// Per-axis count for one extent: ceil(E/S)+1, clamped to [min_count, max_count]
// (4..64 = the GIEditorPanel volume clamp range). spacing <= 0 falls back to 1
// so a bad config cannot produce a degenerate grid.
inline u32 DDGIFitCountForAxis(float extent, float spacing, u32 min_count = 4, u32 max_count = 64) {
    const float s = (spacing > 0.0f) ? spacing : 1.0f;
    u32 n = static_cast<u32>(std::ceil(std::max(extent, 0.0f) / s)) + 1u;
    return std::clamp(n, min_count, max_count);
}

// Derive the fitted grid for a world-space coverage box. margin expands the
// box outward on every side before the counts are derived.
inline DDGIFitGrid DDGIFitGridFromBounds(math::v3 bounds_min, math::v3 bounds_max,
                                         float spacing, float margin = 0.0f,
                                         u32 min_count = 4, u32 max_count = 64) {
    DDGIFitGrid g{};
    const math::v3 fit_min{ bounds_min.x - margin, bounds_min.y - margin, bounds_min.z - margin };
    const math::v3 fit_max{ bounds_max.x + margin, bounds_max.y + margin, bounds_max.z + margin };

    const float ex = std::max(fit_max.x - fit_min.x, 0.0f);
    const float ey = std::max(fit_max.y - fit_min.y, 0.0f);
    const float ez = std::max(fit_max.z - fit_min.z, 0.0f);

    g.count_x = DDGIFitCountForAxis(ex, spacing, min_count, max_count);
    g.count_y = DDGIFitCountForAxis(ey, spacing, min_count, max_count);
    g.count_z = DDGIFitCountForAxis(ez, spacing, min_count, max_count);

    g.origin  = fit_min;
    const float s = (spacing > 0.0f) ? spacing : 1.0f;
    g.covered = math::v3{ (float)(g.count_x - 1) * s,
                          (float)(g.count_y - 1) * s,
                          (float)(g.count_z - 1) * s };
    return g;
}

} // namespace primal::graphics::lumen
