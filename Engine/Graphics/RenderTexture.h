/**
 * @file RenderTexture.h
 * @brief RenderTexture 抽象
 * @details 作为ECS Texture资源的高层包装器，提供面向对象的纹理管理接口
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-09
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHI/Core/RHITypes.h"
#include "RHI/Core/RHIDevice.h"


namespace primal::graphics {

/**
 * @brief 渲染纹理类
 * @details 管理纹理资源，提供创建、更新和销毁接口，支持通过EntityID查找
 */
class RenderTexture {
public:
    // === 静态访问 ===
    
    /**
     * @brief 通过实体ID查找 RenderTexture 实例
     * @details 线程安全地从全局注册表中查找关联的纹理
     * @param entityId ECS实体ID
     * @return RenderTexture指针，如果未找到则返回nullptr
     */
    static RenderTexture* GetByEntityId(primal::id::id_type entityId);

    // === 构造与析构 ===
    
    RenderTexture();
    ~RenderTexture();

    // === 核心接口 ===

    /**
     * @brief 创建纹理资源
     * @param device RHI设备指针
     * @param entityId 关联的ECS实体ID
     * @param desc 纹理描述符（定义尺寸、格式、用途等）
     * @param initialData 初始数据指针（可选）
     * @param dataSize 初始数据大小（字节，可选）
     * @return 是否创建成功
     */
    bool Create(rhi::RHIDeviceBase* device, 
                primal::id::id_type entityId,
                const rhi::TextureDesc& desc,
                const void* initialData = nullptr, 
                u64 dataSize = 0);

    /**
     * @brief 异步上传数据到纹理
     * @details 使用 Staging Buffer 和 CommandBuffer 进行上传。当前实现会等待上传完成。
     * @param device RHI设备指针
     * @param data 数据指针
     * @param size 数据大小
     * @return 是否成功
     */
    bool UploadDataAsync(rhi::RHIDeviceBase* device, const void* data, u64 size);

    /**
     * @brief 生成 Mipmaps
     * @details 使用 Blit 操作生成所有 Mip 层级
     * @param device RHI设备指针
     * @return 是否成功
     */
    bool GenerateMipmaps(rhi::RHIDeviceBase* device);

    /**
     * @brief 从纹理回读数据到 CPU
     * @details 使用 Readback Buffer 和 CommandBuffer 回读数据
     * @param device RHI设备指针
     * @param data 接收数据的缓冲区指针
     * @param size 接收缓冲区大小
     * @return 是否成功
     */
    bool ReadBack(rhi::RHIDeviceBase* device, void* data, u64 size);

    /**
     * @brief 销毁纹理资源
     * @details 释放 RHI 资源并从注册表中注销
     * @param device RHI设备指针
     */
    void Destroy(rhi::RHIDeviceBase* device);

    // === 访问器 ===

    /**
     * @brief 获取 RHI 资源句柄
     * @return 资源句柄
     */
    rhi::ResourceHandle GetHandle() const { return textureHandle_; }

    /**
     * @brief 获取纹理描述符
     * @return 纹理描述符
     */
    const rhi::TextureDesc& GetDesc() const { return desc_; }

    /**
     * @brief 获取纹理宽度
     * @return 宽度（像素）
     */
    u32 GetWidth() const { return desc_.size.x; }

    /**
     * @brief 获取纹理高度
     * @return 高度（像素）
     */
    u32 GetHeight() const { return desc_.size.y; }

    /**
     * @brief 获取关联的实体ID
     * @return 实体ID
     */
    primal::id::id_type GetEntityId() const { return entityId_; }

    /**
     * @brief 检查资源是否有效
     * @return 是否有效
     */
    bool IsValid() const { return textureHandle_ != rhi::handles::INVALID_RESOURCE; }

private:
    // === 成员变量 ===
    
    rhi::ResourceHandle textureHandle_;     ///< RHI 纹理资源句柄
    rhi::TextureDesc desc_;                 ///< 纹理描述符副本
    primal::id::id_type entityId_;          ///< 关联的 ECS 实体 ID
    
    // === 静态注册表 ===
    
    static std::unordered_map<primal::id::id_type, RenderTexture*> registry_;
    static std::mutex registry_mutex_;

    // === 辅助函数 ===
    
    void Register();
    void Unregister();
};

} // namespace primal::graphics
