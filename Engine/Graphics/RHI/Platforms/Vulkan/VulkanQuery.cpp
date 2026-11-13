/**
 * @file VulkanQuery.cpp
 * @brief VulkanQueryPool 实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-26
 */

#include "VulkanQuery.h"

#include "Engine/Common/CoreDiag.h"
#include "Engine/Common/CoreLogStream.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "VulkanHangDiag.h"

#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <thread>

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
            PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanQueryPool] Unknown query type" << std::endl;
            return;
    }

    if (vkCreateQueryPool(device_, &ci, nullptr, &pool_) != VK_SUCCESS) {
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanQueryPool] vkCreateQueryPool failed" << std::endl;
        pool_ = VK_NULL_HANDLE;
        return;
    }
    // T4.6.5 part 24.2 (B6 fix): Vulkan spec requires queries to be reset
    // before first use. CPU-side vkResetQueryPool needs the hostQueryReset
    // device feature enabled (VUID-vkResetQueryPool-None-02665) which the
    // engine does NOT currently enable. Use GPU-side vkCmdResetQueryPool via
    // RenderGraph's per-frame cmd buffer reset instead — works without the
    // feature and is more reliable on MoltenVK.
}

void VulkanQueryPool::Reset(u32 firstQuery, u32 queryCount) {
    if (!pool_ || firstQuery + queryCount > count_) return;
    // NOTE: requires hostQueryReset feature. Not currently enabled — left for
    // future use if CPU-side reset is needed.
    vkResetQueryPool(device_, pool_, firstQuery, queryCount);
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
    // Experiment only: absence of a result is not zero GPU work and must not
    // authorize host reuse of any frame slot or resource. Callers must use the
    // returned boolean to distinguish unavailable timestamp data.
    const char* nonblocking = std::getenv("PRIMAL_VK_QUERY_NONBLOCKING");
    if (type_ == QueryType::Timestamp && nonblocking && std::strcmp(nonblocking,"1") == 0)
        flags &= ~VK_QUERY_RESULT_WAIT_BIT;

    // R10 taskC（Windows 卡死诊断）：WAIT_BIT 在 GPU 楔死/query 永不完成时
    // 无限阻塞（RenderGraph::ResolveTimestamps 每帧 Clear 时走此路径；
    // vkGetQueryPoolResults 无 timeout 参数，唯一有界形态是去 WAIT_BIT 的
    // 非阻塞探测）。PRIMAL_VK_HANG_DIAG=1：非阻塞探测 + 250ms 短宽限重试一次，
    // 仍 NOT_READY 则按旧单次调用的 false 契约返回（MoltenVK 对已执行 CB 的
    // query 也长期 NOT_READY——RenderGraph.cpp 注释记载的怪癖——不能追等，
    // 否则 Mac 每帧假超时）。真正的楔死由后续权威等待点（acquire/fence/
    // present 的 ScopedHangWatch / BoundedWaitFence，超阈值 dump+_Exit(12)）
    // 兜底定位。门控关闭时下方单次调用与原行为逐字节一致。
    if (hangdiag::Enabled() && (flags & VK_QUERY_RESULT_WAIT_BIT)) {
        for (int step = 0; step < 2; ++step) {
            VkResult res = vkGetQueryPoolResults(
                device_, pool_, firstQuery, queryCount,
                queryCount * stride, data, stride,
                flags & ~VK_QUERY_RESULT_WAIT_BIT);
            if (res == VK_SUCCESS) return true;
            if (res == VK_ERROR_DEVICE_LOST) hangdiag::ReportDeviceLost("queryGetResults", res);
            if (res != VK_NOT_READY) return false;
            if (step == 0) {
                fprintf(stderr, "[VK-HANG] still-waiting kind=queryGetResults first=%u count=%u step=0 frame=%llu\n",
                        firstQuery, queryCount,
                        (unsigned long long)hangdiag::g_frame.load());
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
        return false;  // MoltenVK 慢性 NOT_READY：旧单次 WAIT_BIT 调用的 false 契约
    }

    VkResult res = vkGetQueryPoolResults(
        device_, pool_, firstQuery, queryCount,
        queryCount * stride,  // dataSize:严格按 stride 算
        data, stride,
        flags);

    if (res == VK_SUCCESS) return true;
    if (res == VK_NOT_READY) {
        // The nonblocking timestamp experiment expects VK_NOT_READY; keep
        // the existing false contract (the caller must ignore unavailable data).
        return false;
    }
    if (res == VK_ERROR_DEVICE_LOST) {
        // L1 device-lost 结构化事件（CLI 规划 §11.4/§12）：前缀被 DiagnosticsAPI
        // 标记表识别，转成事件表 on_device_lost("Vulkan", …)。
        PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanQueryPool] DEVICE_LOST during GetResults" << std::endl;
        primal::diag::Emit(primal::diag::kLevelFatal, "RHI",
                           "[VulkanQueryPool] DEVICE_LOST during GetResults");
        return false;
    }
    PRIMAL_LOG_STREAM(Error, "RHI") << "[VulkanQueryPool] vkGetQueryPoolResults returned " << res << std::endl;
    return false;
}

} // namespace primal::graphics::rhi

#endif // ENABLE_VULKAN
