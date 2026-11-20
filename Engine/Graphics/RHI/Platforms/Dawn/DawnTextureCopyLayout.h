#pragma once
#include "DawnCommon.h"
#include "Engine/Common/CoreLogStream.h"
#include <algorithm>
#include <limits>

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
namespace primal::graphics::rhi {
struct DawnCopyBlock { u32 width, height, bytes; };
inline DawnCopyBlock DawnTextureCopyBlock(DataFormat format) {
    switch (format) {
    case DataFormat::R8_UNorm: case DataFormat::R8_UInt: return {1, 1, 1};
    case DataFormat::R16_UNorm: case DataFormat::R16_UInt: case DataFormat::R16_Float:
    case DataFormat::D16_UNorm: return {1, 1, 2};
    case DataFormat::RGBA8_UNorm: case DataFormat::RGBA8_sRGB: case DataFormat::BGRA8_UNorm:
    case DataFormat::RG16_Float: case DataFormat::R32_Float: case DataFormat::R32_UInt:
    case DataFormat::R32_SInt: case DataFormat::D32_Float: return {1, 1, 4};
    case DataFormat::RGBA16_Float: case DataFormat::RG32_Float: return {1, 1, 8};
    case DataFormat::RGBA32_Float: return {1, 1, 16};
    case DataFormat::BC1_UNorm: case DataFormat::BC1_sRGB: case DataFormat::BC4_UNorm:
        return {4, 4, 8};
    case DataFormat::BC2_UNorm: case DataFormat::BC3_UNorm: case DataFormat::BC3_sRGB:
    case DataFormat::BC5_UNorm: case DataFormat::BC7_UNorm: case DataFormat::BC7_sRGB:
    case DataFormat::ASTC_4x4_UNorm: case DataFormat::ASTC_4x4_sRGB: return {4, 4, 16};
    case DataFormat::ASTC_6x6_UNorm: case DataFormat::ASTC_6x6_sRGB: return {6, 6, 16};
    case DataFormat::ASTC_8x8_UNorm: case DataFormat::ASTC_8x8_sRGB: return {8, 8, 16};
    default: return {0, 0, 0}; // Combined depth/stencil requires an explicit plane contract.
    }
}

// RHI row length is in texels (zero means tight); WebGPU uses bytes and
// requires 256-byte row alignment for multi-row encoder copies. Split tight
// unaligned rows rather than silently padding beyond the caller's allocation.
template<class Encode>
bool ForEachDawnTextureCopy(const TextureDesc& texture, const BufferTextureCopyRegion& r, Encode&& encode) {
    const auto block = DawnTextureCopyBlock(texture.format);
    const u32 rowTexels = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
    const u32 imageRows = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
    if (!block.bytes || !r.imageExtent.width || !r.imageExtent.height ||
        rowTexels < r.imageExtent.width || imageRows < r.imageExtent.height) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[DawnCopy] unsupported format or invalid buffer layout" << std::endl;
        return false;
    }
    const u64 rowBytes64 = ((u64(rowTexels) + block.width - 1) / block.width) * block.bytes;
    if (rowBytes64 > std::numeric_limits<u32>::max()) return false;
    const u32 rowBytes = static_cast<u32>(rowBytes64);
    const u32 rowsPerImage = static_cast<u32>((u64(imageRows) + block.height - 1) / block.height);
    const u32 copyRows = static_cast<u32>((u64(r.imageExtent.height) + block.height - 1) / block.height);
    const bool volume = texture.type == TextureType::Texture3D;
    const u32 layers = volume ? r.imageExtent.depth : r.imageSubresource.layerCount;
    WGPUOrigin3D origin{static_cast<u32>(r.imageOffset.x), static_cast<u32>(r.imageOffset.y),
                        static_cast<u32>(r.imageOffset.z) + (volume ? 0u : r.imageSubresource.baseArrayLayer)};
    WGPUExtent3D extent{r.imageExtent.width, r.imageExtent.height, layers};
    WGPUTexelCopyBufferLayout layout{};
    layout.offset = r.bufferOffset;
    layout.bytesPerRow = rowBytes;
    layout.rowsPerImage = rowsPerImage;
    if (rowBytes % 256 == 0) {
        encode(layout, origin, extent);
    } else {
        layout.bytesPerRow = WGPU_COPY_STRIDE_UNDEFINED;
        layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;
        extent.depthOrArrayLayers = 1;
        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 row = 0; row < copyRows; ++row) {
                auto rowOrigin = origin;
                rowOrigin.y += row * block.height;
                rowOrigin.z += layer;
                extent.height = std::min(block.height, r.imageExtent.height - row * block.height);
                layout.offset = r.bufferOffset + (u64(layer) * rowsPerImage + row) * rowBytes;
                encode(layout, rowOrigin, extent);
            }
        }
    }
    return true;
}
} // namespace primal::graphics::rhi
#endif
