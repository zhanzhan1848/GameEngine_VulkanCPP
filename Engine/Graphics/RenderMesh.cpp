/**
 * @file RenderMesh.cpp
 * @brief 渲染网格实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-08
 * @version 0.1.0
 */

#include "RenderMesh.h"
#include "RHI/Core/RHIResource.h"
#include "RHI/Core/RHICommand.h"

#include <iostream>

namespace primal::graphics {

// 静态成员定义
std::unordered_map<primal::id::id_type, RenderMesh*> RenderMesh::registry_;
std::mutex RenderMesh::registry_mutex_;

RenderMesh* RenderMesh::GetByEntityId(primal::id::id_type entityId) {
    if (entityId == primal::id::invalid_id) return nullptr;
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = registry_.find(entityId);
    if (it != registry_.end()) {
        return it->second;
    }
    return nullptr;
}

// 注册当前 Mesh 到全局表，以便通过 EntityID 查找
void RenderMesh::Register() {
    if (entityId_ != primal::id::invalid_id) {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        registry_[entityId_] = this;
    }
}

// 从全局表中移除当前 Mesh
void RenderMesh::Unregister() {
    if (entityId_ != primal::id::invalid_id) {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = registry_.find(entityId_);
        if (it != registry_.end() && it->second == this) {
            registry_.erase(it);
        }
    }
}

RenderMesh::RenderMesh() 
    : vertexBuffer_(rhi::handles::INVALID_RESOURCE)
    , indexBuffer_(rhi::handles::INVALID_RESOURCE)
    , entityId_(primal::id::invalid_id)
    , vertexCount_(0)
    , indexCount_(0)
    , vertexStride_(0)
    , indexType_(rhi::DataIndexType::Unknown)
{}

RenderMesh::~RenderMesh() {
    Unregister();
    // 析构函数不自动销毁资源，因为需要device指针
    // 用户必须显式调用Destroy
}

bool RenderMesh::Create(rhi::RHIDeviceBase* device, 
            primal::id::id_type entityId,
            const void* vertices, uint32_t vertexCount, uint32_t vertexStride,
            const void* indices, uint32_t indexCount, 
            rhi::DataIndexType indexType) {

    if (!device || !vertices || vertexCount == 0 || vertexStride == 0) {
        return false;
    }

    if (IsValid()) {
        Destroy(device);
    }

    entityId_ = entityId;
    vertexCount_ = vertexCount;
    vertexStride_ = vertexStride;
    indexCount_ = indexCount;
    indexType_ = indexType;

    // 创建顶点缓冲区
    uint64_t vertexBufferSize = static_cast<uint64_t>(vertexCount) * vertexStride;
    vertexBuffer_ = CreateBuffer(device, vertices, vertexBufferSize, rhi::BufferType::Vertex);
    
    if (vertexBuffer_ == rhi::handles::INVALID_RESOURCE) {
        return false;
    }

    // 创建索引缓冲区（可选）
    if (indices && indexCount > 0) {
        uint64_t indexStride = (indexType == rhi::DataIndexType::UInt32) ? 4 : 2;
        uint64_t indexBufferSize = static_cast<uint64_t>(indexCount) * indexStride;
        indexBuffer_ = CreateBuffer(device, indices, indexBufferSize, rhi::BufferType::Index);
        
        if (indexBuffer_ == rhi::handles::INVALID_RESOURCE) {
            Destroy(device);
            return false;
        }
    }

    Register();

    return true;
}

void RenderMesh::Destroy(rhi::RHIDeviceBase* device) {
    if (device) {
        if (vertexBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device->DestroyBuffer(vertexBuffer_);
            vertexBuffer_ = rhi::handles::INVALID_RESOURCE;
        }
        if (indexBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device->DestroyBuffer(indexBuffer_);
            indexBuffer_ = rhi::handles::INVALID_RESOURCE;
        }
    }
    Unregister();
}

void RenderMesh::Draw(rhi::RHICommandBuffer* cmdBuffer, uint32_t instanceCount, uint32_t startInstance) {
    if (!cmdBuffer || !IsValid()) return;

    // 绑定顶点缓冲区
    rhi::ResourceHandle buffers[] = { vertexBuffer_ };
    uint64_t offsets[] = { 0 };
    cmdBuffer->BindVertexBuffers(0, 1, buffers, offsets);

    if (indexBuffer_ != rhi::handles::INVALID_RESOURCE && indexCount_ > 0) {
        // 绑定索引缓冲区
        rhi::DataFormat indexFormat = (indexType_ == rhi::DataIndexType::UInt32) 
            ? rhi::DataFormat::R32_UInt 
            : rhi::DataFormat::R16_UInt;
            
        cmdBuffer->BindIndexBuffer(indexBuffer_, indexFormat, 0);
        
        // 索引绘制
        cmdBuffer->DrawIndexed(indexCount_, 0, 0, instanceCount, startInstance);
    } else {
        // 顶点绘制
        cmdBuffer->Draw(vertexCount_, 0, instanceCount, startInstance);
    }
}

rhi::ResourceHandle RenderMesh::CreateBuffer(rhi::RHIDeviceBase* device, const void* data, uint64_t size, rhi::BufferType type) {
    rhi::BufferDesc desc;
    desc.size = size;
    desc.type = type;
    // 优先使用静态内存以获得最佳性能
    desc.usage = rhi::GPUMemoryUsage::Static;
    
    // 设置绑定标志
    desc.bindFlags = (type == rhi::BufferType::Vertex) 
        ? static_cast<uint32_t>(rhi::ResourceUsage::VertexBuffer) 
        : static_cast<uint32_t>(rhi::ResourceUsage::IndexBuffer);
    
    // 添加复制目标标志，以便上传数据
    desc.bindFlags |= static_cast<uint32_t>(rhi::ResourceUsage::CopyDest);

    rhi::ResourceHandle handle = device->CreateBuffer(desc);
    
    if (handle != rhi::handles::INVALID_RESOURCE && data) {
        rhi::RHIResource* resource = rhi::ResourceManager::Instance().GetResource(handle);
        if (resource) {
            // 尝试更新数据
            // 注意：对于Static内存，UpdateData可能会失败，取决于具体实现是否支持内部暂存
            if (!resource->UpdateData(data, size)) {
                // 如果直接更新失败（例如因为是Static内存且未实现内部暂存），则回退到Dynamic内存
                // 这是一个简化的处理，生产环境应该使用显式的Staging Buffer
                device->DestroyBuffer(handle);
                
                desc.usage = rhi::GPUMemoryUsage::Dynamic;
                handle = device->CreateBuffer(desc);
                
                if (handle != rhi::handles::INVALID_RESOURCE) {
                    resource = rhi::ResourceManager::Instance().GetResource(handle);
                    if (resource) {
                        resource->UpdateData(data, size);
                    }
                }
            }
        }
    }
    
    return handle;
}

} // namespace primal::graphics
