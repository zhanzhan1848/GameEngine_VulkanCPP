#include "OfflineSDFMerger.h"
#include "CommonHeaders.h"
// P4c: Vulkan 上传路径仅在 ENABLE_VULKAN 下编译(修 OFF 构建未守卫问题)
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#define OFFLINE_SDF_VULKAN_PATH 1
#else
#define OFFLINE_SDF_VULKAN_PATH 0
#endif
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace primal::graphics::nanite {

namespace {
// Read a u32 from a byte buffer at the given offset (little-endian).
u32 ReadU32(const u8* data, size_t offset) {
    u32 v;
    std::memcpy(&v, data + offset, 4);
    return v;
}
f32 ReadF32(const u8* data, size_t offset) {
    f32 v;
    std::memcpy(&v, data + offset, 4);
    return v;
}
}

OfflineSDFMerger::~OfflineSDFMerger() {
    if (device_ && global_sdf_texture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(global_sdf_texture_);
    }
}

u32 OfflineSDFMerger::LoadFromPipelineModel(const char* filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[OfflineSDFMerger] Failed to open: " << filePath << std::endl;
        return 0;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<u8> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "[OfflineSDFMerger] Failed to read: " << filePath << std::endl;
        return 0;
    }

    const u8* data = buffer.data();
    size_t off = 0;

    // Prefix u32(0)
    u32 prefix = ReadU32(data, off); off += 4;
    if (prefix != 0) {
        // No prefix — rewind (some pipeline models don't have it).
        off = 0;
    }

    // num_materials
    u32 numMat = ReadU32(data, off); off += 4;
    if (numMat > 10000) {
        std::cerr << "[OfflineSDFMerger] Implausible material count: " << numMat << std::endl;
        return 0;
    }
    // Skip materials (3 fields each: name, diffuse, normal — all length-prefixed strings)
    for (u32 mi = 0; mi < numMat; ++mi) {
        for (u32 fi = 0; fi < 3; ++fi) {
            u32 slen = ReadU32(data, off); off += 4;
            if (slen > 10000) { std::cerr << "[OfflineSDFMerger] Bad string len" << std::endl; return 0; }
            off += slen;
        }
    }

    // num_lod_groups
    u32 numLods = ReadU32(data, off); off += 4;
    if (numLods == 0 || numLods > 100) return 0;

    // First LOD group: name + mesh_count
    u32 lodNameLen = ReadU32(data, off); off += 4 + lodNameLen;
    u32 meshCount = ReadU32(data, off); off += 4;
    if (meshCount > 100000) return 0;

    u32 sdfFound = 0;

    for (u32 i = 0; i < meshCount; ++i) {
        if (off + 32 > (size_t)size) {
            std::cerr << "[OfflineSDFMerger] Stop at mesh " << i << ": off=" << off << " size=" << size << std::endl;
            break;
        }

        // Mesh header (Compact format)
        u32 nameLen = ReadU32(data, off); off += 4;
        if (nameLen > 200) {
            std::cerr << "[OfflineSDFMerger] Bad nameLen=" << nameLen << " at mesh " << i << " off=" << off-4 << std::endl;
            break;
        }
        off += nameLen;

        u32 lodId    = ReadU32(data, off); off += 4;
        int32_t  matIdx  = (int32_t)ReadU32(data, off); off += 4;
        u32 elemSize = ReadU32(data, off); off += 4;
        u32 elemType = ReadU32(data, off); off += 4;
        u32 vertCount= ReadU32(data, off); off += 4;
        u32 idxSize  = ReadU32(data, off); off += 4;
        u32 idxCount = ReadU32(data, off); off += 4;
        off += 4; // lodThreshold (f32)

        // Skip position + element + index buffers (no padding in pipeline format)
        off += 12 * vertCount;       // positions
        off += elemSize * vertCount; // elements
        off += idxSize * idxCount;   // indices

        // Skip MSHL section
        if (off + 8 <= (size_t)size) {
            u32 magic = ReadU32(data, off);
            if (magic == 0x4C48534D) { // 'MSHL'
                off += 4;
                u32 mlCount = ReadU32(data, off); off += 4;
                off += mlCount * 60; // meshlet structs (60 bytes each)
                u32 mlvCount = ReadU32(data, off); off += 4 + mlvCount * 4;
                u32 mltCount = ReadU32(data, off); off += 4 + mltCount;
            } else {
                // No MSHL — this shouldn't happen for pipeline models.
                // Skip to next: we can't continue safely.
                break;
            }
        }

        // Check for SDF section (all meshes may have it — LOD1+ too).
        // We skip the SDF data for non-LOD0 meshes but must consume the bytes
        // so the offset points to the next mesh header.
        if (off + 8 <= (size_t)size) {
            u32 sdfMagic = ReadU32(data, off);
            if (sdfMagic == 0x20464453) { // 'SDF '
                off += 4;
                // Read header (needed to compute total SDF size for skipping).
                u32 sres[3];
                sres[0] = ReadU32(data, off); off += 4;
                sres[1] = ReadU32(data, off); off += 4;
                sres[2] = ReadU32(data, off); off += 4;
                f32 sbmin[3], sbmax[3];
                for (int j = 0; j < 3; ++j) { sbmin[j] = ReadF32(data, off); off += 4; }
                for (int j = 0; j < 3; ++j) { sbmax[j] = ReadF32(data, off); off += 4; }

                f32 sdim = std::max({sbmax[0]-sbmin[0], sbmax[1]-sbmin[1], sbmax[2]-sbmin[2]});
                if (sdim < 1e-6f) sdim = 1.0f;

                // Only LOD0 meshes store SDF for visualization.
                if (lodId == 0xFFFFFFFF) {
                    MeshSDF sdf{};
                    sdf.resolution[0] = sres[0]; sdf.resolution[1] = sres[1]; sdf.resolution[2] = sres[2];
                    for (int j = 0; j < 3; ++j) { sdf.bounds_min[j] = sbmin[j]; sdf.bounds_max[j] = sbmax[j]; }
                    sdf.max_dim = sdim;

                    u32 dataCount = ReadU32(data, off); off += 4;
                    if (dataCount > 0 && dataCount < 100000000) {
                        sdf.data.resize(dataCount);
                        std::memcpy(sdf.data.data(), data + off, dataCount * sizeof(uint16_t));
                        off += dataCount * 2;
                    }
                    u32 voxCount = ReadU32(data, off); off += 4;
                    if (voxCount > 0 && voxCount < 100000000) {
                        sdf.voxels.resize(voxCount);
                        std::memcpy(sdf.voxels.data(), data + off, voxCount);
                        off += voxCount;
                    }
                    u32 vfCount = ReadU32(data, off); off += 4;
                    if (vfCount < 100000000) off += vfCount * 2;

                    if (!sdf.data.empty()) {
                        mesh_sdfs_.push_back(std::move(sdf));
                        sdfFound++;
                    }
                } else {
                    // Non-LOD0: skip SDF data without storing.
                    u32 dataCount = ReadU32(data, off); off += 4 + dataCount * 2;
                    u32 voxCount = ReadU32(data, off); off += 4 + voxCount;
                    u32 vfCount = ReadU32(data, off); off += 4;
                    if (vfCount < 100000000) off += vfCount * 2;
                }
            }
        }
        continue;
    } // end mesh loop

    // Compute global bounds
    if (mesh_sdfs_.empty()) {
        std::cerr << "[OfflineSDFMerger] No SDF data found in " << filePath << std::endl;
        return 0;
    }

    // Compute global bounds — filter out extreme outlier meshes (e.g. physics
    // cloth meshes with distant vertices). Sponza fits within ~±100 units.
    constexpr f32 BOUND_LIMIT = 50.0f;
    origin_ = math::v3{BOUND_LIMIT, BOUND_LIMIT, BOUND_LIMIT};
    math::v3 maxPt{-BOUND_LIMIT, -BOUND_LIMIT, -BOUND_LIMIT};
    for (const auto& sdf : mesh_sdfs_) {
        // Skip meshes with implausible bounds
        bool plausible = true;
        for (int j = 0; j < 3; ++j) {
            if (sdf.bounds_min[j] < -BOUND_LIMIT || sdf.bounds_max[j] > BOUND_LIMIT ||
                sdf.bounds_max[j] - sdf.bounds_min[j] > BOUND_LIMIT) {
                plausible = false;
                break;
            }
        }
        if (!plausible) continue;
        for (int j = 0; j < 3; ++j) {
            origin_[j] = std::min(origin_[j], sdf.bounds_min[j]);
            maxPt[j]   = std::max(maxPt[j],   sdf.bounds_max[j]);
        }
    }
    // Add small margin
    math::v3 margin = (maxPt - origin_) * 0.02f;
    origin_ -= margin;
    maxPt += margin;
    extent_ = maxPt - origin_;
    voxel_size_ = std::max({extent_[0], extent_[1], extent_[2]}) / (f32)GLOBAL_RES;

    std::cerr << "[OfflineSDFMerger] Loaded " << sdfFound << " mesh SDFs"
              << ", global bounds: (" << origin_[0] << "," << origin_[1] << "," << origin_[2]
              << ") extent=(" << extent_[0] << "," << extent_[1] << "," << extent_[2] << ")"
              << " voxel_size=" << voxel_size_ << std::endl;

    return sdfFound;
}

f32 OfflineSDFMerger::SampleMeshSDF(const MeshSDF& sdf, f32 lx, f32 ly, f32 lz) const {
    // Convert local-space position to grid coordinates.
    f32 stepX = (sdf.bounds_max[0] - sdf.bounds_min[0]) / (f32)sdf.resolution[0];
    f32 stepY = (sdf.bounds_max[1] - sdf.bounds_min[1]) / (f32)sdf.resolution[1];
    f32 stepZ = (sdf.bounds_max[2] - sdf.bounds_min[2]) / (f32)sdf.resolution[2];

    f32 fx = (lx - sdf.bounds_min[0]) / stepX - 0.5f;
    f32 fy = (ly - sdf.bounds_min[1]) / stepY - 0.5f;
    f32 fz = (lz - sdf.bounds_min[2]) / stepZ - 0.5f;

    int32_t x0 = (int32_t)std::floor(fx), x1 = x0 + 1;
    int32_t y0 = (int32_t)std::floor(fy), y1 = y0 + 1;
    int32_t z0 = (int32_t)std::floor(fz), z1 = z0 + 1;

    f32 tx = fx - (f32)x0;
    f32 ty = fy - (f32)y0;
    f32 tz = fz - (f32)z0;

    auto clampCoord = [](int32_t v, u32 res) -> int32_t { return std::max(0, std::min((int32_t)res - 1, v)); };
    x0 = clampCoord(x0, sdf.resolution[0]); x1 = clampCoord(x1, sdf.resolution[0]);
    y0 = clampCoord(y0, sdf.resolution[1]); y1 = clampCoord(y1, sdf.resolution[1]);
    z0 = clampCoord(z0, sdf.resolution[2]); z1 = clampCoord(z1, sdf.resolution[2]);

    auto sampleData = [&](int32_t x, int32_t y, int32_t z, f32 wx, f32 wy, f32 wz) -> f32 {
        u32 idx = (u32)z * sdf.resolution[0] * sdf.resolution[1]
                + (u32)y * sdf.resolution[0]
                + (u32)x;
        if (idx >= sdf.data.size()) return 1e10f;
        // Decode unsigned distance: dist = u16 / 65535 * max_dim
        return (f32)sdf.data[idx] / 65535.0f * sdf.max_dim;
    };

    // Trilinear interpolation
    f32 c000 = sampleData(x0, y0, z0, lx, ly, lz), c100 = sampleData(x1, y0, z0, lx, ly, lz);
    f32 c010 = sampleData(x0, y1, z0, lx, ly, lz), c110 = sampleData(x1, y1, z0, lx, ly, lz);
    f32 c001 = sampleData(x0, y0, z1, lx, ly, lz), c101 = sampleData(x1, y0, z1, lx, ly, lz);
    f32 c011 = sampleData(x0, y1, z1, lx, ly, lz), c111 = sampleData(x1, y1, z1, lx, ly, lz);

    f32 c00 = c000 * (1 - tx) + c100 * tx;
    f32 c01 = c001 * (1 - tx) + c101 * tx;
    f32 c10 = c010 * (1 - tx) + c110 * tx;
    f32 c11 = c011 * (1 - tx) + c111 * tx;

    f32 c0 = c00 * (1 - ty) + c10 * ty;
    f32 c1 = c01 * (1 - ty) + c11 * ty;

    return c0 * (1 - tz) + c1 * tz;
}

bool OfflineSDFMerger::BuildAndUpload(rhi::RHIDeviceBase* device) {
    device_ = device;
    if (mesh_sdfs_.empty() || !device_) return false;

    const u32 res = GLOBAL_RES;
    const u64 totalVoxels = (u64)res * res * res;

    // Allocate CPU-side half-float buffer.
    // R16F: we store the signed distance as half-float.
    // We'll compute as f32 then convert to f16.
    std::vector<u16> globalData(totalVoxels, 0x3C00); // init to 1.0 in half-float

    f32 stepX = extent_[0] / (f32)res;
    f32 stepY = extent_[1] / (f32)res;
    f32 stepZ = extent_[2] / (f32)res;

    // Convert f32 to f16 (IEEE 754 half).
    auto f32toF16 = [](f32 f) -> u16 {
        u32 bits;
        std::memcpy(&bits, &f, 4);
        u32 sign = (bits >> 16) & 0x8000;
        int32_t exp = (int32_t)((bits >> 23) & 0xFF) - 127 + 15;
        u32 mant = bits & 0x7FFFFF;
        if (exp <= 0) {
            if (exp < -10) return sign;
            mant |= 0x800000;
            u32 sh = (u32)(14 - exp);
            u32 rounded = mant >> sh;
            if ((mant >> (sh - 1)) & 1) rounded++;
            return sign | (u16)rounded;
        } else if (exp == 0xFF - (127 - 15)) {
            return sign | 0x7C00 | (mant ? 0x200 : 0);
        }
        if (mant & 0x1000) {
            mant += 0x2000;
            if (mant & 0x800000) { mant = 0; exp++; }
        }
        if (exp >= 0x1F) return sign | 0x7C00;
        return (u16)(sign | (exp << 10) | (mant >> 13));
    };

    std::cerr << "[OfflineSDFMerger] Merging " << mesh_sdfs_.size()
              << " mesh SDFs into " << res << "³ global volume..." << std::endl;

    for (u32 gz = 0; gz < res; ++gz) {
        for (u32 gy = 0; gy < res; ++gy) {
            for (u32 gx = 0; gx < res; ++gx) {
                f32 wx = origin_[0] + ((f32)gx + 0.5f) * stepX;
                f32 wy = origin_[1] + ((f32)gy + 0.5f) * stepY;
                f32 wz = origin_[2] + ((f32)gz + 0.5f) * stepZ;

                f32 bestDist = 1e10f;
                for (const auto& sdf : mesh_sdfs_) {
                    // Bounds check (with small margin for trilinear sampling)
                    if (wx < sdf.bounds_min[0] - 0.01f || wx > sdf.bounds_max[0] + 0.01f ||
                        wy < sdf.bounds_min[1] - 0.01f || wy > sdf.bounds_max[1] + 0.01f ||
                        wz < sdf.bounds_min[2] - 0.01f || wz > sdf.bounds_max[2] + 0.01f)
                        continue;

                    f32 d = SampleMeshSDF(sdf, wx, wy, wz);
                    bestDist = std::min(bestDist, d);
                }

                // If no mesh contains this voxel, set a large positive distance.
                if (bestDist > 1e9f) bestDist = voxel_size_ * 4.0f;

                u64 idx = (u64)gz * res * res + (u64)gy * res + (u64)gx;
                globalData[idx] = f32toF16(bestDist);
            }
        }
        if (gz % 64 == 0) {
            std::cerr << "\r  Merging slice " << gz << "/" << res << std::flush;
        }
    }
    std::cerr << "\n[OfflineSDFMerger] Merge complete. Uploading to GPU..." << std::endl;

    // Dump SDF statistics to verify data has both positive and negative values.
    {
        int negCount = 0, posCount = 0, zeroCount = 0;
        f32 minVal = 1e10f, maxVal = -1e10f;
        for (u64 i = 0; i < totalVoxels; ++i) {
            // Decode half-float back to float for stats
            u16 h = globalData[i];
            // Simple f16→f32 decode
            u32 sign = (h >> 15) & 1;
            u32 exp = (h >> 10) & 0x1F;
            u32 mant = h & 0x3FF;
            f32 f;
            if (exp == 0 && mant == 0) { f = sign ? -0.0f : 0.0f; }
            else if (exp == 0) { f = 0.0f; }
            else {
                u32 bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                std::memcpy(&f, &bits, 4);
            }
            if (f < -0.001) negCount++;
            else if (f > 0.001) posCount++;
            else zeroCount++;
            minVal = std::min(minVal, f);
            maxVal = std::max(maxVal, f);
        }
        std::cerr << "[OfflineSDFMerger] SDF stats: neg=" << negCount
                  << " pos=" << posCount << " zero=" << zeroCount
                  << " min=" << minVal << " max=" << maxVal << std::endl;
    }

    // Create Texture3D and upload.
    rhi::TextureDesc desc{};
    desc.size = {res, res, res};
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = rhi::DataFormat::R16_Float;
    desc.type = rhi::TextureType::Texture3D;
    desc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;
    desc.memoryUsage = rhi::GPUMemoryUsage::Static;
    desc.name = "OfflineSDFMerger_GlobalSDF";

    global_sdf_texture_ = device_->CreateTexture(desc);
    if (global_sdf_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[OfflineSDFMerger] Failed to create Texture3D" << std::endl;
        return false;
    }

    // Upload via staging buffer (mirrors RHIGpuMesh::CreateAndUploadTexture3D).
    u32 bytesPerTexel = 2; // R16F
    u32 srcRowPitch = res * bytesPerTexel;
    u32 alignedRowPitch = (srcRowPitch + 255) & ~255;
    u64 slicePitch = (u64)alignedRowPitch * res;
    u64 totalSize = slicePitch * res;

    rhi::BufferDesc stagingDesc{};
    stagingDesc.size = totalSize;
    stagingDesc.bindFlags = (u32)rhi::BufferUsageFlags::TransferSrc;
    stagingDesc.memoryUsage = rhi::GPUMemoryUsage::Staging;
    stagingDesc.usage = rhi::GPUMemoryUsage::Staging;
    stagingDesc.name = "OfflineSDF_Staging";

    rhi::ResourceHandle staging = device_->CreateBuffer(stagingDesc);
    if (staging == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[OfflineSDFMerger] Failed to create staging buffer" << std::endl;
        return false;
    }

    void* mapped = device_->MapBuffer(staging, 0, totalSize);
    if (!mapped) {
        device_->DestroyBuffer(staging);
        return false;
    }

    // Copy row-by-row with alignment padding.
    u8* dst = static_cast<u8*>(mapped);
    for (u32 z = 0; z < res; ++z) {
        for (u32 y = 0; y < res; ++y) {
            const u16* srcRow = &globalData[(u64)z * res * res + (u64)y * res];
            std::memcpy(dst, srcRow, srcRowPitch);
            dst += alignedRowPitch;
        }
    }
    device_->UnmapBuffer(staging);

    // Copy staging → texture (mirrors RHIGpuMesh::CreateAndUploadTexture3D).
    auto cmdHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
#if OFFLINE_SDF_VULKAN_PATH
    // GetCommandBuffer is platform-specific; cast to VulkanDevice.
    rhi::VulkanCommandBuffer* cmd;
    {
        auto* vkDev = dynamic_cast<rhi::VulkanDevice*>(device_);
        if (vkDev) {
            cmd = vkDev->GetCommandBuffer(cmdHandle);
        } else {
            std::cerr << "[OfflineSDFMerger] Unsupported device for texture upload" << std::endl;
            device_->DestroyCommandBuffer(cmdHandle);
            device_->DestroyBuffer(staging);
            return false;
        }
    }
    cmd->Begin();
#endif

    rhi::BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = alignedRowPitch / bytesPerTexel;
    region.bufferImageHeight = res;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {res, res, res};

#if OFFLINE_SDF_VULKAN_PATH
    cmd->CopyBufferToTexture(staging, global_sdf_texture_, &region, 1);
    cmd->End();
    cmd->Submit();
    cmd->WaitForCompletion();
#endif
    device_->DestroyCommandBuffer(cmdHandle);
    device_->DestroyBuffer(staging);

    std::cerr << "[OfflineSDFMerger] Upload complete (" << totalSize / (1024*1024) << " MB)" << std::endl;
    return true;
}

} // namespace primal::graphics::nanite
