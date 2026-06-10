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
#include "RHI/Core/RHIMeshAsset.h"
#include "Content/ContentToEngine.h"

#include <iostream>
#include <cstring>

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

RenderMesh* RenderMesh::CreateFromAsset(rhi::RHIDeviceBase* device,
                                         primal::id::id_type geometry_content_id) {
    if (!device || geometry_content_id == primal::id::invalid_id) return nullptr;

    // Already created?
    if (auto* existing = GetByEntityId(geometry_content_id)) {
        return existing;
    }

    // geometry_content_id is a geometry_hierarchies ID (from create_resource).
    // Extract the internal rhi_mesh_assets ID to fetch the actual asset data.
    const primal::id::id_type rhi_id = content::get_rhi_mesh_id(geometry_content_id);

    // Fetch RHIMeshAsset from content system
    rhi::RHIMeshAsset asset;
    if (!content::get_rhi_mesh_asset(rhi_id, asset)) {
        std::cerr << "RenderMesh::CreateFromAsset: Failed to get RHIMeshAsset for geometry_id "
                  << geometry_content_id << " (rhi_id=" << rhi_id << ")" << std::endl;
        return nullptr;
    }

    if (asset.num_vertices == 0 || asset.position_buffer.empty()) {
        std::cerr << "RenderMesh::CreateFromAsset: Empty asset for geometry_id "
                  << geometry_content_id << std::endl;
        return nullptr;
    }

    // Interleave position (12B) + element (20B) → 32B stride
    constexpr u32 TARGET_ELEMENT_SIZE = 20;
    constexpr u32 TARGET_VERTEX_STRIDE = 12 + TARGET_ELEMENT_SIZE;
    const u32 vertexCount = asset.num_vertices;

    const u8* posPtr = asset.position_buffer.data();
    const u32 srcPosStride = 12; // RHIMeshAsset stores packed float3

    const u8* elemPtr = asset.element_buffer.empty() ? nullptr : asset.element_buffer.data();
    const u32 srcElemStride = elemPtr ? (u32)(asset.element_buffer.size() / vertexCount) : 0;

    utl::vector<u8> interleaved(vertexCount * TARGET_VERTEX_STRIDE);
    memset(interleaved.data(), 0, interleaved.size());

    for (u32 v = 0; v < vertexCount; ++v) {
        u8* dst = interleaved.data() + v * TARGET_VERTEX_STRIDE;

        // Position: always 12 bytes
        memcpy(dst, posPtr + v * srcPosStride, 12);

        // Elements
        if (elemPtr && srcElemStride > 0) {
            u8* dstElem = dst + 12;
            const u8* srcElem = elemPtr + v * srcElemStride;

            if (srcElemStride >= 24) {
                // Padded format: [Normal+Tangent 12B] [Pad 4B] [UV 8B]
                memcpy(dstElem, srcElem, 12);
                memcpy(dstElem + 12, srcElem + 16, 8);
            } else {
                u32 copySize = std::min(srcElemStride, TARGET_ELEMENT_SIZE);
                memcpy(dstElem, srcElem, copySize);
            }
        }
    }

    // Index data
    const void* idxPtr = asset.index_buffer.empty() ? nullptr : asset.index_buffer.data();
    const u32 indexCount = asset.num_indices;
    rhi::DataIndexType idxType = (asset.index_size == 2)
        ? rhi::DataIndexType::UInt16
        : rhi::DataIndexType::UInt32;

    RenderMesh* mesh = new RenderMesh();
    if (!mesh->Create(device, geometry_content_id,
                      interleaved.data(), vertexCount, TARGET_VERTEX_STRIDE,
                      idxPtr, indexCount, idxType)) {
        delete mesh;
        return nullptr;
    }

    return mesh;
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
            const void* vertices, u32 vertexCount, u32 vertexStride,
            const void* indices, u32 indexCount, 
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

    // 计算AABB
    localAABB_ = rhi::AABB(); // 重置为无效
    const u8* vertexData = static_cast<const u8*>(vertices);
    for (u32 i = 0; i < vertexCount; ++i) {
        // 假设前3个float是位置
        const float* pos = reinterpret_cast<const float*>(vertexData + i * vertexStride);
        localAABB_.Expand(rhi::math::v3{pos[0], pos[1], pos[2]});
    }

    // 创建顶点缓冲区
    // Metal要求缓冲区大小必须是256字节对齐，否则可能会导致访问越界或性能问题
    u64 vertexBufferSize = static_cast<u64>(vertexCount) * vertexStride;
    u64 alignedVertexSize = (vertexBufferSize + 255) & ~255;
    vertexBuffer_ = CreateBuffer(device, vertices, alignedVertexSize, rhi::BufferType::Vertex);
    
    if (vertexBuffer_ != rhi::handles::INVALID_RESOURCE) {
        // 更新缓冲区数据
        // 注意：只更新实际数据大小，保留对齐填充部分的未初始化状态
        rhi::RHIResource* resource = rhi::ResourceManager::Instance().GetResource(vertexBuffer_);
        if (resource) {
            resource->UpdateData(vertices, vertexBufferSize);
        }
    } else {
        return false;
    }

    // 创建索引缓冲区（可选）
    if (indices && indexCount > 0) {
        u64 indexStride = (indexType == rhi::DataIndexType::UInt32) ? 4 : 2;
        u64 indexBufferSize = static_cast<u64>(indexCount) * indexStride;
        
        // 对齐缓冲区大小到256字节，符合Metal最佳实践并避免越界警告
        u64 alignedSize = (indexBufferSize + 255) & ~255;

        rhi::BufferDesc desc{
            .size = alignedSize,
            .type = rhi::BufferType::Index,
            .usage = rhi::GPUMemoryUsage::Dynamic, // 使用动态内存以便调试
            .memoryUsage = rhi::GPUMemoryUsage::Dynamic,
            .bindFlags = static_cast<u32>(rhi::ResourceUsage::IndexBuffer) | static_cast<u32>(rhi::ResourceUsage::CopyDest),
        };
        
        indexBuffer_ = device->CreateBuffer(desc);
        
        if (indexBuffer_ != rhi::handles::INVALID_RESOURCE) {
            rhi::RHIResource* resource = rhi::ResourceManager::Instance().GetResource(indexBuffer_);
            if (resource) {
                resource->UpdateData(indices, indexBufferSize);
            }
        }
        
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

void RenderMesh::SetEntityId(primal::id::id_type id) {
    if (entityId_ == id) return;
    Unregister();
    entityId_ = id;
    Register();
}

void RenderMesh::Draw(rhi::RHICommandBuffer* cmdBuffer, u32 instanceCount, u32 startInstance, u32 bindingSlot) {
    if (!cmdBuffer || !IsValid()) {
        if (!IsValid()) {
            std::cerr << "RenderMesh::Draw Error: Mesh is invalid! VBuffer: " << vertexBuffer_ << ", Count: " << vertexCount_ << std::endl;
        }
        return;
    }

    // 绑定顶点缓冲区
    rhi::ResourceHandle buffers[] = { vertexBuffer_ };
    u64 offsets[] = { 0 };
    cmdBuffer->BindVertexBuffers(bindingSlot, 1, buffers, offsets);

    // Debug: Print Draw Info once
    static bool printed = false;
    if (!printed && vertexCount_ > 0) {
        std::cout << "RenderMesh::Draw - Binding Vertex Buffer " << vertexBuffer_ << " to slot " << bindingSlot << " Offset 0" << std::endl;
        std::cout << "RenderMesh::Draw - Drawing " << (indexBuffer_ != rhi::handles::INVALID_RESOURCE ? indexCount_ : vertexCount_) << " primitives." << std::endl;
        printed = true;
    }

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

rhi::ResourceHandle RenderMesh::CreateBuffer(rhi::RHIDeviceBase* device, const void* data, u64 size, rhi::BufferType type) {
    rhi::BufferDesc desc{
        .size = size,
        .type = type,
        // 优先使用静态内存以获得最佳性能
        .usage = rhi::GPUMemoryUsage::Static,
        .memoryUsage = rhi::GPUMemoryUsage::Static,
    };
    
    // 设置绑定标志
    desc.bindFlags = (type == rhi::BufferType::Vertex) 
        ? static_cast<u32>(rhi::ResourceUsage::VertexBuffer) 
        : static_cast<u32>(rhi::ResourceUsage::IndexBuffer);
    
    // 添加复制目标标志，以便上传数据
    desc.bindFlags |= static_cast<u32>(rhi::ResourceUsage::CopyDest);

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
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
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
