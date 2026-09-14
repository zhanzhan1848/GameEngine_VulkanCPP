#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Utilities/Math.h"
#include <cstring>

namespace primal::graphics::pcg {

// Manages async GPU→CPU readback of GlobalSDF cascade 0 for PCG sampling.
// The PCGReferenceField queries this manager instead of using an analytic SDF.
//
// Usage:
//   PCGSDFReadbackManager mgr;
//   mgr.Initialize(device, 128);  // cascade resolution
//   // Each frame after GlobalSDF renders:
//   mgr.IssueReadback(cmd, cascade_texture);
//   // Next frame or after sync:
//   if (mgr.IsDataReady()) {
//     field.SetReadbackManager(&mgr);
//   }
//
// Phase 2 limitation: only cascade 0 (highest detail), 1-frame latency.
class PCGSDFReadbackManager {
public:
    bool Initialize(rhi::RHIDeviceBase* device, u32 cascade_resolution = 128) {
        device_ = device;
        resolution_ = cascade_resolution;

        // GlobalSDF cascade 纹理是 R32_Float（GlobalSDF.cpp 创建处），4 字节/体素。
        // 此前按 R16_Float（2 字节）分配——缓冲只有一半，CopyTextureToBuffer 在 Metal
        // 校验层下断言、非校验下静默破坏后续编码器（2026-09-11 物理冒烟测试实证）。
        u64 buf_size = static_cast<u64>(cascade_resolution) * cascade_resolution * cascade_resolution * 4;
        rhi::BufferDesc desc{};
        desc.size = buf_size;
        desc.type = rhi::BufferType::Raw;
        desc.usage = rhi::GPUMemoryUsage::Readback;
        desc.memoryUsage = rhi::GPUMemoryUsage::Readback;
        readback_buffer_ = device_->CreateBuffer(desc);

        data_.resize(static_cast<u64>(cascade_resolution) * cascade_resolution * cascade_resolution, 0.0f);
        return readback_buffer_ != rhi::handles::INVALID_RESOURCE;
    }

    void Shutdown() {
        if (device_ && readback_buffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(readback_buffer_);
            readback_buffer_ = rhi::handles::INVALID_RESOURCE;
        }
    }

    // Issue GPU copy from cascade texture to readback buffer.
    // Call after GlobalSDF finishes rendering, on the same command buffer or a separate one.
    void IssueReadback(rhi::RHICommandBuffer* cmd, rhi::ResourceHandle cascade_texture) {
        if (readback_buffer_ == rhi::handles::INVALID_RESOURCE) return;

        rhi::BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;   // tightly packed
        region.bufferImageHeight = 0; // tightly packed
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {resolution_, resolution_, resolution_};

        cmd->CopyTextureToBuffer(cascade_texture, readback_buffer_, &region, 1);
        issued_ = true;
    }

    // Map the readback buffer and copy data to CPU array.
    // Call at least 1 frame after IssueReadback, or after GPU sync.
    bool ReadbackData() {
        if (!issued_ || readback_buffer_ == rhi::handles::INVALID_RESOURCE) return false;

        u64 data_size = static_cast<u64>(resolution_) * resolution_ * resolution_ * sizeof(f32);
        void* mapped = device_->MapBuffer(readback_buffer_, 0, data_size);
        if (!mapped) return false;

        memcpy(data_.data(), mapped, data_size);
        device_->UnmapBuffer(readback_buffer_);
        data_ready_ = true;
        return true;
    }

    bool IsDataReady() const { return data_ready_; }

    // Trilinear sample of the SDF grid at a world-space position.
    // origin, voxel_size define the grid's world mapping.
    f32 SampleSDF(math::v3 world_pos, math::v3 origin, f32 voxel_size) const {
        if (!data_ready_ || data_.empty()) return 0.0f;

        // World → grid coordinates [0, resolution)
        f32 gx = (world_pos.x - origin.x) / voxel_size;
        f32 gy = (world_pos.y - origin.y) / voxel_size;
        f32 gz = (world_pos.z - origin.z) / voxel_size;

        // Clamp to valid range for interpolation
        f32 maxCoord = static_cast<f32>(resolution_ - 2);
        gx = std::clamp(gx, 0.0f, maxCoord);
        gy = std::clamp(gy, 0.0f, maxCoord);
        gz = std::clamp(gz, 0.0f, maxCoord);

        // Integer and fractional parts
        u32 x0 = static_cast<u32>(gx), y0 = static_cast<u32>(gy), z0 = static_cast<u32>(gz);
        u32 x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
        f32 fx = gx - x0, fy = gy - y0, fz = gz - z0;

        // 8-tap trilinear interpolation
        f32 c000 = ReadTexel(x0, y0, z0);
        f32 c100 = ReadTexel(x1, y0, z0);
        f32 c010 = ReadTexel(x0, y1, z0);
        f32 c110 = ReadTexel(x1, y1, z0);
        f32 c001 = ReadTexel(x0, y0, z1);
        f32 c101 = ReadTexel(x1, y0, z1);
        f32 c011 = ReadTexel(x0, y1, z1);
        f32 c111 = ReadTexel(x1, y1, z1);

        f32 x00 = c000 + fx * (c100 - c000);
        f32 x10 = c010 + fx * (c110 - c010);
        f32 x01 = c001 + fx * (c101 - c001);
        f32 x11 = c011 + fx * (c111 - c011);

        f32 y0v = x00 + fy * (x10 - x00);
        f32 y1v = x01 + fy * (x11 - x01);

        return y0v + fz * (y1v - y0v);
    }

    u32 GetResolution() const { return resolution_; }

    // IEEE 754 half → float conversion (public for testing)
    static f32 HalfToFloat(u16 h) {
        u32 sign = (h >> 15) & 0x1;
        u32 exponent = (h >> 10) & 0x1F;
        u32 mantissa = h & 0x3FF;

        f32 result;
        if (exponent == 0) {
            if (mantissa == 0) {
                result = 0.0f;
            } else {
                // Denormalized
                f32 val = static_cast<f32>(mantissa) / 1024.0f;
                result = val * 6.103515625e-5f; // 2^-14
            }
        } else if (exponent == 31) {
            result = (mantissa == 0) ? 1e30f : 1e30f; // inf or nan → large value
        } else {
            f32 val = 1.0f + static_cast<f32>(mantissa) / 1024.0f;
            int exp = static_cast<int>(exponent) - 15;
            result = val * powf(2.0f, static_cast<f32>(exp));
        }

        return sign ? -result : result;
    }

private:
    f32 ReadTexel(u32 x, u32 y, u32 z) const {
        u32 idx = x + y * resolution_ + z * resolution_ * resolution_;
        if (idx >= data_.size()) return 0.0f;
        return data_[idx];   // R32_Float 体素原值
    }

    rhi::RHIDeviceBase* device_{nullptr};
    rhi::ResourceHandle readback_buffer_{rhi::handles::INVALID_RESOURCE};
    std::vector<f32> data_;
    u32 resolution_{128};
    bool issued_{false};
    bool data_ready_{false};
};

} // namespace primal::graphics::pcg
