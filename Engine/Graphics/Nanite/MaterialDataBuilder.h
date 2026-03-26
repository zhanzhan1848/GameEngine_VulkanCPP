#pragma once

#include "GPUMaterialRegistry.h"
#include "../MaterialInstance.h"
#include "../RHI/Core/RHIResource.h"
#include "JobSystem/JobSystem.h"
#include <vector>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::nanite {

class MaterialDataBuilder {
public:
    // Extract material data from MaterialInstance
    static GPUMaterialRegistry::MaterialData ExtractMaterialData(
        graphics::MaterialInstance* instance
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

private:
    // Helper to get texture handle with validation
    static rhi::ResourceHandle GetTextureSafe(graphics::MaterialInstance* instance, u32 binding);
};

} // namespace primal::graphics::nanite
