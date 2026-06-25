#pragma once

#include "GPUMaterialRegistry.h"
#include "../MaterialInstance.h"
#include "../RHI/Core/RHIResource.h"
#include "JobSystem/JobSystem.h"
#include <vector>
#include <unordered_map>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::nanite {

// 🎨 Texture array build context
struct TextureArrayBuildContext {
    // Deduplication mapping: texture handle → array index
    std::unordered_map<rhi::ResourceHandle, uint32_t> albedoToIndex;
    std::unordered_map<rhi::ResourceHandle, uint32_t> normalToIndex;
    std::unordered_map<rhi::ResourceHandle, uint32_t> ormToIndex;

    // Unique texture collection (in insertion order)
    std::vector<rhi::ResourceHandle> uniqueAlbedo;
    std::vector<rhi::ResourceHandle> uniqueNormal;
    std::vector<rhi::ResourceHandle> uniqueORM;

    // Placeholder textures (1x1 default textures)
    rhi::ResourceHandle placeholderAlbedo{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle placeholderNormal{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle placeholderORM{rhi::handles::INVALID_RESOURCE};
};

class MaterialDataBuilder {
public:
    // Extract material data from MaterialInstance (without texture mapping - uses default indices)
    static GPUMaterialRegistry::MaterialData ExtractMaterialData(
        graphics::MaterialInstance* instance
    );

    // Extract material data from MaterialInstance (with texture mapping)
    static GPUMaterialRegistry::MaterialData ExtractMaterialData(
        graphics::MaterialInstance* instance,
        const TextureArrayBuildContext& ctx
    );

    // Check if two materials are compatible (can share same ID)
    static bool AreMaterialsCompatible(
        const graphics::MaterialInstance& a,
        const graphics::MaterialInstance& b,
        float tolerance = 0.001f
    );

    // Build texture arrays from material instances
    static bool BuildTextureArrays(
        rhi::RHIDeviceBase* device,
        const std::vector<graphics::MaterialInstance*>& instances,
        rhi::ResourceHandle& outAlbedoArray,
        rhi::ResourceHandle& outNormalArray,
        rhi::ResourceHandle& outORMArray
    );

    // Async build material data
    static jobsystem::JobHandle BuildAsync(
        rhi::RHIDeviceBase* device,
        const std::vector<graphics::MaterialInstance*>& instances,
        GPUMaterialRegistry* registry
    );

    // 🎨 NEW: Texture mapping and array creation helpers (public for GPUMaterialRegistry)
    static void BuildTextureMapping(
        const std::vector<graphics::MaterialInstance*>& instances,
        TextureArrayBuildContext& ctx,
        rhi::RHIDeviceBase* device
    );

    static void CreatePlaceholderTextures(
        TextureArrayBuildContext& ctx,
        rhi::RHIDeviceBase* device,
        rhi::DataFormat albedo_fmt,
        rhi::DataFormat normal_fmt,
        rhi::DataFormat orm_fmt
    );

    static bool CreateTextureArray(
        rhi::RHIDeviceBase* device,
        rhi::RHICommandBuffer* cmdBuffer,
        const std::vector<rhi::ResourceHandle>& sources,
        const char* name,
        rhi::ResourceHandle& outArray
    );

private:
    // Helper to get texture handle with validation
    static rhi::ResourceHandle GetTextureSafe(graphics::MaterialInstance* instance, u32 binding);
};

} // namespace primal::graphics::nanite
