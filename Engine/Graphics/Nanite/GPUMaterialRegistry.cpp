#include "GPUMaterialRegistry.h"
#include "../MaterialInstance.h"
#include "MaterialDataBuilder.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "JobSystem/JobSystem.h"
#include <iostream>
#include <algorithm>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::nanite {

GPUMaterialRegistry::GPUMaterialRegistry() {
//    std::cout << "[GPUMaterialRegistry] Initialized" << std::endl;
}

GPUMaterialRegistry::~GPUMaterialRegistry() {
//    std::cout << "[GPUMaterialRegistry] Destroyed, releasing GPU resources" << std::endl;
}

void GPUMaterialRegistry::Shutdown(RHIDeviceBase* device) {
    if (!device) return;
    if (materialIDBuffer_ != rhi::handles::INVALID_RESOURCE) {
        device->DestroyBuffer(materialIDBuffer_);
        materialIDBuffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (materialDataBuffer_ != rhi::handles::INVALID_RESOURCE) {
        device->DestroyBuffer(materialDataBuffer_);
        materialDataBuffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (albedoTextureArray_ != rhi::handles::INVALID_RESOURCE) {
        device->DestroyTexture(albedoTextureArray_);
        albedoTextureArray_ = rhi::handles::INVALID_RESOURCE;
    }
    if (normalTextureArray_ != rhi::handles::INVALID_RESOURCE) {
        device->DestroyTexture(normalTextureArray_);
        normalTextureArray_ = rhi::handles::INVALID_RESOURCE;
    }
    if (ormTextureArray_ != rhi::handles::INVALID_RESOURCE) {
        device->DestroyTexture(ormTextureArray_);
        ormTextureArray_ = rhi::handles::INVALID_RESOURCE;
    }
}

GPUMaterialRegistry::MaterialID GPUMaterialRegistry::RegisterMaterial(graphics::MaterialInstance* instance) {
    if (!instance) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: Cannot register null material instance" << std::endl;
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

GPUMaterialRegistry::MaterialID GPUMaterialRegistry::RegisterVTMaterial(
    uint32_t vt_id, const math::v3& albedo_tint, float metallic, float roughness,
    float uv_scale_x, float uv_scale_y) {
    MaterialID newID = static_cast<MaterialID>(materials_.size());
    MaterialData data{};
    data.albedo_texture_idx = 0;
    data.normal_texture_idx = 0;
    data.orm_texture_idx = 0;
    data.albedo_tint[0] = albedo_tint.x;
    data.albedo_tint[1] = albedo_tint.y;
    data.albedo_tint[2] = albedo_tint.z;
    data.metallic_factor = metallic;
    data.roughness_factor = roughness;
    data.normal_scale = 1.0f;
    data.uv_scale[0] = uv_scale_x;
    data.uv_scale[1] = uv_scale_y;
    data.flags = EncodeVTFlags(vt_id);
    materials_.push_back(data);
    registeredInstances_.push_back(nullptr);   // VT 材质无 MaterialInstance
    PRIMAL_LOG_STREAM(Info, "Render") << "[GPUMaterialRegistry] VT material id=" << newID
              << " vtId=" << vt_id << std::endl;
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
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        rhi::RHICommandBuffer* cmdBuffer = rhi::GetCommandBuffer(cmdBufHandle);
        if (!cmdBuffer) {
            buildError_ = "Failed to get command buffer object";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // Begin command buffer recording
        if (!cmdBuffer->Begin()) {
            buildError_ = "Failed to begin command buffer recording";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
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
                PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: Failed to create albedo texture array" << std::endl;
                success = false;
            }
        }

        if (success && !texCtx.uniqueNormal.empty()) {
            if (!MaterialDataBuilder::CreateTextureArray(
                device, cmdBuffer, texCtx.uniqueNormal,
                "NormalTextureArray", normalTextureArray_
            )) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: Failed to create normal texture array" << std::endl;
                success = false;
            }
        }

        if (success && !texCtx.uniqueORM.empty()) {
            if (!MaterialDataBuilder::CreateTextureArray(
                device, cmdBuffer, texCtx.uniqueORM,
                "ORMTextureArray", ormTextureArray_
            )) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: Failed to create ORM texture array" << std::endl;
                success = false;
            }
        }

        if (!success) {
            buildError_ = "Failed to create texture arrays";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // 🎨 Phase 4: End command buffer and submit
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 4: Submitting commands..." << std::endl;

        if (!cmdBuffer->End()) {
            buildError_ = "Failed to end command buffer recording";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }

        // Submit command buffer
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBufHandle;

        // Create fence for synchronization
        rhi::SyncHandle fence = device->CreateSync();
        if (fence == rhi::handles::INVALID_SYNC) {
            buildError_ = "Failed to create synchronization fence";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            return;
        }
        submitInfo.signalFence = fence;

        if (!device->Submit(submitInfo)) {
            buildError_ = "Failed to submit command buffer";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
            device->DestroySync(fence);
            return;
        }

        // 🎨 Phase 5: Update material data with texture indices
        PRIMAL_LOG_STREAM(Info, "Render") << "[GPUMaterialRegistry] Phase 5: Rebuilding " << instances.size()
                  << " materials with texture context ("
                  << texCtx.albedoToIndex.size() << " albedo, "
                  << texCtx.normalToIndex.size() << " normal, "
                  << texCtx.ormToIndex.size() << " ORM unique textures)" << std::endl;

        // Rebuild materials_ vector with correct texture indices.
        // M4:VT 材质(instance == nullptr,flags 带 kMaterialVTFlag)原样保留 —
        // 其纹理走 VT 系统页表/atlas,不依赖 Texture2DArray 映射
        std::vector<MaterialData> rebuilt;
        rebuilt.reserve(instances.size());
        size_t nextVT = 0;
        const std::vector<MaterialData> oldMaterials = materials_;
        for (auto* instance : instances) {
            if (!instance) {
                // VT 条目槽位:按注册序回填旧 VT 数据
                while (nextVT < oldMaterials.size() &&
                       !(oldMaterials[nextVT].flags & kMaterialVTFlag))
                    ++nextVT;
                if (nextVT < oldMaterials.size()) {
                    rebuilt.push_back(oldMaterials[nextVT]);
                    ++nextVT;
                }
                continue;
            }
            rebuilt.push_back(MaterialDataBuilder::ExtractMaterialData(instance, texCtx));
        }
        materials_ = std::move(rebuilt);

        // Diagnostic: print first 3 materials' key fields
        for (size_t i = 0; i < std::min(materials_.size(), (size_t)3); ++i) {
            const auto& m = materials_[i];
            PRIMAL_LOG_STREAM(Info, "Render") << "  [MaterialID=" << i << "] albedo_idx=" << m.albedo_texture_idx
                      << " normal_idx=" << m.normal_texture_idx
                      << " orm_idx=" << m.orm_texture_idx
                      << " uv_scale=(" << m.uv_scale[0] << "," << m.uv_scale[1] << ")"
                      << " tint=(" << m.albedo_tint[0] << "," << m.albedo_tint[1] << "," << m.albedo_tint[2] << ")"
                      << std::endl;
        }
        // 🎨 Phase 6: Wait for GPU operations to complete
//        std::cout << "[GPUMaterialRegistry] [Worker Thread] Phase 6: Waiting for GPU completion..." << std::endl;

        constexpr u32 SYNC_TIMEOUT_MS = 5000;  // 5 second timeout
        if (!device->WaitForSync(fence, SYNC_TIMEOUT_MS)) {
            buildError_ = "Timeout waiting for texture operations to complete";
            PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: Cannot upload, build not complete" << std::endl;
        return false;
    }

    if (!buildError_.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: Cannot upload, build had errors: "
                  << buildError_ << std::endl;
        return false;
    }

//    std::cout << "[GPUMaterialRegistry] Uploading material data to GPU..." << std::endl;

    // Create material data buffer
    // Reference: TestParticleSponza.cpp texture creation pattern
    if (materials_.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] WARNING: No materials to upload" << std::endl;
        return true;  // Not an error, just nothing to do
    }

    // Create storage buffer for material data
    rhi::BufferDesc materialBufferDesc{};
    materialBufferDesc.size = materials_.size() * sizeof(MaterialData);
    materialBufferDesc.type = rhi::BufferType::Structured;
    materialBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::TransferDst);
    // Metal's Dynamic buffers come from the transient shared placement heap;
    // this long-lived table must use a standalone Shared allocation. Unknown
    // maps to standalone Shared on Metal. Other backends keep the established
    // Dynamic host-visible path.
    materialBufferDesc.memoryUsage = device->GetPlatform() == rhi::RHIPlatform::Metal
        ? rhi::GPUMemoryUsage::Unknown
        : rhi::GPUMemoryUsage::Dynamic;
    materialBufferDesc.name = "NaniteMaterialData";
    materialDataBuffer_ = device->CreateBuffer(materialBufferDesc);

    if (materialDataBuffer_ == rhi::handles::INVALID_RESOURCE) {
        buildError_ = "Failed to create material data buffer";
        PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
        return false;
    }

    // Upload the complete table directly into the persistent host-visible buffer.
    const u64 uploadSize = materials_.size() * sizeof(MaterialData);
    void* mappedData = device->MapBuffer(materialDataBuffer_);
    if (mappedData) {
        memcpy(mappedData, materials_.data(), uploadSize);
        device->UnmapBuffer(materialDataBuffer_);
        // Diagnostic: verify what was uploaded
        PRIMAL_LOG_STREAM(Info, "Render") << "[GPUMaterialRegistry] UploadToGPU: " << materials_.size()
                  << " materials, sizeof(MaterialData)=" << sizeof(MaterialData) << std::endl;
        for (size_t i = 0; i < std::min(materials_.size(), (size_t)3); ++i) {
            const auto& m = materials_[i];
            PRIMAL_LOG_STREAM(Info, "Render") << "  [MaterialID=" << i << "] UPLOAD: albedo_idx=" << m.albedo_texture_idx
                      << " uv_scale=(" << m.uv_scale[0] << "," << m.uv_scale[1] << ")"
                      << std::endl;
        }
    } else {
        buildError_ = "Failed to upload material data buffer";
        PRIMAL_LOG_STREAM(Error, "Render") << "[GPUMaterialRegistry] ERROR: " << buildError_ << std::endl;
        return false;
    }

    // DIAGNOSTIC: dump first 5 materials to verify texture indices.
    PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataDBG] Uploaded " << materials_.size()
              << " materials (sizeof=" << sizeof(MaterialData) << ")" << std::endl;
    for (size_t i = 0; i < std::min<size_t>(5, materials_.size()); ++i) {
        const auto& m = materials_[i];
        PRIMAL_LOG_STREAM(Error, "Render") << "[MaterialDataDBG]   [" << i
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
