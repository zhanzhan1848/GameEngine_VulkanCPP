/**
 * @file D3D12Format.cpp
 * @brief RHI DataFormat <-> DXGI_FORMAT 双向映射实现（平台中立）
 * @details 值来源 D3D12Abi.h（Microsoft 公开 ABI）。逐值断言见
 *          EngineTest/UnitTests/RHI/D3D12/TestD3D12Format.cpp。
 */

#include "D3D12Format.h"

namespace primal::graphics::rhi::d3d12 {

u32 ToDxgiFormat(DataFormat format) {
    switch (format) {
        case DataFormat::Unknown:
            return kDXGIFormat_Unknown;

        // --- 8 位 ---
        case DataFormat::R8_UNorm:  return kDXGIFormat_R8_UNorm;
        case DataFormat::R8_SNorm:  return kDXGIFormat_R8_SNorm;
        case DataFormat::R8_UInt:   return kDXGIFormat_R8_UInt;
        case DataFormat::R8_SInt:   return kDXGIFormat_R8_SInt;

        // --- 16 位 ---
        case DataFormat::R16_UNorm: return kDXGIFormat_R16_UNorm;
        case DataFormat::R16_SNorm: return kDXGIFormat_R16_SNorm;
        case DataFormat::R16_UInt:  return kDXGIFormat_R16_UInt;
        case DataFormat::R16_SInt:  return kDXGIFormat_R16_SInt;
        case DataFormat::R16_Float: return kDXGIFormat_R16_Float;
        case DataFormat::RG8_UNorm: return kDXGIFormat_R8G8_UNorm;
        case DataFormat::RG8_SNorm: return kDXGIFormat_R8G8_SNorm;
        case DataFormat::RG8_UInt:  return kDXGIFormat_R8G8_UInt;
        case DataFormat::RG8_SInt:  return kDXGIFormat_R8G8_SInt;

        // --- 24 位：DXGI 无 24-bit 布局（Vulkan 有 R8G8B8；跨端缺口登记） ---
        case DataFormat::R8G8B8_UNorm:
        case DataFormat::R8G8B8_SNorm:
        case DataFormat::R8G8B8_UInt:
        case DataFormat::R8G8B8_SInt:
            return kDXGIFormat_Unknown;

        // --- 32 位 R32（UNorm/SNorm 按 Vulkan 先例 fallback FLOAT） ---
        case DataFormat::R32_UNorm:
        case DataFormat::R32_SNorm:
        case DataFormat::R32_Float: return kDXGIFormat_R32_Float;
        case DataFormat::R32_UInt:  return kDXGIFormat_R32_UInt;
        case DataFormat::R32_SInt:  return kDXGIFormat_R32_SInt;

        // --- 32 位 RG16 ---
        case DataFormat::RG16_UNorm: return kDXGIFormat_R16G16_UNorm;
        case DataFormat::RG16_SNorm: return kDXGIFormat_R16G16_SNorm;
        case DataFormat::RG16_UInt:  return kDXGIFormat_R16G16_UInt;
        case DataFormat::RG16_SInt:  return kDXGIFormat_R16G16_SInt;
        case DataFormat::RG16_Float: return kDXGIFormat_R16G16_Float;

        // --- 32 位 RG8B8A8（历史命名 = RGBA8 字节序，见 VulkanMath.h 同款映射） ---
        case DataFormat::RG8B8A8_UNorm: return kDXGIFormat_R8G8B8A8_UNorm;
        case DataFormat::RG8B8A8_SNorm: return kDXGIFormat_R8G8B8A8_SNorm;
        case DataFormat::RG8B8A8_UInt:  return kDXGIFormat_R8G8B8A8_UInt;
        case DataFormat::RG8B8A8_SInt:  return kDXGIFormat_R8G8B8A8_SInt;

        // --- 32 位 BGRA8（DXGI 只有 UNORM/UNORM_SRGB/TYPELESS） ---
        case DataFormat::BGRA8_UNorm: return kDXGIFormat_B8G8R8A8_UNorm;
        case DataFormat::BGRA8_SNorm:
        case DataFormat::BGRA8_UInt:
        case DataFormat::BGRA8_SInt:
            return kDXGIFormat_Unknown;

        // --- 32 位 RGBA8 ---
        case DataFormat::RGBA8_UNorm: return kDXGIFormat_R8G8B8A8_UNorm;
        case DataFormat::RGBA8_SNorm: return kDXGIFormat_R8G8B8A8_SNorm;
        case DataFormat::RGBA8_UInt:  return kDXGIFormat_R8G8B8A8_UInt;
        case DataFormat::RGBA8_SInt:  return kDXGIFormat_R8G8B8A8_SInt;
        case DataFormat::RGBA8_sRGB:  return kDXGIFormat_R8G8B8A8_UNorm_sRGB;

        // --- 64 位 RG32（UNorm/SNorm fallback FLOAT） ---
        case DataFormat::RG32_UNorm:
        case DataFormat::RG32_SNorm:
        case DataFormat::RG32_Float: return kDXGIFormat_R32G32_Float;
        case DataFormat::RG32_UInt:  return kDXGIFormat_R32G32_UInt;
        case DataFormat::RG32_SInt:  return kDXGIFormat_R32G32_SInt;

        // --- 96 位 RGB32（UNorm/SNorm fallback FLOAT） ---
        case DataFormat::RGB32_UNorm:
        case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_Float: return kDXGIFormat_R32G32B32_Float;
        case DataFormat::RGB32_UInt:  return kDXGIFormat_R32G32B32_UInt;
        case DataFormat::RGB32_SInt:  return kDXGIFormat_R32G32B32_SInt;

        // --- 64 位 RGBA16 ---
        case DataFormat::RGBA16_UNorm: return kDXGIFormat_R16G16B16A16_UNorm;
        case DataFormat::RGBA16_SNorm: return kDXGIFormat_R16G16B16A16_SNorm;
        case DataFormat::RGBA16_UInt:  return kDXGIFormat_R16G16B16A16_UInt;
        case DataFormat::RGBA16_SInt:  return kDXGIFormat_R16G16B16A16_SInt;
        case DataFormat::RGBA16_Float: return kDXGIFormat_R16G16B16A16_Float;

        // --- 128 位 RGBA32（UNorm/SNorm fallback FLOAT；sRGB 无 DXGI 格式） ---
        case DataFormat::RGBA32_UNorm:
        case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_Float: return kDXGIFormat_R32G32B32A32_Float;
        case DataFormat::RGBA32_UInt:  return kDXGIFormat_R32G32B32A32_UInt;
        case DataFormat::RGBA32_SInt:  return kDXGIFormat_R32G32B32A32_SInt;
        case DataFormat::RGBA32_sRGB:  return kDXGIFormat_Unknown;

        // --- BC 压缩 ---
        case DataFormat::BC1_UNorm: return kDXGIFormat_BC1_UNorm;
        case DataFormat::BC1_sRGB:  return kDXGIFormat_BC1_UNorm_sRGB;
        case DataFormat::BC2_UNorm: return kDXGIFormat_BC2_UNorm;
        case DataFormat::BC2_sRGB:  return kDXGIFormat_BC2_UNorm_sRGB;
        case DataFormat::BC3_UNorm: return kDXGIFormat_BC3_UNorm;
        case DataFormat::BC3_sRGB:  return kDXGIFormat_BC3_UNorm_sRGB;
        case DataFormat::BC4_UNorm: return kDXGIFormat_BC4_UNorm;
        case DataFormat::BC4_SNorm: return kDXGIFormat_BC4_SNorm;
        case DataFormat::BC5_UNorm: return kDXGIFormat_BC5_UNorm;
        case DataFormat::BC5_SNorm: return kDXGIFormat_BC5_SNorm;
        case DataFormat::BC6H_UF16: return kDXGIFormat_BC6H_UF16;
        case DataFormat::BC6H_SF16: return kDXGIFormat_BC6H_SF16;
        case DataFormat::BC7_UNorm: return kDXGIFormat_BC7_UNorm;
        case DataFormat::BC7_sRGB:  return kDXGIFormat_BC7_UNorm_sRGB;

        // --- 深度模板 ---
        case DataFormat::D16_UNorm:          return kDXGIFormat_D16_UNorm;
        case DataFormat::D24_UNorm_S8_UInt:  return kDXGIFormat_D24_UNorm_S8_UInt;
        case DataFormat::D32_Float:          return kDXGIFormat_D32_Float;
        case DataFormat::D32_Float_S8X24_UInt: return kDXGIFormat_D32_Float_S8X24_UInt;

        // --- ASTC：D3D12 原生无 ASTC（Apple/移动 tile 格式；跨端缺口登记） ---
        case DataFormat::ASTC_4x4_UNorm:
        case DataFormat::ASTC_4x4_sRGB:
        case DataFormat::ASTC_6x6_UNorm:
        case DataFormat::ASTC_6x6_sRGB:
        case DataFormat::ASTC_8x8_UNorm:
        case DataFormat::ASTC_8x8_sRGB:
            return kDXGIFormat_Unknown;

        default:
            return kDXGIFormat_Unknown;
    }
}

DataFormat FromDxgiFormat(u32 dxgiFormat) {
    switch (dxgiFormat) {
        case kDXGIFormat_Unknown: return DataFormat::Unknown;

        // 8 位
        case kDXGIFormat_R8_UNorm: return DataFormat::R8_UNorm;
        case kDXGIFormat_R8_SNorm: return DataFormat::R8_SNorm;
        case kDXGIFormat_R8_UInt:  return DataFormat::R8_UInt;
        case kDXGIFormat_R8_SInt:  return DataFormat::R8_SInt;

        // 16 位
        case kDXGIFormat_R16_UNorm: return DataFormat::R16_UNorm;
        case kDXGIFormat_R16_SNorm: return DataFormat::R16_SNorm;
        case kDXGIFormat_R16_UInt:  return DataFormat::R16_UInt;
        case kDXGIFormat_R16_SInt:  return DataFormat::R16_SInt;
        case kDXGIFormat_R16_Float: return DataFormat::R16_Float;
        case kDXGIFormat_R8G8_UNorm: return DataFormat::RG8_UNorm;
        case kDXGIFormat_R8G8_SNorm: return DataFormat::RG8_SNorm;
        case kDXGIFormat_R8G8_UInt:  return DataFormat::RG8_UInt;
        case kDXGIFormat_R8G8_SInt:  return DataFormat::RG8_SInt;

        // 32 位
        case kDXGIFormat_R32_Float: return DataFormat::R32_Float;
        case kDXGIFormat_R32_UInt:  return DataFormat::R32_UInt;
        case kDXGIFormat_R32_SInt:  return DataFormat::R32_SInt;
        case kDXGIFormat_R16G16_UNorm: return DataFormat::RG16_UNorm;
        case kDXGIFormat_R16G16_SNorm: return DataFormat::RG16_SNorm;
        case kDXGIFormat_R16G16_UInt:  return DataFormat::RG16_UInt;
        case kDXGIFormat_R16G16_SInt:  return DataFormat::RG16_SInt;
        case kDXGIFormat_R16G16_Float: return DataFormat::RG16_Float;
        case kDXGIFormat_B8G8R8A8_UNorm: return DataFormat::BGRA8_UNorm;
        case kDXGIFormat_R8G8B8A8_UNorm: return DataFormat::RGBA8_UNorm;
        case kDXGIFormat_R8G8B8A8_UNorm_sRGB: return DataFormat::RGBA8_sRGB;
        case kDXGIFormat_R8G8B8A8_SNorm: return DataFormat::RGBA8_SNorm;
        case kDXGIFormat_R8G8B8A8_UInt:  return DataFormat::RGBA8_UInt;
        case kDXGIFormat_R8G8B8A8_SInt:  return DataFormat::RGBA8_SInt;

        // 64 位
        case kDXGIFormat_R32G32_Float: return DataFormat::RG32_Float;
        case kDXGIFormat_R32G32_UInt:  return DataFormat::RG32_UInt;
        case kDXGIFormat_R32G32_SInt:  return DataFormat::RG32_SInt;
        case kDXGIFormat_R16G16B16A16_UNorm: return DataFormat::RGBA16_UNorm;
        case kDXGIFormat_R16G16B16A16_SNorm: return DataFormat::RGBA16_SNorm;
        case kDXGIFormat_R16G16B16A16_UInt:  return DataFormat::RGBA16_UInt;
        case kDXGIFormat_R16G16B16A16_SInt:  return DataFormat::RGBA16_SInt;
        case kDXGIFormat_R16G16B16A16_Float: return DataFormat::RGBA16_Float;

        // 96 位
        case kDXGIFormat_R32G32B32_Float: return DataFormat::RGB32_Float;
        case kDXGIFormat_R32G32B32_UInt:  return DataFormat::RGB32_UInt;
        case kDXGIFormat_R32G32B32_SInt:  return DataFormat::RGB32_SInt;

        // 128 位
        case kDXGIFormat_R32G32B32A32_Float: return DataFormat::RGBA32_Float;
        case kDXGIFormat_R32G32B32A32_UInt:  return DataFormat::RGBA32_UInt;
        case kDXGIFormat_R32G32B32A32_SInt:  return DataFormat::RGBA32_SInt;

        // BC
        case kDXGIFormat_BC1_UNorm: return DataFormat::BC1_UNorm;
        case kDXGIFormat_BC1_UNorm_sRGB: return DataFormat::BC1_sRGB;
        case kDXGIFormat_BC2_UNorm: return DataFormat::BC2_UNorm;
        case kDXGIFormat_BC2_UNorm_sRGB: return DataFormat::BC2_sRGB;
        case kDXGIFormat_BC3_UNorm: return DataFormat::BC3_UNorm;
        case kDXGIFormat_BC3_UNorm_sRGB: return DataFormat::BC3_sRGB;
        case kDXGIFormat_BC4_UNorm: return DataFormat::BC4_UNorm;
        case kDXGIFormat_BC4_SNorm: return DataFormat::BC4_SNorm;
        case kDXGIFormat_BC5_UNorm: return DataFormat::BC5_UNorm;
        case kDXGIFormat_BC5_SNorm: return DataFormat::BC5_SNorm;
        case kDXGIFormat_BC6H_UF16: return DataFormat::BC6H_UF16;
        case kDXGIFormat_BC6H_SF16: return DataFormat::BC6H_SF16;
        case kDXGIFormat_BC7_UNorm: return DataFormat::BC7_UNorm;
        case kDXGIFormat_BC7_UNorm_sRGB: return DataFormat::BC7_sRGB;

        // 深度模板
        case kDXGIFormat_D16_UNorm: return DataFormat::D16_UNorm;
        case kDXGIFormat_D24_UNorm_S8_UInt: return DataFormat::D24_UNorm_S8_UInt;
        case kDXGIFormat_D32_Float: return DataFormat::D32_Float;
        case kDXGIFormat_D32_Float_S8X24_UInt: return DataFormat::D32_Float_S8X24_UInt;

        // 其余（含全部 TYPELESS、B8G8R8A8_UNorm_sRGB=91——RHI 无 BGRA sRGB
        // 枚举、视频/打包格式）：显式无映射。
        default:
            return DataFormat::Unknown;
    }
}

u32 GetTypelessDxgiFormat(DataFormat format) {
    switch (format) {
        case DataFormat::R8_UNorm: case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt: case DataFormat::R8_SInt:
            return kDXGIFormat_R8_Typeless;
        case DataFormat::RG8_UNorm: case DataFormat::RG8_SNorm:
        case DataFormat::RG8_UInt: case DataFormat::RG8_SInt:
            return kDXGIFormat_R8G8_Typeless;
        case DataFormat::R16_UNorm: case DataFormat::R16_SNorm:
        case DataFormat::R16_UInt: case DataFormat::R16_SInt:
        case DataFormat::R16_Float:
            return kDXGIFormat_R16_Typeless;
        case DataFormat::RG16_UNorm: case DataFormat::RG16_SNorm:
        case DataFormat::RG16_UInt: case DataFormat::RG16_SInt:
        case DataFormat::RG16_Float:
            return kDXGIFormat_R16G16_Typeless;
        case DataFormat::R32_UNorm: case DataFormat::R32_SNorm:
        case DataFormat::R32_UInt: case DataFormat::R32_SInt:
        case DataFormat::R32_Float: case DataFormat::D32_Float:
            return kDXGIFormat_R32_Typeless;
        case DataFormat::RG32_UNorm: case DataFormat::RG32_SNorm:
        case DataFormat::RG32_UInt: case DataFormat::RG32_SInt:
        case DataFormat::RG32_Float:
            return kDXGIFormat_R32G32_Typeless;
        case DataFormat::RGB32_UNorm: case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_UInt: case DataFormat::RGB32_SInt:
        case DataFormat::RGB32_Float:
            return kDXGIFormat_R32G32B32_Typeless;
        case DataFormat::RGBA16_UNorm: case DataFormat::RGBA16_SNorm:
        case DataFormat::RGBA16_UInt: case DataFormat::RGBA16_SInt:
        case DataFormat::RGBA16_Float:
            return kDXGIFormat_R16G16B16A16_Typeless;
        case DataFormat::RGBA32_UNorm: case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_UInt: case DataFormat::RGBA32_SInt:
        case DataFormat::RGBA32_Float:
            return kDXGIFormat_R32G32B32A32_Typeless;
        case DataFormat::RG8B8A8_UNorm: case DataFormat::RG8B8A8_SNorm:
        case DataFormat::RG8B8A8_UInt: case DataFormat::RG8B8A8_SInt:
        case DataFormat::RGBA8_UNorm: case DataFormat::RGBA8_SNorm:
        case DataFormat::RGBA8_UInt: case DataFormat::RGBA8_SInt:
        case DataFormat::RGBA8_sRGB:
            return kDXGIFormat_R8G8B8A8_Typeless;
        case DataFormat::BGRA8_UNorm:
            return kDXGIFormat_B8G8R8A8_Typeless;
        case DataFormat::BC1_UNorm: case DataFormat::BC1_sRGB:
            return kDXGIFormat_BC1_Typeless;
        case DataFormat::BC2_UNorm: case DataFormat::BC2_sRGB:
            return kDXGIFormat_BC2_Typeless;
        case DataFormat::BC3_UNorm: case DataFormat::BC3_sRGB:
            return kDXGIFormat_BC3_Typeless;
        case DataFormat::BC4_UNorm: case DataFormat::BC4_SNorm:
            return kDXGIFormat_BC4_Typeless;
        case DataFormat::BC5_UNorm: case DataFormat::BC5_SNorm:
            return kDXGIFormat_BC5_Typeless;
        case DataFormat::BC6H_UF16: case DataFormat::BC6H_SF16:
            return kDXGIFormat_BC6H_Typeless;
        case DataFormat::BC7_UNorm: case DataFormat::BC7_sRGB:
            return kDXGIFormat_BC7_Typeless;
        case DataFormat::D24_UNorm_S8_UInt:
            return kDXGIFormat_R24G8_Typeless;
        case DataFormat::D32_Float_S8X24_UInt:
            return kDXGIFormat_R32G8X24_Typeless;
        default:
            return kDXGIFormat_Unknown;
    }
}

u32 ToSrgbDxgiFormat(DataFormat format) {
    switch (format) {
        case DataFormat::RGBA8_UNorm:
        case DataFormat::RG8B8A8_UNorm:
            return kDXGIFormat_R8G8B8A8_UNorm_sRGB;
        case DataFormat::BGRA8_UNorm:
            // RHI 无 BGRA8_sRGB 枚举，但 DXGI 变体存在（91）——视图层可用。
            return kDXGIFormat_B8G8R8A8_UNorm_sRGB;
        case DataFormat::BC1_UNorm: return kDXGIFormat_BC1_UNorm_sRGB;
        case DataFormat::BC2_UNorm: return kDXGIFormat_BC2_UNorm_sRGB;
        case DataFormat::BC3_UNorm: return kDXGIFormat_BC3_UNorm_sRGB;
        case DataFormat::BC7_UNorm: return kDXGIFormat_BC7_UNorm_sRGB;
        default:
            return kDXGIFormat_Unknown;
    }
}

bool IsSrgbDxgiFormat(u32 dxgiFormat) {
    switch (dxgiFormat) {
        case kDXGIFormat_R8G8B8A8_UNorm_sRGB:
        case kDXGIFormat_B8G8R8A8_UNorm_sRGB:
        case kDXGIFormat_BC1_UNorm_sRGB:
        case kDXGIFormat_BC2_UNorm_sRGB:
        case kDXGIFormat_BC3_UNorm_sRGB:
        case kDXGIFormat_BC7_UNorm_sRGB:
            return true;
        default:
            return false;
    }
}

} // namespace primal::graphics::rhi::d3d12
