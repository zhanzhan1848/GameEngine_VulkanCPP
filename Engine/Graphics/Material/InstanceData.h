#pragma once
#include "CommonHeaders.h"

namespace primal::graphics {

struct alignas(16) InstanceData {
    math::m4x4 transform;
    f32        base_color[4]{1.f, 1.f, 1.f, 1.f};
    f32        roughness{0.5f};
    f32        metallic{0.0f};
    f32        alpha_cutoff{0.5f};
    f32        _pad{0.f};
};

static_assert(sizeof(InstanceData) == 96, "InstanceData must be 96 bytes");

} // namespace primal::graphics
