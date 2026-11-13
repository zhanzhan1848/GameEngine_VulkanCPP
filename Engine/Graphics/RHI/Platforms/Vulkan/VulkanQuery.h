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
     * @return true 全部就绪并写出；false 表示结果不可用或参数/设备错误，不能当零耗时
     *
     * @details 默认非阻塞 availability 读（R11 taskB：不带 WAIT bit——原生
     *          NVIDIA 驱动对不可用 query 的 WAIT_BIT 读无 timeout 无限阻塞，
     *          Windows 第 2 帧死锁根因；MoltenVK 本就按 NOT_READY 返回，
     *          返回值行为不变，像素输出实测无漂移）。VK_NOT_READY 时返回
     *          false（不可用数据）。
     *          实验环境变量 PRIMAL_VK_QUERY_NONBLOCKING=1 保留：查询侧现与
     *          默认一致，RenderGraph 侧仍按它门控暂存数据发布。
     *          非阻塞读取不可代替 frame fence 或授权任何 host 资源复用。
     *          Timestamp/PipelineStatistics:每条 u64;
     *          Occlusion:每条 u64(可见 sample 数)。
     */
    bool GetResults(u32 firstQuery, u32 queryCount, void* data, size_t stride);

    /**
     * @brief 重置查询池中的查询（vkResetQueryPool，CPU 端）
     * @param firstQuery 起始 query 索引
     * @param queryCount 重置数量
     * @details T4.6.5 part 24.2 (B6 fix): Vulkan spec 要求 query 创建后 + 每次
     *          复用前必须 reset。CPU 端 reset 要求 GPU 不再使用该池（本引擎
     *          submit+wait 后调用安全）。
     */
    void Reset(u32 firstQuery, u32 queryCount);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkQueryPool pool_{VK_NULL_HANDLE};
    QueryType type_{QueryType::Timestamp};
    u32 count_{0};
};

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
