/**
 * @file VulkanMath.h
 * @brief RHI 类型 ↔ Vulkan 类型的转换助手
 * @details Phase 3 起被 VulkanTexture/VulkanCommandBuffer 等复用,作为不占二进制的 inline 头。
 *          - DataFormat  → VkFormat
 *          - TextureType → VkImageType / VkImageViewType
 *          - TextureUsage → VkImageUsageFlags
 *          - PipelineStage / AccessFlag / ResourceState → Vk 同类
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIResource.h"
#include "../../Core/RHITypes.h"
#include "../../Core/RHICommand.h"

namespace primal::graphics::rhi::vulkan {

// ============================================================================
// DataFormat → VkFormat
// P4c-F1: 与 Metal ToMTLPixelFormat 全量对齐 — 除 Unknown / RGBA32_sRGB
// (双方共同不支持项) 外全部映射到有效 VkFormat。
// 32 位整型的 UNorm/SNorm 变体无 Vulkan/Metal 等价格式,按 Metal 先例
// (R32_UNorm→R32Float) fallback 到 Float。
// BC 压缩格式 macOS MoltenVK 路径不可用(反向缺口),为 Linux/Win 对齐。
// 未支持的格式返回 VK_FORMAT_UNDEFINED 让上层在 vkCreateImage 时报错。
// ============================================================================
inline VkFormat ToVkFormat(DataFormat fmt) {
    switch (fmt) {
        case DataFormat::R8_UNorm:            return VK_FORMAT_R8_UNORM;
        case DataFormat::R8_SNorm:            return VK_FORMAT_R8_SNORM;
        case DataFormat::R8_UInt:             return VK_FORMAT_R8_UINT;
        case DataFormat::R8_SInt:             return VK_FORMAT_R8_SINT;
        case DataFormat::RG8_UNorm:           return VK_FORMAT_R8G8_UNORM;
        case DataFormat::RG8_SNorm:           return VK_FORMAT_R8G8_SNORM;
        case DataFormat::RG8_UInt:            return VK_FORMAT_R8G8_UINT;
        case DataFormat::RG8_SInt:            return VK_FORMAT_R8G8_SINT;
        case DataFormat::R8G8B8_UNorm:        return VK_FORMAT_R8G8B8_UNORM;
        case DataFormat::R8G8B8_SNorm:        return VK_FORMAT_R8G8B8_SNORM;
        case DataFormat::R8G8B8_UInt:         return VK_FORMAT_R8G8B8_UINT;
        case DataFormat::R8G8B8_SInt:         return VK_FORMAT_R8G8B8_SINT;
        case DataFormat::RG8B8A8_UNorm:       return VK_FORMAT_R8G8B8A8_UNORM;
        case DataFormat::RG8B8A8_SNorm:       return VK_FORMAT_R8G8B8A8_SNORM;
        case DataFormat::RG8B8A8_UInt:        return VK_FORMAT_R8G8B8A8_UINT;
        case DataFormat::RG8B8A8_SInt:        return VK_FORMAT_R8G8B8A8_SINT;
        case DataFormat::BGRA8_UNorm:         return VK_FORMAT_B8G8R8A8_UNORM;
        case DataFormat::BGRA8_SNorm:         return VK_FORMAT_B8G8R8A8_SNORM;
        case DataFormat::BGRA8_UInt:          return VK_FORMAT_B8G8R8A8_UINT;
        case DataFormat::BGRA8_SInt:          return VK_FORMAT_B8G8R8A8_SINT;
        case DataFormat::RGBA8_UNorm:         return VK_FORMAT_R8G8B8A8_UNORM;
        case DataFormat::RGBA8_SNorm:         return VK_FORMAT_R8G8B8A8_SNORM;
        case DataFormat::RGBA8_UInt:          return VK_FORMAT_R8G8B8A8_UINT;
        case DataFormat::RGBA8_SInt:          return VK_FORMAT_R8G8B8A8_SINT;
        case DataFormat::RGBA8_sRGB:          return VK_FORMAT_R8G8B8A8_SRGB;
        // --- 16 位整型变体(P4c-F1) ---
        case DataFormat::R16_UNorm:           return VK_FORMAT_R16_UNORM;
        case DataFormat::R16_SNorm:           return VK_FORMAT_R16_SNORM;
        case DataFormat::R16_UInt:            return VK_FORMAT_R16_UINT;
        case DataFormat::R16_SInt:            return VK_FORMAT_R16_SINT;
        case DataFormat::R16_Float:           return VK_FORMAT_R16_SFLOAT;
        case DataFormat::RG16_UNorm:          return VK_FORMAT_R16G16_UNORM;
        case DataFormat::RG16_SNorm:          return VK_FORMAT_R16G16_SNORM;
        case DataFormat::RG16_UInt:           return VK_FORMAT_R16G16_UINT;
        case DataFormat::RG16_SInt:           return VK_FORMAT_R16G16_SINT;
        case DataFormat::RG16_Float:          return VK_FORMAT_R16G16_SFLOAT;
        case DataFormat::RGBA16_UNorm:        return VK_FORMAT_R16G16B16A16_UNORM;
        case DataFormat::RGBA16_SNorm:        return VK_FORMAT_R16G16B16A16_SNORM;
        case DataFormat::RGBA16_UInt:         return VK_FORMAT_R16G16B16A16_UINT;
        case DataFormat::RGBA16_SInt:         return VK_FORMAT_R16G16B16A16_SINT;
        case DataFormat::RGBA16_Float:        return VK_FORMAT_R16G16B16A16_SFLOAT;
        // --- 32 位(P4c-F1;UNorm/SNorm 按 Metal 先例 fallback Float) ---
        case DataFormat::R32_UNorm:           return VK_FORMAT_R32_SFLOAT;
        case DataFormat::R32_SNorm:           return VK_FORMAT_R32_SFLOAT;
        case DataFormat::R32_UInt:            return VK_FORMAT_R32_UINT;
        case DataFormat::R32_SInt:            return VK_FORMAT_R32_SINT;
        case DataFormat::R32_Float:           return VK_FORMAT_R32_SFLOAT;
        case DataFormat::RG32_UNorm:          return VK_FORMAT_R32G32_SFLOAT;
        case DataFormat::RG32_SNorm:          return VK_FORMAT_R32G32_SFLOAT;
        case DataFormat::RG32_UInt:           return VK_FORMAT_R32G32_UINT;
        case DataFormat::RG32_SInt:           return VK_FORMAT_R32G32_SINT;
        case DataFormat::RG32_Float:          return VK_FORMAT_R32G32_SFLOAT;
        // RGB32 是 3 通道 12 字节;部分驱动对 storage/blit 受限,仅作采样/RT。
        case DataFormat::RGB32_UNorm:         return VK_FORMAT_R32G32B32_SFLOAT;
        case DataFormat::RGB32_SNorm:         return VK_FORMAT_R32G32B32_SFLOAT;
        case DataFormat::RGB32_UInt:          return VK_FORMAT_R32G32B32_UINT;
        case DataFormat::RGB32_SInt:          return VK_FORMAT_R32G32B32_SINT;
        case DataFormat::RGB32_Float:         return VK_FORMAT_R32G32B32_SFLOAT;
        case DataFormat::RGBA32_UNorm:        return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DataFormat::RGBA32_SNorm:        return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DataFormat::RGBA32_UInt:         return VK_FORMAT_R32G32B32A32_UINT;
        case DataFormat::RGBA32_SInt:         return VK_FORMAT_R32G32B32A32_SINT;
        case DataFormat::RGBA32_Float:        return VK_FORMAT_R32G32B32A32_SFLOAT;
        // --- 深度模板(P4c-F1 补 D16) ---
        case DataFormat::D16_UNorm:           return VK_FORMAT_D16_UNORM;
        case DataFormat::D32_Float:           return VK_FORMAT_D32_SFLOAT;
        case DataFormat::D24_UNorm_S8_UInt:   return VK_FORMAT_D24_UNORM_S8_UINT;
        case DataFormat::D32_Float_S8X24_UInt:return VK_FORMAT_D32_SFLOAT_S8_UINT;
        // --- BC 压缩(P4c-F1 补齐全系;macOS 不可用,为 Linux/Win 对齐) ---
        case DataFormat::BC1_UNorm:           return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case DataFormat::BC1_sRGB:            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case DataFormat::BC2_UNorm:           return VK_FORMAT_BC2_UNORM_BLOCK;
        case DataFormat::BC2_sRGB:            return VK_FORMAT_BC2_SRGB_BLOCK;
        case DataFormat::BC3_UNorm:           return VK_FORMAT_BC3_UNORM_BLOCK;
        case DataFormat::BC3_sRGB:            return VK_FORMAT_BC3_SRGB_BLOCK;
        case DataFormat::BC4_UNorm:           return VK_FORMAT_BC4_UNORM_BLOCK;
        case DataFormat::BC4_SNorm:           return VK_FORMAT_BC4_SNORM_BLOCK;
        case DataFormat::BC5_UNorm:           return VK_FORMAT_BC5_UNORM_BLOCK;
        case DataFormat::BC5_SNorm:           return VK_FORMAT_BC5_SNORM_BLOCK;
        case DataFormat::BC6H_UF16:           return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case DataFormat::BC6H_SF16:           return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case DataFormat::BC7_UNorm:           return VK_FORMAT_BC7_UNORM_BLOCK;
        case DataFormat::BC7_sRGB:            return VK_FORMAT_BC7_SRGB_BLOCK;
        default:                              return VK_FORMAT_UNDEFINED;
    }
}

// ============================================================================
// DataFormat 每 texel 字节数(未压缩格式;BC 压缩按 4x4 block 尺寸返回 block 字节数)
// P4c-F1/F3: updateDataImpl 计算 bytesPerRow/bytesPerImage、EstimateTextureSizeBytes
// 复用。返回 0 = 未知格式(调用方需显式处理,不静默)。
// ============================================================================
inline u64 BytesPerTexel(DataFormat fmt) {
    switch (fmt) {
        case DataFormat::R8_UNorm: case DataFormat::R8_SNorm:
        case DataFormat::R8_UInt:  case DataFormat::R8_SInt:
            return 1;
        case DataFormat::R16_UNorm: case DataFormat::R16_SNorm:
        case DataFormat::R16_UInt:  case DataFormat::R16_SInt:
        case DataFormat::R16_Float:
        case DataFormat::RG8_UNorm: case DataFormat::RG8_SNorm:
        case DataFormat::RG8_UInt:  case DataFormat::RG8_SInt:
            return 2;
        case DataFormat::R8G8B8_UNorm: case DataFormat::R8G8B8_SNorm:
        case DataFormat::R8G8B8_UInt:  case DataFormat::R8G8B8_SInt:
            return 3;
        case DataFormat::R32_UNorm: case DataFormat::R32_SNorm:
        case DataFormat::R32_UInt:  case DataFormat::R32_SInt:
        case DataFormat::R32_Float:
        case DataFormat::RG16_UNorm: case DataFormat::RG16_SNorm:
        case DataFormat::RG16_UInt:  case DataFormat::RG16_SInt:
        case DataFormat::RG16_Float:
        case DataFormat::RG8B8A8_UNorm: case DataFormat::RG8B8A8_SNorm:
        case DataFormat::RG8B8A8_UInt:  case DataFormat::RG8B8A8_SInt:
        case DataFormat::BGRA8_UNorm: case DataFormat::BGRA8_SNorm:
        case DataFormat::BGRA8_UInt:  case DataFormat::BGRA8_SInt:
        case DataFormat::RGBA8_UNorm: case DataFormat::RGBA8_SNorm:
        case DataFormat::RGBA8_UInt:  case DataFormat::RGBA8_SInt:
        case DataFormat::RGBA8_sRGB:
        case DataFormat::D16_UNorm:
        case DataFormat::D32_Float:
        case DataFormat::D24_UNorm_S8_UInt:
        case DataFormat::D32_Float_S8X24_UInt:
            return 4;
        case DataFormat::RG32_UNorm: case DataFormat::RG32_SNorm:
        case DataFormat::RG32_UInt:  case DataFormat::RG32_SInt:
        case DataFormat::RG32_Float:
        case DataFormat::RGB32_UNorm: case DataFormat::RGB32_SNorm:
        case DataFormat::RGB32_UInt:  case DataFormat::RGB32_SInt:
        case DataFormat::RGB32_Float:
        case DataFormat::RGBA16_UNorm: case DataFormat::RGBA16_SNorm:
        case DataFormat::RGBA16_UInt:  case DataFormat::RGBA16_SInt:
        case DataFormat::RGBA16_Float:
            return 8;
        case DataFormat::RGBA32_UNorm: case DataFormat::RGBA32_SNorm:
        case DataFormat::RGBA32_UInt:  case DataFormat::RGBA32_SInt:
        case DataFormat::RGBA32_Float:
            return 16;
        // BC 压缩:block 尺寸(8B = BC1/BC4,16B = BC2/BC3/BC5/BC6H/BC7)。
        // 换算成 per-texel 需除以 16(block 4x4),由调用方处理。
        case DataFormat::BC1_UNorm: case DataFormat::BC1_sRGB:
        case DataFormat::BC4_UNorm: case DataFormat::BC4_SNorm:
            return 8;
        case DataFormat::BC2_UNorm: case DataFormat::BC2_sRGB:
        case DataFormat::BC3_UNorm: case DataFormat::BC3_sRGB:
        case DataFormat::BC5_UNorm: case DataFormat::BC5_SNorm:
        case DataFormat::BC6H_UF16: case DataFormat::BC6H_SF16:
        case DataFormat::BC7_UNorm: case DataFormat::BC7_sRGB:
            return 16;
        default:
            return 0;
    }
}

/// BC 压缩格式判断(影响 BytesPerTexel 的语义:updateData 按 block 计)
inline bool IsBlockCompressedFormat(DataFormat fmt) {
    switch (fmt) {
        case DataFormat::BC1_UNorm: case DataFormat::BC1_sRGB:
        case DataFormat::BC2_UNorm: case DataFormat::BC2_sRGB:
        case DataFormat::BC3_UNorm: case DataFormat::BC3_sRGB:
        case DataFormat::BC4_UNorm: case DataFormat::BC4_SNorm:
        case DataFormat::BC5_UNorm: case DataFormat::BC5_SNorm:
        case DataFormat::BC6H_UF16: case DataFormat::BC6H_SF16:
        case DataFormat::BC7_UNorm: case DataFormat::BC7_sRGB:
            return true;
        default:
            return false;
    }
}

/// 深度(含深度+模板)格式判断 — VkImageView aspectMask / barrier aspect 计算
/// 统一入口。P4c-F1 起含 D16_UNORM。
inline bool IsDepthVkFormat(VkFormat fmt) {
    return fmt == VK_FORMAT_D16_UNORM ||
           fmt == VK_FORMAT_D32_SFLOAT ||
           fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
           fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// ============================================================================
// TextureType → VkImageType / VkImageViewType
// ============================================================================
inline VkImageType ToVkImageType(TextureType t) {
    switch (t) {
        case TextureType::Texture1D:
        case TextureType::Texture1DArray:        return VK_IMAGE_TYPE_1D;
        case TextureType::Texture2D:
        case TextureType::Texture2DArray:
        case TextureType::TextureCube:
        case TextureType::TextureCubeArray:      return VK_IMAGE_TYPE_2D;
        case TextureType::Texture3D:             return VK_IMAGE_TYPE_3D;
        default:                                 return VK_IMAGE_TYPE_2D;
    }
}

inline VkImageViewType ToVkImageViewType(TextureType t, u32 arraySize, u32 mipCount) {
    (void)mipCount;
    switch (t) {
        case TextureType::Texture1D:             return VK_IMAGE_VIEW_TYPE_1D;
        case TextureType::Texture1DArray:        return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case TextureType::Texture2D:             return arraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case TextureType::Texture2DArray:        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureType::Texture3D:             return VK_IMAGE_VIEW_TYPE_3D;
        case TextureType::TextureCube:           return VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureType::TextureCubeArray:      return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        default:                                 return VK_IMAGE_VIEW_TYPE_2D;
    }
}

// ============================================================================
// TextureUsage → VkImageUsageFlags
// 自动追加 TRANSFER_SRC/DST 让 Copy/Blit/Mipmap 始终可用(Metal 等价于默认 mtlTexture usage)
// 注意:RHITypes.h 已声明 TextureUsage operator&,但没有 HasUsage(TextureUsage,...) 助手,
// 所以这里用 static_cast<u32> 直接做 bitwise 与。
// ============================================================================
inline VkImageUsageFlags ToVkImageUsageFlags(TextureUsage usage) {
    const u32 u = static_cast<u32>(usage);
    VkImageUsageFlags f = 0;
    if (u & static_cast<u32>(TextureUsage::ShaderResource))    f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u & static_cast<u32>(TextureUsage::RenderTarget))      f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u & static_cast<u32>(TextureUsage::DepthStencil))      f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (u & static_cast<u32>(TextureUsage::UnorderedAccess))   f |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (u & static_cast<u32>(TextureUsage::CopySource))        f |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (u & static_cast<u32>(TextureUsage::CopyDest))          f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // RenderTarget / ShaderResource 都需要 TRANSFER_DST 才能 clear / upload
    if ((u & static_cast<u32>(TextureUsage::RenderTarget)) ||
        (u & static_cast<u32>(TextureUsage::ShaderResource))) {
        f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return f;
}

// ============================================================================
// PipelineStage → VkPipelineStageFlags
// ============================================================================
inline VkPipelineStageFlags ToVkPipelineStageFlags(PipelineStage s) {
    VkPipelineStageFlags f = 0;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::TopOfPipe))            f |= VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::DrawIndirect))         f |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::VertexInput))          f |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::VertexShader))         f |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::FragmentShader))       f |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::EarlyFragmentTests))   f |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::LateFragmentTests))    f |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::ColorAttachmentOutput))f |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::ComputeShader))        f |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::Transfer))             f |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::BottomOfPipe))         f |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::Host))                 f |= VK_PIPELINE_STAGE_HOST_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::AllGraphics))          f |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    if (static_cast<u32>(s) & static_cast<u32>(PipelineStage::AllCommands))          f |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return f;
}

// ============================================================================
// AccessFlag → VkAccessFlags
// ============================================================================
inline VkAccessFlags ToVkAccessFlags(AccessFlag a) {
    VkAccessFlags f = 0;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::IndirectCommandRead))       f |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::IndexRead))                 f |= VK_ACCESS_INDEX_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::VertexAttributeRead))       f |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::UniformRead))               f |= VK_ACCESS_UNIFORM_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::InputAttachmentRead))       f |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::ShaderRead))                f |= VK_ACCESS_SHADER_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::ShaderWrite))               f |= VK_ACCESS_SHADER_WRITE_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::ColorAttachmentRead))       f |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::ColorAttachmentWrite))      f |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::DepthStencilAttachmentRead))f |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::DepthStencilAttachmentWrite))f|= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::TransferRead))              f |= VK_ACCESS_TRANSFER_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::TransferWrite))             f |= VK_ACCESS_TRANSFER_WRITE_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::HostRead))                  f |= VK_ACCESS_HOST_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::HostWrite))                 f |= VK_ACCESS_HOST_WRITE_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::MemoryRead))                f |= VK_ACCESS_MEMORY_READ_BIT;
    if (static_cast<u32>(a) & static_cast<u32>(AccessFlag::MemoryWrite))               f |= VK_ACCESS_MEMORY_WRITE_BIT;
    return f;
}

// ============================================================================
// ResourceState → (VkImageLayout, VkAccessFlags, VkPipelineStageFlags)
// 用于 InsertBarrier / 在 record 期间隐式 layout transition
// ============================================================================
struct VkLayoutAccess {
    VkImageLayout         layout;
    VkAccessFlags         access;
    VkPipelineStageFlags  stage;
};

inline VkLayoutAccess ResourceStateToVkLayout(ResourceState s) {
    switch (s) {
        case ResourceState::Unknown:
        case ResourceState::Created:
        case ResourceState::Allocated:
            // 未定义 — Vulkan 用 UNDEFINED 起始,首次 transition 必须在 UNDEFINED→X
            return { VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };

        case ResourceState::Ready:
            // Generic ready = SAMPLED if texture, but caller may transition to something specific.
            // Default to SHADER_READ_ONLY_OPTIMAL with frag/vertex shader read.
            return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

        case ResourceState::InUse:
            return { VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };

        case ResourceState::General:
            return { VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };

        case ResourceState::RenderTarget:
            return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

        case ResourceState::DepthStencil:
            return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };

        case ResourceState::DepthStencilReadOnly:
            return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };

        case ResourceState::ShaderResource:
            return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

        case ResourceState::UnorderedAccess:
            return { VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };

        case ResourceState::CopySource:
            return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT };

        case ResourceState::CopyDest:
            return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT };

        case ResourceState::Present:
            return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };

        case ResourceState::ResolveSource:
            return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT };

        case ResourceState::ResolveDest:
            return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT };

        default:
            return { VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
    }
}

// ============================================================================
// FilterMode → VkFilter (用于 BlitTexture)
// Anisotropic 在当前 RHI 不暴露各向异性参数,降级到 Linear
// ============================================================================
inline VkFilter ToVkFilter(FilterMode f) {
    // FilterMode::Point == FilterMode::Nearest == 1 (alias), so single case handles both.
    switch (f) {
        case FilterMode::Point:     return VK_FILTER_NEAREST;
        case FilterMode::Linear:
        case FilterMode::Anisotropic:
        default:                    return VK_FILTER_LINEAR;
    }
}

// ============================================================================
// PipelineStage → CommandQueueType (用于选择 VkQueue)
// Compute/Transfer 在 Phase 1 都复用 graphics queue;这里只供代码可读性。
// ============================================================================
inline CommandQueueType FromVkQueueFamily(u32 /*familyIndex*/) {
    return CommandQueueType::Graphics;  // Phase 1-3 全 graphics queue
}

} // namespace primal::graphics::rhi::vulkan

#endif // ENABLE_VULKAN
