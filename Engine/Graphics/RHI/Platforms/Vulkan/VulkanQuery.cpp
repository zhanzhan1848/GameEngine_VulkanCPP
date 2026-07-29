/**
 * @file VulkanQuery.cpp
 * @brief VulkanQueryPool 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanQuery.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include <iostream>
#include <cstring>

namespace primal::graphics::rhi {

VulkanQueryPool::VulkanQueryPool(VkDevice device, QueryType type, u32 count)
    : device_(device), type_(type), count_(count) {
    if (!device || count == 0) return;

    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.flags = 0;
    ci.queryCount = count;

    switch (type) {
        case QueryType::Timestamp:
            ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            // TIMESTAMP bits 由 CommandBuffer::WriteTimestamp 设
            break;
        case QueryType::Occlusion:
            ci.queryType = VK_QUERY_TYPE_OCCLUSION;
            break;
        case QueryType::PipelineStatistics:
            ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            // 全 bits 用于通用性;具体某条统计结果按 stride 解析
            ci.pipelineStatistics =
                  VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
                | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
                | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
                | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
                | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT
                | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT
                | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
            break;
        default:
            std::cerr << "[VulkanQueryPool] Unknown query type" << std::endl;
            return;
    }

    if (vkCreateQueryPool(device_, &ci, nullptr, &pool_) != VK_SUCCESS) {
        std::cerr << "[VulkanQueryPool] vkCreateQueryPool failed" << std::endl;
        pool_ = VK_NULL_HANDLE;
    }
}

VulkanQueryPool::~VulkanQueryPool() {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
}

bool VulkanQueryPool::GetResults(u32 firstQuery, u32 queryCount, void* data, size_t stride) {
    if (!pool_ || !data || firstQuery + queryCount > count_) return false;

    // 每条结果大小:Timestamp/Occlusion = u64;PipelineStatistics 默认按 u64 取每条
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT;

    VkResult res = vkGetQueryPoolResults(
        device_, pool_, firstQuery, queryCount,
        queryCount * stride,  // dataSize:严格按 stride 算
        data, stride,
        flags);

    if (res == VK_SUCCESS) return true;
    if (res == VK_NOT_READY) {
        // 不应发生(WAIT bit 已设),但兜底
        return false;
    }
    if (res == VK_ERROR_DEVICE_LOST) {
        std::cerr << "[VulkanQueryPool] DEVICE_LOST during GetResults" << std::endl;
        return false;
    }
    std::cerr << "[VulkanQueryPool] vkGetQueryPoolResults returned " << res << std::endl;
    return false;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
