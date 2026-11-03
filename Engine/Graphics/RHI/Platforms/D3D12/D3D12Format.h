/**
 * @file D3D12Format.h
 * @brief RHI DataFormat <-> DXGI_FORMAT 双向映射（平台中立，无 Windows 头）
 * @details D3D12 RHI 现代化 M0（F0-6）。
 *
 *          设计约定（与 VulkanMath.h ToVkFormat 的既有先例对齐，保证跨端
 *          行为对等——同一种 RHI 格式在后端间不产生一个能渲染一个报错的分歧）：
 *            1. 32 位整型家族的 UNorm/SNorm 变体（R32/RG32/RGB32/RGBA32）
 *               DXGI 不存在，按 Vulkan 先例 fallback 到对应 FLOAT 格式。
 *            2. 无 DXGI 等价物的格式返回 kDXGIFormat_Unknown（调用方按
 *               "显式不支持"处理，不静默换格式）：
 *               - R8G8B8 24-bit 家族（DXGI 无 24-bit 布局）
 *               - BGRA8 SNorm/UInt/SInt（DXGI B8G8R8A8 只有 UNORM/SRGB/TYPELESS）
 *               - RGBA32_sRGB（DXGI 无此格式；Vulkan 同样返回 UNDEFINED）
 *               - ASTC 家族（D3D12 原生无 ASTC；跨端缺口登记见阶段文档）
 *            3. sRGB 家族与 TYPELESS 家族提供独立查询：ToSrgbDxgiFormat /
 *               GetTypelessDxgiFormat，供纹理视图/格式协商使用。
 *
 *          macOS 可编译可单测（TestD3D12Format）；Windows 集成时值正确性
 *          由 PRIMAL_D3D12_ABI_CHECKS（D3D12Abi.h）锁死。
 */

#pragma once

#include "../../Core/RHITypes.h"
#include "D3D12Abi.h"

namespace primal::graphics::rhi::d3d12 {

/// RHI DataFormat -> DXGI_FORMAT。无等价物返回 kDXGIFormat_Unknown（0）。
u32 ToDxgiFormat(DataFormat format);

/// DXGI_FORMAT -> RHI DataFormat。反向取规范代表元（R8G8B8A8_UNorm 同时是
/// RG8B8A8_UNorm 与 RGBA8_UNorm 的目标，反向返回 RGBA8_UNorm）；
/// 未映射值（含全部 TYPELESS、视频格式）返回 DataFormat::Unknown。
DataFormat FromDxgiFormat(u32 dxgiFormat);

/// typed 格式 -> 同家族 TYPELESS 父格式（视图/重解释用）。
/// 无 typeless 父格式或格式未知返回 kDXGIFormat_Unknown。
u32 GetTypelessDxgiFormat(DataFormat format);

/// UNorm 格式 -> 同家族 sRGB 变体（创建采样视图时切 gamma 空间）。
/// 无 sRGB 变体返回 kDXGIFormat_Unknown。
u32 ToSrgbDxgiFormat(DataFormat format);

/// DXGI 值是否属于 sRGB 变体（含 BC 压缩 sRGB）。
bool IsSrgbDxgiFormat(u32 dxgiFormat);

/// RHI DataFormat 是否为 sRGB 变体。
inline bool IsSrgbDataFormat(DataFormat format) {
    switch (format) {
        case DataFormat::RGBA8_sRGB:
        case DataFormat::BC1_sRGB: case DataFormat::BC2_sRGB:
        case DataFormat::BC3_sRGB: case DataFormat::BC7_sRGB:
        case DataFormat::ASTC_4x4_sRGB: case DataFormat::ASTC_6x6_sRGB:
        case DataFormat::ASTC_8x8_sRGB:
            return true;
        default:
            return false;
    }
}

} // namespace primal::graphics::rhi::d3d12
