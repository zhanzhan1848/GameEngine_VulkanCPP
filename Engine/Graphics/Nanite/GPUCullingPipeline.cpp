#include "GPUCullingPipeline.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHIResource.h"
#include "../Scene/RenderSceneSnapshot.h"
#include "../Nanite/NaniteStreamingManager.h"
#include <iostream>

namespace primal::graphics::nanite {

GPUCullingPipeline& GPUCullingPipeline::Get() {
    static GPUCullingPipeline instance;
    return instance;
}

bool GPUCullingPipeline::Initialize(rhi::RHIDeviceBase* device, const CullingConfig& config) {
    if (!device) return false;
    if (initialized_) return true;
    
    device_ = device;
    config_ = config;
    
    // Create buffers for culling
    rhi::BufferDesc boundsDesc{};
    boundsDesc.size = sizeof(math::v3) * 2 * config.max_clusters_per_dispatch;
    boundsDesc.type = rhi::BufferType::Structured;
    boundsDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    boundsDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ShaderResource);
    cluster_bounds_buffer_ = device->CreateBuffer(boundsDesc);
    
    rhi::BufferDesc lodDesc{};
    lodDesc.size = sizeof(u32) * 4 * config.max_clusters_per_dispatch;
    lodDesc.type = rhi::BufferType::Structured;
    lodDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    lodDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ShaderResource);
    lod_data_buffer_ = device->CreateBuffer(lodDesc);
    
    rhi::BufferDesc visDesc{};
    visDesc.size = sizeof(u32) * config.max_clusters_per_dispatch;
    visDesc.type = rhi::BufferType::Structured;
    visDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    visDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess);
    visibility_buffer_ = device->CreateBuffer(visDesc);
    
    rhi::BufferDesc indirectDesc{};
    indirectDesc.size = sizeof(u32) * 4 * config.max_clusters_per_dispatch;
    indirectDesc.type = rhi::BufferType::Structured;
    indirectDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    indirectDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::IndirectArg);
    indirect_args_buffer_ = device->CreateBuffer(indirectDesc);
    
    rhi::TextureDesc hizDesc{};
    hizDesc.size = {2048, 2048, 1};
    hizDesc.mipLevels = 1;
    hizDesc.arraySize = 1;
    hizDesc.format = rhi::DataFormat::D32_Float;
    hizDesc.type = rhi::TextureType::Texture2D;
    hizDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    hizDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    hiz_buffer_ = device->CreateTexture(hizDesc);
    
    initialized_ = true;
    return true;
}

void GPUCullingPipeline::Shutdown() {
    if (!device_) return;
    
    if (cluster_bounds_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(cluster_bounds_buffer_);
        cluster_bounds_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (lod_data_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(lod_data_buffer_);
        lod_data_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (visibility_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(visibility_buffer_);
        visibility_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (indirect_args_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(indirect_args_buffer_);
        indirect_args_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    if (hiz_buffer_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(hiz_buffer_);
        hiz_buffer_ = rhi::handles::INVALID_RESOURCE;
    }
    
    device_ = nullptr;
    initialized_ = false;
}

bool GPUCullingPipeline::Execute(rhi::RHICommandBuffer* cmdBuffer,
                const RenderSceneSnapshot& snapshot,
                const math::m4x4& viewMatrix,
                const math::m4x4& projectionMatrix,
                NaniteStreamingManager* streamingManager,
                u32 frameIndex) {
    if (!initialized_) {
        std::cerr << "GPUCullingPipeline: Not initialized" << std::endl;
        return false;
    }
    
    math::m4x4 viewProjection = projectionMatrix * viewMatrix;
    
    if (!Stage1_FrustumCulling(cmdBuffer, snapshot, viewProjection, frameIndex)) {
        std::cerr << "GPUCullingPipeline: Stage 1 failed" << std::endl;
        return false;
    }
    
    if (config_.enable_occlusion_culling) {
        if (!Stage2_OcclusionCulling(cmdBuffer, snapshot, frameIndex)) {
            std::cerr << "GPUCullingPipeline: Stage 2 failed" << std::endl;
            return false;
        }
    }
    
    if (config_.enable_lod_selection) {
        if (!Stage3_LODSelection(cmdBuffer, snapshot, viewMatrix, math::v3{0, 0, 0}, frameIndex)) {
            std::cerr << "GPUCullingPipeline: Stage 3 failed" << std::endl;
            return false;
        }
    }
    
    if (config_.enable_instance_culling) {
        if (!Stage4_InstanceCulling(cmdBuffer, snapshot, frameIndex)) {
            std::cerr << "GPUCullingPipeline: Stage 4 failed" << std::endl;
            return false;
        }
    }
    
    if (streamingManager && config_.enable_streaming_feedback) {
        Stage5_StreamingFeedback(cmdBuffer, streamingManager, frameIndex);
    }
    
    CompactResults(cmdBuffer, frameIndex);
    
    results_.culled_cluster_count = snapshot.GetClusterRefCount() - results_.visible_cluster_count;
    
    return true;
}

bool GPUCullingPipeline::Stage1_FrustumCulling(rhi::RHICommandBuffer* cmdBuffer,
                                               const RenderSceneSnapshot& snapshot,
                                               const math::m4x4& viewProjection,
                                               u32 frameIndex) {
    results_.visible_cluster_count = snapshot.GetClusterRefCount();
    return true;
}

bool GPUCullingPipeline::Stage2_OcclusionCulling(rhi::RHICommandBuffer* cmdBuffer,
                                                 const RenderSceneSnapshot& snapshot,
                                                 u32 frameIndex) {
    return true;
}

bool GPUCullingPipeline::Stage3_LODSelection(rhi::RHICommandBuffer* cmdBuffer,
                                             const RenderSceneSnapshot& snapshot,
                                             const math::m4x4& viewMatrix,
                                             const math::v3& cameraPosition,
                                             u32 frameIndex) {
    return true;
}

bool GPUCullingPipeline::Stage4_InstanceCulling(rhi::RHICommandBuffer* cmdBuffer,
                                                const RenderSceneSnapshot& snapshot,
                                                u32 frameIndex) {
    results_.visible_instance_count = snapshot.GetInstanceCount();
    return true;
}

void GPUCullingPipeline::Stage5_StreamingFeedback(rhi::RHICommandBuffer* cmdBuffer,
                                                   NaniteStreamingManager* streamingManager,
                                                   u32 frameIndex) {
    if (streamingManager) {
        streamingManager->ProcessRequests(frameIndex);
    }
}

bool GPUCullingPipeline::CompactResults(rhi::RHICommandBuffer* cmdBuffer, u32 frameIndex) {
    results_.indirect_args_buffer = indirect_args_buffer_;
    results_.visibility_bitmask_buffer = visibility_buffer_;
    return true;
}

void GPUCullingPipeline::UpdateResults() {
}

}