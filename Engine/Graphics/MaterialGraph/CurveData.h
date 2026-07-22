#pragma once

#include "CommonHeaders.h"
#include <algorithm>
#include <cmath>

namespace primal::graphics::material_graph {

struct CurveControlPoint {
    f32 time{0.0f};
    f32 value{0.0f};
};

struct CurveData {
    static constexpr u32 MAX_POINTS = 8;
    CurveControlPoint points[MAX_POINTS];
    u32 point_count{0};

    f32 Evaluate(f32 t) const {
        if (point_count == 0) return 0.0f;
        if (point_count == 1) return points[0].value;
        if (t <= points[0].time) return points[0].value;
        if (t >= points[point_count - 1].time) return points[point_count - 1].value;

        for (u32 i = 0; i < point_count - 1; i++) {
            if (t >= points[i].time && t <= points[i + 1].time) {
                f32 range = points[i + 1].time - points[i].time;
                if (std::abs(range) < 1e-7f) return points[i].value;
                f32 alpha = (t - points[i].time) / range;
                return points[i].value + alpha * (points[i + 1].value - points[i].value);
            }
        }
        return points[point_count - 1].value;
    }
};

} // namespace primal::graphics::material_graph
