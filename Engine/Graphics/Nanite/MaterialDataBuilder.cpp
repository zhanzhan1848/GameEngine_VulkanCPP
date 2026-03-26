#include "MaterialDataBuilder.h"
#include "../MaterialInstance.h"
#include "../RHI/Core/RHIDevice.h"
#include <iostream>
#include <cmath>
#include <algorithm>

namespace primal::graphics::nanite {

GPUMaterialRegistry::MaterialData MaterialDataBuilder::ExtractMaterialData(
    graphics::MaterialInstance* instance
) {
    GPUMaterialRegistry::MaterialData data{};

    if (!instance) {
        std::cerr << "[MaterialDataBuilder] Null material instance" << std::endl;
        return data;
    }

    // Get texture handles (placeholder indices for now)
    data.albedo_texture_idx = 0;
    data.normal_texture_idx = 0;
    data.orm_texture_idx = 0;

    // Get material factors
    math::v3 tint;
    float metallic, roughness;
    instance->GetMaterialFactors(tint, metallic, roughness);

    data.albedo_tint[0] = tint.x;
    data.albedo_tint[1] = tint.y;
    data.albedo_tint[2] = tint.z;
    data.metallic_factor = metallic;
    data.roughness_factor = roughness;
    data.normal_scale = 1.0f;
    data.flags = 0;

    return data;
}

bool MaterialDataBuilder::AreMaterialsCompatible(
    const graphics::MaterialInstance& a,
    const graphics::MaterialInstance& b,
    float tolerance
) {
    // Compare textures
    rhi::ResourceHandle a_albedo = a.GetTextureHandle(0);
    rhi::ResourceHandle b_albedo = b.GetTextureHandle(0);
    if (a_albedo != b_albedo) return false;

    rhi::ResourceHandle a_normal = a.GetTextureHandle(1);
    rhi::ResourceHandle b_normal = b.GetTextureHandle(1);
    if (a_normal != b_normal) return false;

    rhi::ResourceHandle a_orm = a.GetTextureHandle(2);
    rhi::ResourceHandle b_orm = b.GetTextureHandle(2);
    if (a_orm != b_orm) return false;

    // Compare material factors
    math::v3 a_tint, b_tint;
    float a_metallic, b_metallic;
    float a_roughness, b_roughness;

    a.GetMaterialFactors(a_tint, a_metallic, a_roughness);
    b.GetMaterialFactors(b_tint, b_metallic, b_roughness);

    // Check tint difference
    float tint_diff = std::sqrt(
        std::pow(a_tint.x - b_tint.x, 2) +
        std::pow(a_tint.y - b_tint.y, 2) +
        std::pow(a_tint.z - b_tint.z, 2)
    );
    if (tint_diff > tolerance) return false;

    // Check metallic difference
    if (std::abs(a_metallic - b_metallic) > tolerance) return false;

    // Check roughness difference
    if (std::abs(a_roughness - b_roughness) > tolerance) return false;

    return true;
}

bool MaterialDataBuilder::BuildTextureArrays(
    rhi::RHIDeviceBase* device,
    const std::vector<graphics::MaterialInstance*>& instances,
    rhi::ResourceHandle& outAlbedoArray,
    rhi::ResourceHandle& outNormalArray,
    rhi::ResourceHandle& outORMArray
) {
    if (instances.empty()) {
        std::cerr << "[MaterialDataBuilder] WARNING: No material instances to build" << std::endl;
        return true;  // Not an error, just nothing to do
    }

    std::cout << "[MaterialDataBuilder] Building texture arrays for "
              << instances.size() << " materials..." << std::endl;

    // Phase 1: Collect unique textures
    std::vector<rhi::ResourceHandle> albedoTextures;
    std::vector<rhi::ResourceHandle> normalTextures;
    std::vector<rhi::ResourceHandle> ormTextures;

    for (const auto* instance : instances) {
        if (!instance) continue;

        // Get texture handles
        rhi::ResourceHandle albedo = GetTextureSafe(const_cast<MaterialInstance*>(instance), 0);
        rhi::ResourceHandle normal = GetTextureSafe(const_cast<MaterialInstance*>(instance), 1);
        rhi::ResourceHandle orm = GetTextureSafe(const_cast<MaterialInstance*>(instance), 2);

        // Skip if invalid (will use default texture in shader)
        if (albedo != rhi::handles::INVALID_RESOURCE) {
            albedoTextures.push_back(albedo);
        }
        if (normal != rhi::handles::INVALID_RESOURCE) {
            normalTextures.push_back(normal);
        }
        if (orm != rhi::handles::INVALID_RESOURCE) {
            ormTextures.push_back(orm);
        }
    }

    std::cout << "[MaterialDataBuilder] Collected "
              << albedoTextures.size() << " albedo, "
              << normalTextures.size() << " normal, "
              << ormTextures.size() << " ORM textures" << std::endl;

    // Phase 1: For now, return individual textures (Phase 1)
    // TODO: Implement actual texture array creation (requires RHI extension)
    // Reference: TestParticleSponza.cpp:1750 for texture creation pattern
    //
    // Pseudo-code for texture array creation:
    // for (size_t i = 0; i < albedoTextures.size(); ++i) {
    //     // Get texture description from source texture
    //     rhi::TextureDesc srcDesc = device->GetTextureDesc(albedoTextures[i]);
    //
    //     // Create array texture
    //     rhi::TextureDesc arrayDesc = srcDesc;
    //     arrayDesc.arrayLayers = static_cast<u32>(albedoTextures.size());
    //     arrayDesc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
    //
    //     // Copy each texture slice into array
    //     device->CopyTextureSlice(albedoTextures[i], outAlbedoArray, i);
    // }

    // Phase 1 fallback: Use individual textures
    // This is not ideal but allows us to proceed with implementation
    if (!albedoTextures.empty()) {
        outAlbedoArray = albedoTextures[0];  // Use first texture as placeholder
        std::cout << "[MaterialDataBuilder] WARNING: Using placeholder texture (Phase 1)" << std::endl;
    }

    if (!normalTextures.empty()) {
        outNormalArray = normalTextures[0];
    }

    if (!ormTextures.empty()) {
        outORMArray = ormTextures[0];
    }

    std::cout << "[MaterialDataBuilder] Texture array build complete (Phase 1 placeholder)" << std::endl;
    return true;
}

jobsystem::JobHandle MaterialDataBuilder::BuildAsync(
    rhi::RHIDeviceBase* device,
    const std::vector<graphics::MaterialInstance*>& instances,
    GPUMaterialRegistry* registry
) {
    // TODO: Implement async build
    std::cout << "[MaterialDataBuilder] BuildAsync not yet implemented" << std::endl;
    return jobsystem::JobHandle();
}

rhi::ResourceHandle MaterialDataBuilder::GetTextureSafe(
    graphics::MaterialInstance* instance,
    u32 binding
) {
    if (!instance) {
        return rhi::handles::INVALID_RESOURCE;
    }

    rhi::ResourceHandle handle = instance->GetTextureHandle(binding);
    if (handle == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[MaterialDataBuilder] Missing texture at binding " << binding << std::endl;
    }

    return handle;
}

} // namespace primal::graphics::nanite
