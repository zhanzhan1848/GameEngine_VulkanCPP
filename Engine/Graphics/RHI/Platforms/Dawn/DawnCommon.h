#pragma once

#include "Engine/Common/CommonHeaders.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"
#include <webgpu/webgpu.h>

// Emscripten's webgpu.h transitively includes X11 headers which define
// macros like None, Success, etc. that conflict with C++ enum values.
#ifdef __EMSCRIPTEN__
#undef None
#undef Success
#undef CopyFromParent
#endif

namespace primal::graphics::rhi {

// === Format conversion helpers ===

inline WGPUTextureFormat ToWGPUTextureFormat(DataFormat format) {
    switch (format) {
        case DataFormat::BGRA8_UNorm:       return WGPUTextureFormat_BGRA8Unorm;
        case DataFormat::RGBA8_UNorm:       return WGPUTextureFormat_RGBA8Unorm;
        case DataFormat::RGBA8_sRGB:        return WGPUTextureFormat_RGBA8UnormSrgb;
        case DataFormat::RGBA16_Float:      return WGPUTextureFormat_RGBA16Float;
        case DataFormat::RGBA32_Float:      return WGPUTextureFormat_RGBA32Float;
        case DataFormat::R16_UNorm:         return WGPUTextureFormat_R16Unorm;
        case DataFormat::R16_Float:         return WGPUTextureFormat_R16Float;
        case DataFormat::R32_Float:         return WGPUTextureFormat_R32Float;
        case DataFormat::R8_UNorm:          return WGPUTextureFormat_R8Unorm;
        case DataFormat::RG16_Float:        return WGPUTextureFormat_RG16Float;
        case DataFormat::RG32_Float:        return WGPUTextureFormat_RG32Float;
        case DataFormat::D16_UNorm:         return WGPUTextureFormat_Depth16Unorm;
        case DataFormat::D24_UNorm_S8_UInt: return WGPUTextureFormat_Depth24PlusStencil8;
        case DataFormat::D32_Float:         return WGPUTextureFormat_Depth32Float;
        case DataFormat::D32_Float_S8X24_UInt: return WGPUTextureFormat_Depth32FloatStencil8;
        case DataFormat::BC1_UNorm:         return WGPUTextureFormat_BC1RGBAUnorm;
        case DataFormat::BC2_UNorm:         return WGPUTextureFormat_BC2RGBAUnorm;
        case DataFormat::BC3_UNorm:         return WGPUTextureFormat_BC3RGBAUnorm;
        case DataFormat::BC4_UNorm:         return WGPUTextureFormat_BC4RUnorm;
        case DataFormat::BC5_UNorm:         return WGPUTextureFormat_BC5RGUnorm;
        case DataFormat::BC7_UNorm:         return WGPUTextureFormat_BC7RGBAUnorm;
        case DataFormat::BC1_sRGB:          return WGPUTextureFormat_BC1RGBAUnormSrgb;
        case DataFormat::BC3_sRGB:          return WGPUTextureFormat_BC3RGBAUnormSrgb;
        case DataFormat::BC7_sRGB:          return WGPUTextureFormat_BC7RGBAUnormSrgb;
        default:                            return WGPUTextureFormat_Undefined;
    }
}

inline WGPUVertexFormat ToWGPUVertexFormat(DataFormat format) {
    switch (format) {
        case DataFormat::R32_Float:    return WGPUVertexFormat_Float32;
        case DataFormat::RG32_Float:   return WGPUVertexFormat_Float32x2;
        case DataFormat::RGB32_Float:  return WGPUVertexFormat_Float32x3;
        case DataFormat::RGBA32_Float: return WGPUVertexFormat_Float32x4;
        case DataFormat::R8_UNorm:     return WGPUVertexFormat_Unorm8;
        case DataFormat::RG8_UNorm:    return WGPUVertexFormat_Unorm8x2;
        case DataFormat::RGBA8_UNorm:  return WGPUVertexFormat_Unorm8x4;
        case DataFormat::R16_UNorm:    return WGPUVertexFormat_Unorm16;
        case DataFormat::RG16_Float:   return WGPUVertexFormat_Float16x2;
        case DataFormat::RGBA16_Float: return WGPUVertexFormat_Float16x4;
        case DataFormat::R32_UInt:     return WGPUVertexFormat_Uint32;
        case DataFormat::RG32_UInt:    return WGPUVertexFormat_Uint32x2;
        default:                       return static_cast<WGPUVertexFormat>(0);
    }
}

inline WGPUPrimitiveTopology ToWGPUPrimitiveTopology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::PointList:     return WGPUPrimitiveTopology_PointList;
        case PrimitiveTopology::LineList:      return WGPUPrimitiveTopology_LineList;
        case PrimitiveTopology::LineStrip:     return WGPUPrimitiveTopology_LineStrip;
        case PrimitiveTopology::TriangleList:  return WGPUPrimitiveTopology_TriangleList;
        case PrimitiveTopology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
        default:                               return WGPUPrimitiveTopology_TriangleList;
    }
}

inline WGPUCompareFunction ToWGPUCompareFunction(ComparisonFunc func) {
    switch (func) {
        case ComparisonFunc::Never:        return WGPUCompareFunction_Never;
        case ComparisonFunc::Less:         return WGPUCompareFunction_Less;
        case ComparisonFunc::Equal:        return WGPUCompareFunction_Equal;
        case ComparisonFunc::LessEqual:    return WGPUCompareFunction_LessEqual;
        case ComparisonFunc::Greater:      return WGPUCompareFunction_Greater;
        case ComparisonFunc::NotEqual:     return WGPUCompareFunction_NotEqual;
        case ComparisonFunc::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
        case ComparisonFunc::Always:       return WGPUCompareFunction_Always;
        default:                           return WGPUCompareFunction_Less;
    }
}

inline WGPUStencilOperation ToWGPUStencilOperation(StencilOp op) {
    switch (op) {
        case StencilOp::Keep:    return WGPUStencilOperation_Keep;
        case StencilOp::Zero:    return WGPUStencilOperation_Zero;
        case StencilOp::Replace: return WGPUStencilOperation_Replace;
        case StencilOp::IncSat:  return WGPUStencilOperation_IncrementClamp;
        case StencilOp::DecSat:  return WGPUStencilOperation_DecrementClamp;
        case StencilOp::Invert:  return WGPUStencilOperation_Invert;
        case StencilOp::Inc:     return WGPUStencilOperation_IncrementWrap;
        case StencilOp::Dec:     return WGPUStencilOperation_DecrementWrap;
        default:                 return WGPUStencilOperation_Keep;
    }
}

inline WGPUBlendFactor ToWGPUBlendFactor(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::Zero:         return WGPUBlendFactor_Zero;
        case BlendFactor::One:          return WGPUBlendFactor_One;
        case BlendFactor::SrcColor:     return WGPUBlendFactor_Src;
        case BlendFactor::InvSrcColor:  return WGPUBlendFactor_OneMinusSrc;
        case BlendFactor::SrcAlpha:     return WGPUBlendFactor_SrcAlpha;
        case BlendFactor::InvSrcAlpha:  return WGPUBlendFactor_OneMinusSrcAlpha;
        case BlendFactor::DestAlpha:    return WGPUBlendFactor_DstAlpha;
        case BlendFactor::InvDestAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
        case BlendFactor::DestColor:    return WGPUBlendFactor_Dst;
        case BlendFactor::InvDestColor: return WGPUBlendFactor_OneMinusDst;
        case BlendFactor::SrcAlphaSat:  return WGPUBlendFactor_SrcAlphaSaturated;
        default:                        return WGPUBlendFactor_Zero;
    }
}

inline WGPUBlendOperation ToWGPUBlendOperation(BlendOp op) {
    switch (op) {
        case BlendOp::Add:              return WGPUBlendOperation_Add;
        case BlendOp::Subtract:         return WGPUBlendOperation_Subtract;
        case BlendOp::RevSubtract:       return WGPUBlendOperation_ReverseSubtract;
        case BlendOp::Min:              return WGPUBlendOperation_Min;
        case BlendOp::Max:              return WGPUBlendOperation_Max;
        default:                        return WGPUBlendOperation_Add;
    }
}

inline WGPUTextureDimension ToWGPUTextureDimension(TextureType type) {
    switch (type) {
        case TextureType::Texture1D: return WGPUTextureDimension_1D;
        case TextureType::Texture2D:
        case TextureType::Texture2DArray:
        case TextureType::TextureCube:
        case TextureType::TextureCubeArray: return WGPUTextureDimension_2D;
        case TextureType::Texture3D: return WGPUTextureDimension_3D;
        default: return WGPUTextureDimension_2D;
    }
}

inline WGPUAddressMode ToWGPUAddressMode(TextureAddressMode mode) {
    switch (mode) {
        case TextureAddressMode::Wrap:   return WGPUAddressMode_Repeat;
        case TextureAddressMode::Mirror: return WGPUAddressMode_MirrorRepeat;
        case TextureAddressMode::Clamp:  return WGPUAddressMode_ClampToEdge;
        default:                         return WGPUAddressMode_ClampToEdge;
    }
}

inline WGPUFilterMode ToWGPUFilterMode(FilterMode filter) {
    switch (filter) {
        case FilterMode::Point:  return WGPUFilterMode_Nearest;
        case FilterMode::Linear: return WGPUFilterMode_Linear;
        default:                 return WGPUFilterMode_Linear;
    }
}

inline WGPUMipmapFilterMode ToWGPUMipmapFilterMode(FilterMode filter) {
    switch (filter) {
        case FilterMode::Point:  return WGPUMipmapFilterMode_Nearest;
        case FilterMode::Linear: return WGPUMipmapFilterMode_Linear;
        default:                 return WGPUMipmapFilterMode_Linear;
    }
}

inline WGPULoadOp ToWGPULoadOp(LoadAction action) {
    switch (action) {
        case LoadAction::Load:     return WGPULoadOp_Load;
        case LoadAction::Clear:    return WGPULoadOp_Clear;
        case LoadAction::DontCare: return WGPULoadOp_Clear;
        default:                   return WGPULoadOp_Clear;
    }
}

inline WGPUStoreOp ToWGPUStoreOp(StoreAction action) {
    switch (action) {
        case StoreAction::Store:    return WGPUStoreOp_Store;
        case StoreAction::DontCare: return WGPUStoreOp_Discard;
        default:                    return WGPUStoreOp_Store;
    }
}

// Helper to convert a const char* to WGPUStringView for the new webgpu.h API.
// Uses WGPU_STRLEN sentinel to indicate null-terminated strings.
inline WGPUStringView ToWGPUStringView(const char* str) {
    return { str, WGPU_STRLEN };
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
