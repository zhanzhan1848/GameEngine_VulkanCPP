#include "GPUMaterialRegistry.h"
#include "../MaterialInstance.h"
#include "MaterialDataBuilder.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "JobSystem/JobSystem.h"
#include <iostream>
#include <algorithm>

namespace primal::graphics::nanite {

GPUMaterialRegistry::GPUMaterialRegistry() {
//    std::cout << "[GPUMaterialRegistry] Initialized" << std::endl;
}

GPUMaterialRegistry::~GPUMaterialRegistry() {
//    std::cout << "[GPUMaterialRegistry] Destroyed, releasing GPU resources" << std::endl;
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
//        std::cout << "[GPUMaterialRegistry] Material already registered, reusing ID " << it->second << std::endl;
        return it->second;
    }

    // Allocate new material ID
    MaterialID newID = static_cast<MaterialID>(materials_.size());

    // Extract material data from MaterialInstance
    MaterialData data = MaterialDataBuilder::ExtractMaterialData(instance);

    materials_.push_back(data);
    materialToID_[instance] = newID;
    registeredInstances_.push_back(instance);  // 🔥 NEW: Preserve registration order

//    std::cout << "[GPUMaterialRegistry] Registered material ID " << newID
//              << " (total: " << materials_.size() << ")" << std::endl;
    return newID;
}

jobsystem::JobHandle GPUMaterialRegistry::BuildAsync(rhi::RHIDeviceBase* device) {
//    std::cout << "[GPUMaterialRegistry] Starting async material data build..." << std::endl;

    // 🔥 CRITICAL FIX: Use registeredInstances_ to preserve registration order
    // This ensures texture array indices match MaterialID assignments
    const std::vector<MaterialInstance*>& instances = registeredInstances_;

    // Dawn device operations must run on the main thread that owns the device.
    // Build the lambda in a shared_ptr so we can pass it both to the scheduler
    // and to inline execution.
    auto work = std::make_shared<std::function<void()>>([this, device, instances]() {
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 1: Building texture mapping..." << std::endl;

        // 🎨 Phase 1: Build texture mapping (deduplication)
        TextureArrayBuildContext texCtx;
        MaterialDataBuilder::BuildTextureMapping(instances, texCtx, device);

        // 🎨 Phase 2: Create command buffer for texture operations
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 2: Creating command buffer..." << std::endl;

        rhi::CommandBufferHandle cmdBufHandle = device->CreateCommandBuffer(rhi::CommandQueueType::Transfer);
        if (cmdBufHandle == rhi::handles::INVALID_COMMAND_BUFFER) {
            buildError_ = "Failed to create command buffer for texture operations";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        rhi::RHICommandBuffer* cmdBuffer = rhi::GetCommandBuffer(cmdBufHandle);
        if (!cmdBuffer) {
            buildError_ = "Failed to get command buffer object";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // Begin command buffer recording
        if (!cmdBuffer->Begin()) {
            buildError_ = "Failed to begin command buffer recording";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // 🎨 Phase 3: Create texture arrays with BlitTexture
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 3: Creating texture arrays..." << std::endl;

        bool success = true;

        if (!texCtx.uniqueAlbedo.empty()) {
            if (!MaterialDataBuilder::CreateTextureArray(
                device, cmdBuffer, texCtx.uniqueAlbedo,
                "AlbedoTextureArray", albedoTextureArray_
            )) {
                std::cerr << "[GPUMaterialRegistry] ERROR: Failed to create albedo texture array" << std::endl;
                success = false;
            }
        }

        if (success && !texCtx.uniqueNormal.empty()) {
            if (!MaterialDataBuilder::CreateTextureArray(
                device, cmdBuffer, texCtx.uniqueNormal,
                "NormalTextureArray", normalTextureArray_
            )) {
                std::cerr << "[GPUMaterialRegistry] ERROR: Failed to create normal texture array" << std::endl;
                success = false;
            }
        }

        if (success && !texCtx.uniqueORM.empty()) {
            if (!MaterialDataBuilder::CreateTextureArray(
                device, cmdBuffer, texCtx.uniqueORM,
                "ORMTextureArray", ormTextureArray_
            )) {
                std::cerr << "[GPUMaterialRegistry] ERROR: Failed to create ORM texture array" << std::endl;
                success = false;
            }
        }

        if (!success) {
            buildError_ = "Failed to create texture arrays";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // 🎨 Phase 4: End command buffer and submit
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 4: Submitting commands..." << std::endl;

        if (!cmdBuffer->End()) {
            buildError_ = "Failed to end command buffer recording";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // Submit command buffer
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBufHandle;

        // Create fence for synchronization
        rhi::SyncHandle fence = device->CreateSync();
        if (fence == rhi::handles::INVALID_SYNC) {
            buildError_ = "Failed to create synchronization fence";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }
        submitInfo.signalFence = fence;

        if (!device->Submit(submitInfo)) {
            buildError_ = "Failed to submit command buffer";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            device->DestroySync(fence);
            return;
        }

        // 🎨 Phase 5: Update material data with texture indices
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 5: Updating material data..." << std::endl;

        // Rebuild materials_ vector with correct texture indices
        materials_.clear();
        materials_.reserve(instances.size());

        for (auto* instance : instances) {
            if (!instance) continue;

            // Extract material data with texture mapping
            MaterialData data = MaterialDataBuilder::ExtractMaterialData(instance, texCtx);
            materials_.push_back(data);
        }

        // 🎨 Phase 6: Wait for GPU operations to complete
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 6: Waiting for GPU completion..." << std::endl;

        constexpr u32 SYNC_TIMEOUT_MS = 5000;  // 5 second timeout
        if (!device->WaitForSync(fence, SYNC_TIMEOUT_MS)) {
            buildError_ = "Timeout waiting for texture operations to complete";
            std::cerr << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            device->DestroySync(fence);
            return;
        }

        device->DestroySync(fence);

        // Mark build complete
        buildComplete_ = true;
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Build complete successfully: "
//                  << materials_.size() << " materials, "
//                  << texCtx.uniqueAlbedo.size() << " albedo textures, "
//                  << texCtx.uniqueNormal.size() << " normal textures, "
//                  << texCtx.uniqueORM.size() << " ORM textures"
//                  << std::endl;
    });

    // Dawn device operations must execute on the main thread that owns the
    // device — running them on a JobSystem worker thread deadlocks inside
    // Dawn's ContentLessObjectCache. Run inline on main thread; return a
    // completed JobHandle so the caller's .Wait() is a no-op.
    const bool isDawn = (device->GetPlatform() == rhi::RHIPlatform::Dawn);
    if (isDawn) {
        (*work)();
        // Construct a completed handle so Wait() returns immediately.
        auto tracker = std::make_shared<jobsystem::JobStateTracker>(0);
        tracker->NotifyJobCompleted();
        buildJob_ = jobsystem::JobHandle(tracker);
        return buildJob_;
    }

    buildJob_ = jobsystem::JobSystem::Schedule([work]() { (*work)(); });
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

//    std::cout << "[GPUMaterialRegistry] Uploading material data to GPU..." << std::endl;

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

    // DIAGNOSTIC: dump first 5 materials to verify texture indices.
    std::cerr << "[MaterialDataDBG] Uploaded " << materials_.size()
              << " materials (sizeof=" << sizeof(MaterialData) << ")" << std::endl;
    for (size_t i = 0; i < std::min<size_t>(5, materials_.size()); ++i) {
        const auto& m = materials_[i];
        std::cerr << "[MaterialDataDBG]   [" << i
                  << "] albedo_idx=" << m.albedo_texture_idx
                  << " normal_idx=" << m.normal_texture_idx
                  << " orm_idx=" << m.orm_texture_idx
                  << " tint=(" << m.albedo_tint[0] << "," << m.albedo_tint[1] << "," << m.albedo_tint[2] << ")"
                  << " metal=" << m.metallic_factor
                  << " rough=" << m.roughness_factor
                  << " uv_scale=(" << m.uv_scale[0] << "," << m.uv_scale[1] << ")"
                  << std::endl;
    }

//    std::cout << "[GPUMaterialRegistry] Uploaded " << materials_.size()
//              << " materials (" << (materials_.size() * sizeof(MaterialData) / 1024.0f) << " KB)" << std::endl;
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