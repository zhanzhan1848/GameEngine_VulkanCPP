/**
 * @file RenderMesh.h
 * @brief 渲染网格抽象
 * @details 作为ECS GPUBufferComponent的高层包装器，提供面向对象的网格管理接口
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-08
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "Engine/Common/Id.h"
#include "RHI/Core/RHITypes.h"
#include "RHI/Core/RHIDevice.h"
#include "RHI/Core/RHICommand.h"
#include "RHI/Core/RHIGeometry.h"
#include <unordered_map>
#include <mutex>

namespace primal::graphics {

/**
 * @brief 渲染网格类
 * @details 管理顶点缓冲区和索引缓冲区，提供创建、更新和绘制接口
 */
class RenderMesh {
public:
    /**
     * @brief 通过实体ID查找RenderMesh
     * @param entityId 实体ID
     * @return RenderMesh指针，如果未找到则返回nullptr
     */
    static RenderMesh* GetByEntityId(primal::id::id_type entityId);

    /**
     * @brief 从 content 系统创建 RenderMesh
     * @details 通过 geometry_hierarchies ID 提取 RHIMeshAsset → 交错 position+element 到 32B stride → 创建 GPU 缓冲区
     * @param device RHI设备指针
     * @param geometry_content_id content 系统中的 geometry ID (create_resource 返回值)
     * @return 新创建的 RenderMesh 指针 (调用者拥有所有权)，失败返回 nullptr
     */
    static RenderMesh* CreateFromAsset(rhi::RHIDeviceBase* device,
                                       primal::id::id_type geometry_content_id);

    /**
     * @brief 构造函数
     */
    RenderMesh();

    /**
     * @brief 析构函数
     */
    ~RenderMesh();

    /**
     * @brief 创建网格资源
     * @param device RHI设备指针
     * @param entityId 关联的ECS实体ID
     * @param vertices 顶点数据指针
     * @param vertexCount 顶点数量
     * @param vertexStride 顶点步长（字节）
     * @param indices 索引数据指针（可选）
     * @param indexCount 索引数量（可选）
     * @param indexType 索引类型（可选，默认Unknown）
     * @return 是否创建成功
     */
    bool Create(rhi::RHIDeviceBase* device, 
                primal::id::id_type entityId,
                const void* vertices, u32 vertexCount, u32 vertexStride,
                const void* indices = nullptr, u32 indexCount = 0, 
                rhi::DataIndexType indexType = rhi::DataIndexType::Unknown);

    /**
     * @brief 销毁网格资源
     * @param device RHI设备指针
     */
    void Destroy(rhi::RHIDeviceBase* device);

    /**
     * @brief 绘制网格
     * @param cmdBuffer 命令缓冲区指针
     * @param instanceCount 实例数量（默认1）
     * @param startInstance 起始实例（默认0）
     * @param bindingSlot 顶点缓冲区绑定槽位（默认0）
     */
    void Draw(rhi::RHICommandBuffer* cmdBuffer, u32 instanceCount = 1, u32 startInstance = 0, u32 bindingSlot = 0);

    /**
     * @brief 设置实体ID并更新注册表
     * @param id 新的实体ID
     */
    void SetEntityId(primal::id::id_type id);

    /**
     * @brief 获取顶点缓冲区句柄
     * @return 顶点缓冲区句柄
     */
    rhi::ResourceHandle GetVertexBuffer() const { return vertexBuffer_; }

    /**
     * @brief 获取索引缓冲区句柄
     * @return 索引缓冲区句柄
     */
    rhi::ResourceHandle GetIndexBuffer() const { return indexBuffer_; }

    /**
     * @brief 获取索引类型
     */
    rhi::DataIndexType GetIndexType() const { return indexType_; }


    /**
     * @brief 获取顶点数量
     * @return 顶点数量
     */
    u32 GetVertexCount() const { return vertexCount_; }

    /**
     * @brief 获取顶点步长
     * @return 顶点步长（字节）
     */
    u32 GetVertexStride() const { return vertexStride_; }

    /**
     * @brief 获取索引数量
     * @return 索引数量
     */
    u32 GetIndexCount() const { return indexCount_; }

    /**
     * @brief 获取局部包围盒
     * @return 局部AABB
     */
    const rhi::AABB& GetLocalAABB() const { return localAABB_; }

    /**
     * @brief 获取关联的实体ID
     * @return 实体ID
     */
    primal::id::id_type GetEntityId() const { return entityId_; }

    /**
     * @brief 检查是否有效
     * @return 是否包含有效的GPU资源
     */
    bool IsValid() const { return vertexBuffer_ != rhi::handles::INVALID_RESOURCE; }

private:
    /**
     * @brief 创建缓冲区辅助函数
     * @param device 设备指针
     * @param data 数据指针
     * @param size 数据大小
     * @param type 缓冲区类型
     * @return 资源句柄
     */
    rhi::ResourceHandle CreateBuffer(rhi::RHIDeviceBase* device, const void* data, u64 size, rhi::BufferType type);

    friend class RenderMeshTestHelper;

    rhi::ResourceHandle vertexBuffer_;      ///< 顶点缓冲区句柄
    rhi::ResourceHandle indexBuffer_;       ///< 索引缓冲区句柄
    primal::id::id_type entityId_;          ///< 关联的ECS实体ID
    u32 vertexCount_;                  ///< 顶点数量
    u32 indexCount_;                   ///< 索引数量
    u32 vertexStride_;                 ///< 顶点步长
    rhi::DataIndexType indexType_;          ///< 索引类型
    rhi::AABB localAABB_;                   ///< 局部坐标系下的包围盒

    // 静态注册表
    static std::unordered_map<primal::id::id_type, RenderMesh*> registry_;
    static std::mutex registry_mutex_;
    
    // 注册/注销辅助函数
    void Register();
    void Unregister();
};

} // namespace primal::graphics
