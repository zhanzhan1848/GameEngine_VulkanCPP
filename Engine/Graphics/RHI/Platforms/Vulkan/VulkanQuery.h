/**
 * @file VulkanQuery.h
 * @brief Vulkan 查询池实现
 * @details 封装 VkQueryPool,支持 Timestamp / Occlusion / PipelineStatistics。
 *          Vulkan 查询结果通过 vkGetQueryPoolResults 读取(支持 WAIT / PARTIAL bits)。
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#pragma once

#include "VulkanCommon.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class VulkanQueryPool {
public:
    VulkanQueryPool(VkDevice device, QueryType type, u32 count);
    ~VulkanQueryPool();

    VulkanQueryPool(const VulkanQueryPool&) = delete;
    VulkanQueryPool& operator=(const VulkanQueryPool&) = delete;

    VkQueryPool GetNativePool() const { return pool_; }
    QueryType GetType() const { return type_; }
    u32 GetCount() const { return count_; }

    /**
     * @brief 读取查询结果
     * @param firstQuery 起始 query 索引
     * @param queryCount 读取数量
     * @param data 输出缓冲
     * @param stride 每条结果的字节步长
     * @return true 全部就绪并写出;false 超时或参数非法
     *
     * @details 阻塞等待(WAIT bit),超时 1 秒;调用方按 stride 拷贝。
     *          Timestamp/PipelineStatistics:每条 u64;
     *          Occlusion:每条 u64(可见 sample 数)。
     */
    bool GetResults(u32 firstQuery, u32 queryCount, void* data, size_t stride);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkQueryPool pool_{VK_NULL_HANDLE};
    QueryType type_{QueryType::Timestamp};
    u32 count_{0};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
