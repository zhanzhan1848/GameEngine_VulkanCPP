#pragma once

// FieldDebugOverlay.h — R3 场预览：KillVolume 边界线框生成（架构文档 §7-N6）。
// 生成 12 条 AABB 棱边或球体经线/纬线段对，供 debug_draw 或 LinePass 消费。
// 纯 CPU 计算，无 GPU 依赖；颜色编码：红=KillVolume，绿=SDF 边界。

#include "../PfxTypes.h"

#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

struct debug_line {
    math::v3 from;
    math::v3 to;
    math::v4 color;
};

/// <summary>AABB 线框：12 条棱边 × 2 顶点 = 24 个端点。</summary>
void append_box_wireframe(math::v3 center, math::v3 half, math::v4 color,
                          std::vector<debug_line>& out);

/// <summary>球体线框：3 个正交圆环 × 16 段 = 48 条线段。</summary>
void append_sphere_wireframe(math::v3 center, f32 radius, math::v4 color,
                             std::vector<debug_line>& out, int segments = 16);

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX