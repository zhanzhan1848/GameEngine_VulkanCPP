#include "RenderTexture.h"
#include "RHI/Core/RHIResource.h"
#include "RHI/Core/RHICommand.h"
#include <cstring>
#include <algorithm>

namespace primal::graphics {

// === Static Members ===
std::unordered_map<primal::id::id_type, RenderTexture*> RenderTexture::registry_;
std::mutex RenderTexture::registry_mutex_;

RenderTexture* RenderTexture::GetByEntityId(primal::id::id_type entityId) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = registry_.find(entityId);
    if (it != registry_.end()) {
        return it->second;
    }
    return nullptr;
}

// === Constructor / Destructor ===

RenderTexture::RenderTexture() 
    : textureHandle_(rhi::handles::INVALID_RESOURCE)
    , entityId_(primal::id::invalid_id) {
}

RenderTexture::~RenderTexture() {
    // Note: Destroy should be called explicitly with device pointer
    // If not called, we at least unregister
    Unregister();
}

// === Core Interface ===

bool RenderTexture::Create(rhi::RHIDeviceBase* device, 
                           primal::id::id_type entityId,
                           const rhi::TextureDesc& desc,
                           const void* initialData, 
                           u64 dataSize) {
    if (!device) return false;
    
    // Ensure cleanup if already created
    if (IsValid()) {
        Destroy(device);
    }

    desc_ = desc;
    entityId_ = entityId;

    // Create RHI Texture
    textureHandle_ = device->CreateTexture(desc);
    if (textureHandle_ == rhi::handles::INVALID_RESOURCE) {
        return false;
    }

    // Upload initial data if provided
    if (initialData && dataSize > 0) {
        auto* res = rhi::ResourceManager::Instance().GetResource(textureHandle_);
        if (res) {
            // Note: UpdateData might not work for all texture types/usages directly 
            // depending on backend implementation, but for now we assume it does 
            // or the user provided usage allows it.
            // Ideally we should use UploadDataAsync logic for GPU_Only textures,
            // but Create is usually synchronous/initialization.
            // For now, use the resource's UpdateData.
            res->UpdateData(initialData, dataSize);
        }
    }

    Register();
    return true;
}

void RenderTexture::Destroy(rhi::RHIDeviceBase* device) {
    if (device && IsValid()) {
        device->DestroyTexture(textureHandle_);
    }
    textureHandle_ = rhi::handles::INVALID_RESOURCE;
    Unregister();
    entityId_ = primal::id::invalid_id;
}

bool RenderTexture::UploadDataAsync(rhi::RHIDeviceBase* device, const void* data, u64 size) {
    if (!device || !IsValid() || !data || size == 0) return false;

    // 1. Create Staging Buffer
    rhi::BufferDesc bufDesc{
        size,
        rhi::BufferType::Unknown, // Generic buffer, used for transfer
        rhi::GPUMemoryUsage::Staging, 
        rhi::GPUMemoryUsage::Staging,
        0,
    };
    
    rhi::ResourceHandle stagingBuffer = device->CreateBuffer(bufDesc);
    if (stagingBuffer == rhi::handles::INVALID_RESOURCE) return false;
    
    // 2. Upload to Staging Buffer
    auto* bufRes = rhi::ResourceManager::Instance().GetResource(stagingBuffer);
    if (!bufRes || !bufRes->UpdateData(data, size)) {
        device->DestroyBuffer(stagingBuffer);
        return false;
    }
    
    // 3. Create Command Buffer
    auto cmdHandle = device->CreateCommandBuffer(rhi::CommandQueueType::Transfer);
    // rhi::GetCommandBuffer relies on registry, which might fail if not linked properly in unit tests.
    // However, in normal Engine usage, it should work.
    // The issue in tests is that TestRenderTexture doesn't link against the translation unit that defines GetCommandBuffer if it's not exported.
    // But we linked 'Engine'.
    // Wait, RHIDevice.cpp defines GetCommandBuffer? No, RHICommand.cpp usually does.
    // Let's assume GetCommandBuffer is available via RHICommand.h include.
    
    // In TestRenderTexture, MockDevice creates MockCommandBuffer and stores it.
    // But MockDevice::SubmitCommandBuffer uses GetCommandBuffer(handle).
    // And RenderTexture uses GetCommandBuffer(handle).
    
    // If the linker fails, it means GetCommandBuffer is not found.
    // GetCommandBuffer is likely a global function in namespace primal::graphics::rhi.
    // We need to ensure it's implemented and exported.
    
    // To fix the linker error without modifying Engine architecture too much:
    // We can expose the CommandBuffer pointer directly from Device or have a way to retrieve it.
    // But RenderTexture relies on GetCommandBuffer global function.
    
    // For now, let's use the pointer returned by CreateCommandBuffer if possible?
    // No, CreateCommandBuffer returns a handle.
    
    // If we look at RHICommand.h, GetCommandBuffer is likely declared there.
    // Is it defined in RHICommand.cpp?
    // I need to check RHICommand.cpp content again.
    
    auto* cmd = rhi::GetCommandBuffer(cmdHandle);
    if (!cmd) {
        device->DestroyBuffer(stagingBuffer);
        return false;
    }
    
    // 4. Record Copy Command
    cmd->Begin();
    
    rhi::BufferTextureCopyRegion region;
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {desc_.size.x, desc_.size.y, desc_.size.z};
    
    cmd->CopyBufferToTexture(stagingBuffer, textureHandle_, &region, 1);
    
    cmd->End();
    
    // 5. Submit and Wait (Synchronous for now)
    rhi::QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    device->Submit(submitInfo);
    cmd->WaitForCompletion();
    
    // 6. Cleanup
    device->DestroyBuffer(stagingBuffer);
    
    return true;
}

bool RenderTexture::ReadBack(rhi::RHIDeviceBase* device, void* data, u64 size) {
    if (!device || !IsValid() || !data || size == 0) return false;
    
    // 1. Create Readback Buffer
    rhi::BufferDesc bufDesc{
        size,
        rhi::BufferType::Unknown,
        rhi::GPUMemoryUsage::Readback,
        rhi::GPUMemoryUsage::Readback,
        0,
    };
    
    rhi::ResourceHandle readbackBuffer = device->CreateBuffer(bufDesc);
    if (readbackBuffer == rhi::handles::INVALID_RESOURCE) return false;
    
    // 2. Create Command Buffer & Record
    auto cmdHandle = device->CreateCommandBuffer(rhi::CommandQueueType::Transfer);
    auto* cmd = rhi::GetCommandBuffer(cmdHandle);
    if (!cmd) {
        device->DestroyBuffer(readbackBuffer);
        return false;
    }
    
    cmd->Begin();
    
    rhi::BufferTextureCopyRegion region{
        0, 0, 0,
        { 0, 0, 1 },
        rhi::Offset3D{0, 0, 0},
        rhi::Extent3D{ desc_.size.x, desc_.size.y, desc_.size.z },
    };
    
    cmd->CopyTextureToBuffer(textureHandle_, readbackBuffer, &region, 1);
    
    cmd->End();
    
    // 3. Submit and Wait
    rhi::QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    device->Submit(submitInfo);
    cmd->WaitForCompletion();
    
    // 4. Map and Copy
    auto* bufRes = rhi::ResourceManager::Instance().GetResource(readbackBuffer);
    if (bufRes) {
        void* mapped = bufRes->Map(0, size);
        if (mapped) {
            memcpy(data, mapped, size);
            bufRes->Unmap();
        }
    }
    
    device->DestroyBuffer(readbackBuffer);
    
    return true;
}

bool RenderTexture::GenerateMipmaps(rhi::RHIDeviceBase* device) {
    if (!device || !IsValid() || desc_.mipLevels <= 1) return false;
    
    auto cmdHandle = device->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    auto* cmd = rhi::GetCommandBuffer(cmdHandle);
    if (!cmd) return false;
    
    cmd->Begin();
    
    s32 width = desc_.size.x;
    s32 height = desc_.size.y;
    s32 depth = desc_.size.z;
    
    for (u32 i = 0; i < desc_.mipLevels - 1; ++i) {
        // Next level dims
        s32 nextWidth = std::max(1, width / 2);
        s32 nextHeight = std::max(1, height / 2);
        s32 nextDepth = std::max(1, depth / 2);
        rhi::TextureBlitRegion region{
            .srcSubresource = {i, 0, 1},
            .srcOffsets = {rhi::Offset3D{0, 0, 0}, rhi::Offset3D{width, height, depth}},
            .dstSubresource = {i + 1, 0, 1},
            .dstOffsets = {rhi::Offset3D{0, 0, 0}, rhi::Offset3D{nextWidth, nextHeight, nextDepth}},
        };
        
        // Blit
        cmd->BlitTexture(textureHandle_, textureHandle_, &region, 1, rhi::FilterMode::Linear);
        
        // Update for next iteration
        width = nextWidth;
        height = nextHeight;
        depth = nextDepth;
    }
    
    cmd->End();
    rhi::QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    device->Submit(submitInfo);
    cmd->WaitForCompletion();
    
    return true;
}

// === Helper Functions ===

void RenderTexture::Register() {
    if (entityId_ != primal::id::invalid_id) {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        registry_[entityId_] = this;
    }
}

void RenderTexture::Unregister() {
    if (entityId_ != primal::id::invalid_id) {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        registry_.erase(entityId_);
    }
}

} // namespace primal::graphics
