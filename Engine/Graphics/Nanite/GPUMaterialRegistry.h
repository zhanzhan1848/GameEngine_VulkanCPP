#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "JobSystem/JobSystem.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstddef>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics {
    class MaterialInstance;
}

namespace primal::graphics::nanite {

class GPUMaterialRegistry {
public:
    // Type aliases for brevity
    using ResourceHandle = rhi::ResourceHandle;
    using RHIDeviceBase = rhi::RHIDeviceBase;
    using JobHandle = jobsystem::JobHandle;
public:
    // Material data structure (matches GPU layout)
    struct MaterialData {
        uint32_t albedo_texture_idx;  // Index into albedo texture array
        uint32_t normal_texture_idx;  // Index into normal texture array
        uint32_t orm_texture_idx;     // Index into ORM texture array
        float albedo_tint[3];         // RGB tint
        float metallic_factor;
        float roughness_factor;
        float normal_scale;
        float uv_scale[2];            // UV scaling (tiling) for texture repetition
        uint32_t flags;               // bit0 = VT 材质;bits[4:8) = vtId(M4;
                                     // 单尺寸 Texture2DArray 限制随 VT 化解除)
    };
    static_assert(sizeof(MaterialData) == 48, "MaterialData GPU stride changed");
    static_assert(offsetof(MaterialData, albedo_tint) == 12, "MaterialData tint offset changed");
    static_assert(offsetof(MaterialData, uv_scale) == 36, "MaterialData UV offset changed");
    static_assert(offsetof(MaterialData, flags) == 44, "MaterialData flags offset changed");

    // M4:flags 编码 — VT 材质标志 + vtId(feedback 编码上限 15)
    static constexpr uint32_t kMaterialVTFlag = 0x1u;
    static constexpr uint32_t kMaterialVTIdShift = 4;
    static constexpr uint32_t kMaterialVTIdMask = 0xFu;

    // Per-material thin-surface transport.  Keep this in the existing flags
    // word so MaterialData remains the 48-byte ABI shared by all backends.
    // bits[8:16) carry transmission as UNorm8.  bits[16:24) are reserved for
    // the Surface Cache thickness term (stage 3); do not consume them for
    // unrelated flags.
    static constexpr uint32_t kMaterialTransmissionShift = 8u;
    static constexpr uint32_t kMaterialTransmissionMask = 0xFFu << kMaterialTransmissionShift;
    static constexpr uint32_t kMaterialThicknessShift = 16u;
    static constexpr uint32_t kMaterialThicknessMask = 0xFFu << kMaterialThicknessShift;

    static uint32_t EncodeTransmissionFlags(uint32_t flags, float transmission) {
        // Avoid <cmath> in this header; the ordered comparisons also map NaN
        // to the safe opaque default of zero.
        const float clamped = transmission > 1.0f ? 1.0f :
                              (transmission > 0.0f ? transmission : 0.0f);
        const uint32_t encoded = static_cast<uint32_t>(clamped * 255.0f + 0.5f);
        return (flags & ~kMaterialTransmissionMask) |
               ((encoded & 0xFFu) << kMaterialTransmissionShift);
    }

    static float DecodeTransmissionFactor(uint32_t flags) {
        return static_cast<float>((flags & kMaterialTransmissionMask) >>
                                  kMaterialTransmissionShift) / 255.0f;
    }

    static uint32_t EncodeThicknessFlags(uint32_t flags, float thickness) {
        const float clamped = thickness > 1.0f ? 1.0f :
                              (thickness > 0.0f ? thickness : 0.0f);
        const uint32_t encoded = static_cast<uint32_t>(clamped * 255.0f + 0.5f);
        return (flags & ~kMaterialThicknessMask) |
               ((encoded & 0xFFu) << kMaterialThicknessShift);
    }

    static float DecodeThicknessFactor(uint32_t flags) {
        return static_cast<float>((flags & kMaterialThicknessMask) >>
                                  kMaterialThicknessShift) / 255.0f;
    }

    static uint32_t EncodeVTFlags(uint32_t vtId) {
        return kMaterialVTFlag | ((vtId & kMaterialVTIdMask) << kMaterialVTIdShift);
    }
    static bool DecodeVTFlags(uint32_t flags, uint32_t& vtId) {
        if ((flags & kMaterialVTFlag) == 0) return false;
        vtId = (flags >> kMaterialVTIdShift) & kMaterialVTIdMask;
        return true;
    }

    // Material ID type
    using MaterialID = uint32_t;
    static constexpr MaterialID INVALID_MATERIAL_ID = UINT32_MAX;

    // Statistics structure
    struct Stats {
        uint32_t materialCount;
        uint32_t usedMaterialCount;
        uint32_t textureArrayCount;
        float memoryUsageMB;
        float avgFrameTimeMs;
    };

    // Constructor/Destructor
    GPUMaterialRegistry();
    ~GPUMaterialRegistry();

    // Prevent copying
    GPUMaterialRegistry(const GPUMaterialRegistry&) = delete;
    GPUMaterialRegistry& operator=(const GPUMaterialRegistry&) = delete;

    // Register a material (extract data from MaterialInstance)
    // Returns: MaterialID, or INVALID_MATERIAL_ID on failure
    MaterialID RegisterMaterial(graphics::MaterialInstance* instance);

    // M4:VT 材质注册 — 纹理走 virtual_texture_system(页表/atlas 全局
    // set),无需 Texture2DArray 层;uvScale 平铺进 MaterialData。
    // texture_idx 三槽置 0(数组未用)。
    MaterialID RegisterVTMaterial(uint32_t vt_id,
                                  const math::v3& albedo_tint = math::v3{1, 1, 1},
                                  float metallic = 0.0f, float roughness = 1.0f,
                                  float uv_scale_x = 1.0f, float uv_scale_y = 1.0f);

    // Async build material data structures
    // Returns: JobHandle for tracking completion
    JobHandle BuildAsync(RHIDeviceBase* device);

    // Upload material data to GPU (call after BuildAsync completes)
    bool UploadToGPU(RHIDeviceBase* device);

    // Check if build is complete
    bool IsBuildComplete() const { return buildComplete_; }

    // Check if there were build errors
    bool HasBuildErrors() const { return !buildError_.empty(); }
    const std::string& GetBuildError() const { return buildError_; }

    // Get GPU resource handles
    ResourceHandle GetMaterialIDBuffer() const { return materialIDBuffer_; }
    ResourceHandle GetMaterialDataBuffer() const { return materialDataBuffer_; }

    // 🔧 NEW: Get mutable material data for UV scaling adjustments
    MaterialData* GetMaterialDataMutable() { return materials_.data(); }
    const MaterialData* GetMaterialData() const { return materials_.data(); }
    size_t GetMaterialCount() const { return materials_.size(); }
    ResourceHandle GetAlbedoTextureArray() const { return albedoTextureArray_; }
    ResourceHandle GetNormalTextureArray() const { return normalTextureArray_; }
    ResourceHandle GetORMTextureArray() const { return ormTextureArray_; }

    // Get statistics
    Stats GetStats() const;

    // Validate registry state
    bool IsValid() const;

    /// Release all GPU resources. Must call before device shutdown.
    void Shutdown(RHIDeviceBase* device);

private:
    // Material storage
    std::vector<MaterialData> materials_;
    std::unordered_map<graphics::MaterialInstance*, MaterialID> materialToID_;
    std::vector<MaterialInstance*> registeredInstances_;  // 🔥 NEW: Preserve registration order

    // GPU resources
    ResourceHandle materialIDBuffer_{rhi::handles::INVALID_RESOURCE};
    ResourceHandle materialDataBuffer_{rhi::handles::INVALID_RESOURCE};
    ResourceHandle albedoTextureArray_{rhi::handles::INVALID_RESOURCE};
    ResourceHandle normalTextureArray_{rhi::handles::INVALID_RESOURCE};
    ResourceHandle ormTextureArray_{rhi::handles::INVALID_RESOURCE};

    // Build state
    JobHandle buildJob_;
    bool buildComplete_ = false;
    std::string buildError_;
};

} // namespace primal::graphics::nanite
