#pragma once
#include "CommonHeaders.h"

namespace primal::graphics {

enum class ShaderTechnique : u8 {
    Opaque = 0,
    AlphaClip,
    Foliage,
    Water,
    Transparent,
    Unlit,
    VolumeProxy,
    Count
};

constexpr u32 TechniqueCount = static_cast<u32>(ShaderTechnique::Count);

} // namespace primal::graphics
