/**
 * @file VulkanBuffer.h
 * @brief Vulkan RHI 缓冲区实现
 * @details 封装 VkBuffer + VmaAllocation,实现 RHIResource 接口。
 *          生命周期:createBufferImpl → 构造(空)+ Initialize(VMA 分配)→ destroyImpl(GC 延迟 vmaDestroyBuffer)。
 *          映射语义:Dynamic/Staging/Readback 走 HOST_VISIBLE,VMA persistent-map 优化;
 *                   Static/Immutable 走 DEVICE_LOCAL,UpdateData 走 staging 路径。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHIResource.h"

namespace primal::graphics::rhi {

class VulkanDevice;

class VulkanBuffer : public RHIResource {
    friend class VulkanDevice;
public:
    VulkanBuffer(VulkanDevice& device, const BufferDesc& desc);
    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    virtual ~VulkanBuffer();

    bool Initialize() override;

    VkBuffer GetNativeBuffer() const { return vkBuffer_; }
    VmaAllocation GetAllocation() const { return allocation_; }

protected:
    // === RHIResource 接口 ===
    void destroyImpl() override;
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;

private:
    VkBuffer vkBuffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{nullptr};

    // 持久映射的 CPU 指针(若 VMA 启用 persistent mapping)
    void* persistentMappedPtr_{nullptr};

    // 构造时从 BufferDesc 翻译并缓存;Initialize() 时用。
    // 不存原始 BufferType 是因为 RHIResource 把它压平成 ResourceType::Buffer。
    VkBufferUsageFlags vkUsageFlags_{0};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
