#pragma once
#include "CommonHeaders.h"

namespace primal::graphics {

struct alignas(16) InstanceData {
    math::m4x4 transform;
    f32        base_color[4]{1.f, 1.f, 1.f, 1.f};
    f32        roughness{0.5f};
    f32        metallic{0.0f};
    f32        alpha_cutoff{0.5f};
    // P2-B（sponza-gap-audit §4.7）: per-instance 透光材质掩码（0 = 石材等
    // 不透光，1 = 薄布料全开）。占用原 _pad 槽位，struct 尺寸/顶点步长不变。
    f32        transmission{0.f};
};

static_assert(sizeof(InstanceData) == 96, "InstanceData must be 96 bytes");

} // namespace primal::graphics
