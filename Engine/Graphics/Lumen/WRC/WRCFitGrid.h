#pragma once

#include "../../../Utilities/MathTypes.h"
#include <algorithm>
#include <cmath>

namespace primal::graphics::lumen {

// ============================================================================
// WRCFitGrid — [WRC] 场景贴合探针网格推导（A5 placement 自动化）。
//
// 刻意复制 DDGIFitGrid.h 的算术契约而非 include 复用（设计文档 §1.2-4 同款
// 理由：两条线的 placement 语义独立演进；§2.5 "起步复用场景贴合思想但独立
// 实现"）。
//
// 契约（对齐 DDGI scene-fit，Docs/completed/2026-09-18-ddgi-scene-fit-probe-
// grid-plan.md §3）：
//   N_i    = clamp(ceil(E_i / S) + 1, min_count, max_count)   per axis
//   origin = fit_min（probe[0] 落在覆盖盒 min 角，RTXGI (N-1)×S 约定）
//   covered_i = (N_i - 1) × S  >=  E_i + 2·margin
//
// 需求驱动预算（A5，UE NumProbesToTraceBudget 精神）：update list 的轮转
// 窗口按"近期被 SPGI 消费次数"排序——消费计数（u32/probe，SPGI finalize
// 原子累加）周期性读回后，窗口探针按计数降序 + 轮转游标混合排序，使
// 预算优先花在被消费的探针上。in-geometry / 未收敛探针（state bit）由
// WRCUpdateRadiance 排除后计数自然为 0，自动退到窗口尾部。
// ============================================================================

struct WRCFitGrid {
    u32      count_x{4};
    u32      count_y{4};
    u32      count_z{4};
    math::v3 origin{};    // probe[0] world position = fit_min
    math::v3 covered{};   // (N-1)*S per axis — actual world coverage
};

inline u32 WRCFitCountForAxis(float extent, float spacing, u32 min_count = 4, u32 max_count = 64) {
    const float s = (spacing > 0.0f) ? spacing : 1.0f;
    u32 n = static_cast<u32>(std::ceil(std::max(extent, 0.0f) / s)) + 1u;
    return std::clamp(n, min_count, max_count);
}

inline WRCFitGrid WRCFitGridFromBounds(math::v3 bounds_min, math::v3 bounds_max,
                                       float spacing, float margin = 0.0f,
                                       u32 min_count = 4, u32 max_count = 64) {
    WRCFitGrid g{};
    const math::v3 fit_min{ bounds_min.x - margin, bounds_min.y - margin, bounds_min.z - margin };
    const math::v3 fit_max{ bounds_max.x + margin, bounds_max.y + margin, bounds_max.z + margin };

    const float ex = std::max(fit_max.x - fit_min.x, 0.0f);
    const float ey = std::max(fit_max.y - fit_min.y, 0.0f);
    const float ez = std::max(fit_max.z - fit_min.z, 0.0f);

    g.count_x = WRCFitCountForAxis(ex, spacing, min_count, max_count);
    g.count_y = WRCFitCountForAxis(ey, spacing, min_count, max_count);
    g.count_z = WRCFitCountForAxis(ez, spacing, min_count, max_count);

    g.origin  = fit_min;
    const float s = (spacing > 0.0f) ? spacing : 1.0f;
    g.covered = math::v3{ (float)(g.count_x - 1) * s,
                          (float)(g.count_y - 1) * s,
                          (float)(g.count_z - 1) * s };
    return g;
}

} // namespace primal::graphics::lumen
