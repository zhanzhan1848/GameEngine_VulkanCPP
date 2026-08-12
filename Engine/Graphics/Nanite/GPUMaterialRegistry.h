#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "JobSystem/JobSystem.h"
#include <vector>
#include <unordered_map>
#include <memory>

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
        uint32_t flags;               // Reserved for future use
    };

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
    ResourceHandle materialIDBuffer_;
    ResourceHandle materialDataBuffer_;
    ResourceHandle albedoTextureArray_;
    ResourceHandle normalTextureArray_;
    ResourceHandle ormTextureArray_;

    // Build state
    JobHandle buildJob_;
    bool buildComplete_ = false;
    std::string buildError_;
};

} // namespace primal::graphics::nanite
