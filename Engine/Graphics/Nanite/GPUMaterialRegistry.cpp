#include "GPUMaterialRegistry.h"
#include "../MaterialInstance.h"
#include "MaterialDataBuilder.h"
#include "../RHI/Core/RHIDevice.h"
#include "JobSystem/JobSystem.h"
#include <iostream>
#include <algorithm>

namespace primal::graphics::nanite {

GPUMaterialRegistry::GPUMaterialRegistry() {
    std::cout << "[GPUMaterialRegistry] Initialized" << std::endl;
}

GPUMaterialRegistry::~GPUMaterialRegistry() {
    std::cout << "[GPUMaterialRegistry] Destroyed, releasing GPU resources" << std::endl;
    // GPU resources will be released by RHI when ResourceHandles go out of scope
}

GPUMaterialRegistry::MaterialID GPUMaterialRegistry::RegisterMaterial(graphics::MaterialInstance* instance) {
    if (!instance) {
        std::cerr << "[GPUMaterialRegistry] ERROR: Cannot register null material instance" << std::endl;
        return INVALID_MATERIAL_ID;
    }

    // Check if already registered (deduplication)
    auto it = materialToID_.find(instance);
    if (it != materialToID_.end()) {
        std::cout << "[GPUMaterialRegistry] Material already registered, reusing ID " << it->second << std::endl;
        return it->second;
    }

    // Allocate new material ID
    MaterialID newID = static_cast<MaterialID>(materials_.size());

    // Extract material data from MaterialInstance
    MaterialData data = MaterialDataBuilder::ExtractMaterialData(instance);

    materials_.push_back(data);
    materialToID_[instance] = newID;

    std::cout << "[GPUMaterialRegistry] Registered material ID " << newID
              << " (total: " << materials_.size() << ")" << std::endl;
    return newID;
}

jobsystem::JobHandle GPUMaterialRegistry::BuildAsync(rhi::RHIDeviceBase* device) {
    std::cout << "[GPUMaterialRegistry] Starting async material data build..." << std::endl;

    // Collect all unique MaterialInstance pointers
    std::vector<MaterialInstance*> instances;
    instances.reserve(materialToID_.size());
    for (const auto& [instance, id] : materialToID_) {
        instances.push_back(instance);
    }

    // Schedule async build on worker thread
    // Reference: EngineTest/UnitTests/TestJobSystem.cpp:255
    buildJob_ = jobsystem::JobSystem::Schedule([this, device, instances]() {
        std::cout << "[GPUMaterialRegistry] [Worker Thread] Building texture arrays..." << std::endl;

        // Build texture arrays
        bool success = MaterialDataBuilder::BuildTextureArrays(
            device,
            instances,
            albedoTextureArray_,
            normalTextureArray_,
            ormTextureArray_
        );

        if (!success) {
            buildError_ = "Failed to build texture arrays";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // Build material data buffer
        // TODO: Create GPU buffer with materials_ data
        std::cout << "[GPUMaterialRegistry] [Worker Thread] Built "
                  << instances.size() << " materials" << std::endl;

        // Mark build complete
        buildComplete_ = true;
        std::cout << "[GPUMaterialRegistry] [Worker Thread] Build complete successfully" << std::endl;
    });

    return buildJob_;
}

bool GPUMaterialRegistry::UploadToGPU(RHIDeviceBase* device) {
    if (!buildComplete_) {
        std::cerr << "[GPUMaterialRegistry] ERROR: Cannot upload, build not complete" << std::endl;
        return false;
    }

    if (!buildError_.empty()) {
        std::cerr << "[GPUMaterialRegistry] ERROR: Cannot upload, build had errors: "
                  << buildError_ << std::endl;
        return false;
    }

    std::cout << "[GPUMaterialRegistry] Uploading material data to GPU..." << std::endl;

    // Create material data buffer
    // Reference: TestParticleSponza.cpp texture creation pattern
    if (materials_.empty()) {
        std::cerr << "[GPUMaterialRegistry] WARNING: No materials to upload" << std::endl;
        return true;  // Not an error, just nothing to do
    }

    // Create storage buffer for material data
    rhi::BufferDesc materialBufferDesc{};
    materialBufferDesc.size = materials_.size() * sizeof(MaterialData);
    materialBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::TransferDst);
    materialBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    materialDataBuffer_ = device->CreateBuffer(materialBufferDesc);

    if (materialDataBuffer_ == rhi::handles::INVALID_RESOURCE) {
        buildError_ = "Failed to create material data buffer";
        std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
        return false;
    }

    // Upload material data to buffer
    // TODO: Use staging buffer and copy for efficiency
    void* mappedData = device->MapBuffer(materialDataBuffer_);
    if (mappedData) {
        memcpy(mappedData, materials_.data(), materials_.size() * sizeof(MaterialData));
        device->UnmapBuffer(materialDataBuffer_);
    } else {
        buildError_ = "Failed to map material data buffer";
        std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
        return false;
    }

    std::cout << "[GPUMaterialRegistry] Uploaded " << materials_.size()
              << " materials (" << (materials_.size() * sizeof(MaterialData) / 1024.0f) << " KB)" << std::endl;
    return true;
}

GPUMaterialRegistry::Stats GPUMaterialRegistry::GetStats() const {
    Stats stats{};
    stats.materialCount = static_cast<uint32_t>(materials_.size());
    stats.usedMaterialCount = static_cast<uint32_t>(materialToID_.size());

    // Count texture arrays (check if valid)
    stats.textureArrayCount = 0;
    if (albedoTextureArray_ != rhi::handles::INVALID_RESOURCE) stats.textureArrayCount++;
    if (normalTextureArray_ != rhi::handles::INVALID_RESOURCE) stats.textureArrayCount++;
    if (ormTextureArray_ != rhi::handles::INVALID_RESOURCE) stats.textureArrayCount++;

    // Estimate memory usage (rough approximation)
    stats.memoryUsageMB = (stats.materialCount * sizeof(MaterialData)) / (1024.0f * 1024.0f);
    // TODO: Add texture array memory estimation

    stats.avgFrameTimeMs = 0.0f;  // TODO: Implement frame time tracking
    return stats;
}

bool GPUMaterialRegistry::IsValid() const {
    return buildComplete_ && buildError_.empty() && !materials_.empty();
}

} // namespace primal::graphics::nanite