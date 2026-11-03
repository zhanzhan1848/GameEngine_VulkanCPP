#pragma once
// RHIPassTimestampAudit.h — pass timestamp 审计的平台中立入口（D3D12 M0
// F0-11）。
//
// 背景：Sponza 诊断的时间戳审计（VulkanPassTimestampAudit.h 的 Audit 类）
// 原本被共享代码 RenderGraph.cpp 直接 include 并调用，测试侧
// TestVulkanSponzaRenderGraph 还要 static_cast<VulkanDevice*>——共享代码长
// 出后端硬依赖（F0-12 门禁违规）。本头提供中立入口：
//   - 共享代码只 include 本头；GetEntry() 返回 nullptr（后端未注册）时
//     全部调用自然 no-op——与旧行为一致（Metal/Dawn 路径本来就被平台门
//     短路）。
//   - Vulkan 实现：Platforms/Vulkan/VulkanPassTimestampEntry.cpp，静态注册，
//     内部转发给既有 thread_local Audit 实例——ticket 本体逻辑零改动
//     （PassTimestampTicket.h / VulkanPassTimestampAudit.h 未动）。
//   - D3D12 特化：M1+ 同模式提供；未提供 = 显式不支持（GetEntry()==
//     nullptr，渲染图统计走 RG 自身 query pool 路径，不受影响）。
//
// 线程性：实现转发到调用线程的 thread_local Audit 实例——与旧直接调用
// 完全相同；Entry 方法必须在录制线程调用（RenderGraph::Execute 即是）。
#include "RHITypes.h"
#include "RHICommand.h"
#include <cstdint>

namespace primal::graphics::rhi::pass_timestamp {

/// 平台中立审计入口。方法语义与 Vulkan Audit 类同名方法一一对应；
/// 返回值语义见 VulkanPassTimestampAudit.h（Begin 返回 0 = 未启用/被拒）。
class Entry {
public:
    virtual ~Entry() = default;

    /// 帧提交边界开始一次审计窗口。command = 命令缓冲句柄数值，
    /// slot = 帧槽（0-2），fence = 该帧信号 fence 的目标值数值。
    virtual u64 Begin(RHIDeviceBase* device, RHICommandBuffer* cmd,
                      u64 command, u32 slot, u64 fence, u64 frame) = 0;
    /// 渲染图执行计数（每个 RenderGraph::Execute 调用一次）。
    virtual void Graph(RHICommandBuffer* cmd) = 0;
    /// pass 边界时间戳对（仅选定的 pass 名）；返回 pair 索引或 -1。
    virtual int BeginPass(RHICommandBuffer* cmd, const char* name, bool graphics) = 0;
    virtual void EndPass(RHICommandBuffer* cmd, int index) = 0;
    /// 提交认证：ended/submitted 由调用方的真实 End/Submit 结果给出。
    virtual void Commit(u32 slot, u64 cmd, u64 fence, u64 frame, u64 ticket,
                        bool ended, bool submitted) = 0;
    /// 设备 WaitIdle 成功后释放（绑定生命周期解绑 + 池销毁）。
    virtual void ReleaseAfterIdle(RHIDeviceBase* device) = 0;
};

/// 当前注册的后端入口；nullptr = 无后端支持（调用方按 no-op 处理）。
inline Entry*& RegisteredEntry() {
    static Entry* entry = nullptr;  // inline 函数局部 static：ODR 安全
    return entry;
}

inline void RegisterEntry(Entry* entry) { RegisteredEntry() = entry; }

/// 便捷入口：未注册时返回 nullptr。
inline Entry* GetEntry() { return RegisteredEntry(); }

} // namespace primal::graphics::rhi::pass_timestamp
