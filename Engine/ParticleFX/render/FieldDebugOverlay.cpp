// FieldDebugOverlay.cpp — 见 FieldDebugOverlay.h。

#include "FieldDebugOverlay.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>

namespace primal::pfx::render {

void append_box_wireframe(math::v3 center, math::v3 half, math::v4 color,
                          std::vector<debug_line>& out) {
    // 8 个顶点
    math::v3 v[8];
    for (int i = 0; i < 8; ++i) {
        v[i] = math::v3{
            center.x + ((i & 1) ? half.x : -half.x),
            center.y + ((i & 2) ? half.y : -half.y),
            center.z + ((i & 4) ? half.z : -half.z),
        };
    }
    // 12 条棱
    static const int edges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},   // X 方向
        {0,2},{1,3},{4,6},{5,7},   // Y 方向
        {0,4},{1,5},{2,6},{3,7},   // Z 方向
    };
    for (auto& e : edges)
        out.push_back({ v[e[0]], v[e[1]], color });
}

void append_sphere_wireframe(math::v3 center, f32 radius, math::v4 color,
                             std::vector<debug_line>& out, int segments) {
    // 三正交圆环（XY/XZ/YZ 平面）
    for (int ring = 0; ring < 3; ++ring) {
        for (int s = 0; s < segments; ++s) {
            f32 a0 = (f32)s / segments * 2.f * 3.14159265f;
            f32 a1 = (f32)(s + 1) / segments * 2.f * 3.14159265f;
            math::v3 p0, p1;
            switch (ring) {
                case 0: // XY
                    p0 = { center.x + radius * std::cos(a0), center.y + radius * std::sin(a0), center.z };
                    p1 = { center.x + radius * std::cos(a1), center.y + radius * std::sin(a1), center.z };
                    break;
                case 1: // XZ
                    p0 = { center.x + radius * std::cos(a0), center.y, center.z + radius * std::sin(a0) };
                    p1 = { center.x + radius * std::cos(a1), center.y, center.z + radius * std::sin(a1) };
                    break;
                default: // YZ
                    p0 = { center.x, center.y + radius * std::cos(a0), center.z + radius * std::sin(a0) };
                    p1 = { center.x, center.y + radius * std::cos(a1), center.z + radius * std::sin(a1) };
                    break;
            }
            out.push_back({ p0, p1, color });
        }
    }
}

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
