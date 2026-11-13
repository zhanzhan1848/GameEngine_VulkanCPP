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
    // R11 taskB（Windows 默认路径第 2 帧死锁修复）：默认 flags 不带
    // VK_QUERY_RESULT_WAIT_BIT。RenderGraph::ResolveTimestamps 每帧 Clear 时读
    // 2 槽环里上一拍的 query pool；GPU 慢一拍以上时（Windows 原生 NVIDIA 驱动
    // + 显存超预算下 ~600ms/帧）上一拍 query 尚未 available，而
    // vkGetQueryPoolResults 无 timeout 参数，WAIT_BIT 在原生驱动上对不可用
    // query 无限阻塞 → 第 2 帧起必然死等（watchdog 300s exit 7）。
    // MoltenVK 有文档化怪癖（RenderGraph.cpp T8.13/P0 注释）：即使带 WAIT_BIT
    // 也立即返回 VK_NOT_READY 且结果缓冲已暂存有效数据——Mac 从未真正阻塞，
    // 该缺陷一直被掩盖、只在 Windows 暴露。默认改为非阻塞 availability 读后，
    // VK_NOT_READY 按既有 false 契约返回（调用方不发布当帧时序数据，也不授权
    // 任何 host 资源复用），MoltenVK 返回值/暂存数据行为不变（实测 readOk
    // 分布与改动前一致），Mac 像素输出无可测漂移——CPU 等待仅从 query 读内
    // 移到帧 fence（净帧时不变），实测落在同二进制运行间噪声底内。
    VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
    // Experiment only: absence of a result is not zero GPU work and must not
    // authorize host reuse of any frame slot or resource. Callers must use the
    // returned boolean to distinguish unavailable timestamp data.
    // R11 taskB: 查询侧现已默认非阻塞，本开关不再改变此处 flags；保留环境变量
    // 是因为 RenderGraph::ResolveTimestamps 仍用它门控 NOT_READY 时的暂存数据
    // 发布路径（=1 的语义向后兼容不变）。
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
    // 兜底定位。R11 taskB 后默认 flags 已无 WAIT_BIT，本分支改为仅按
    // hangdiag::Enabled() 进入（否则诊断遥测静默失效）：对现在的非阻塞默认读
    // 保留 still-waiting 观测，区分"慢一拍"与"query 永不完成"。
    if (hangdiag::Enabled()) {
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
        // The nonblocking default (R11 taskB) and the legacy PRIMAL_VK_QUERY_
        // NONBLOCKING experiment both expect VK_NOT_READY when the previous
        // ring slot's queries are not yet available; keep the existing false
        // contract (the caller must ignore unavailable data).
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
