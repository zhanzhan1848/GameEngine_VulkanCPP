#pragma once
#include "Engine/Graphics/RHI/Core/RHITypes.h"

namespace primal::tests::dawn_sponza {
// The frozen Sponza GPU-driven path stores normals and ORM in BGRA8_UNorm.
// RGBA16F remains accepted for independent numerical fixtures, not the scene.
inline bool IsGBufferAttributeFormat(graphics::rhi::DataFormat format) {
    return format == graphics::rhi::DataFormat::BGRA8_UNorm ||
           format == graphics::rhi::DataFormat::RGBA16_Float;
}
inline unsigned CapturePixelBytes(graphics::rhi::DataFormat format) {
    using graphics::rhi::DataFormat;
    switch (format) {
        case DataFormat::D32_Float:
        case DataFormat::R32_Float:
        case DataFormat::RGBA8_UNorm:
        case DataFormat::BGRA8_UNorm: return 4;
        case DataFormat::RGBA16_Float: return 8;
        default: return 0;
    }
}
inline const char* CaptureFormatName(graphics::rhi::DataFormat format) {
    using graphics::rhi::DataFormat;
    switch (format) {
        case DataFormat::D32_Float: return "d32f";
        case DataFormat::R32_Float: return "r32f";
        case DataFormat::RGBA8_UNorm: return "rgba8";
        case DataFormat::BGRA8_UNorm: return "bgra8";
        case DataFormat::RGBA16_Float: return "rgba16f";
        default: return "unsupported";
    }
}
}
