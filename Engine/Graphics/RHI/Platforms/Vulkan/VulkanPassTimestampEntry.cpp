/**
 * @file VulkanPassTimestampEntry.cpp
 * @brief RHIPassTimestampAudit 入口的 Vulkan 特化（F0-11）
 * @details 把中立 Entry 转发给既有 thread_local Audit 实例——ticket 本体
 *          逻辑（PassTimestampTicket.h / VulkanPassTimestampAudit.h 的
 *          Audit 类）零改动。static_cast 位于后端目录内（F0-12 合法域）。
 *
 *          平台门语义保持：旧共享代码在 platform!=Vulkan 时传 nullptr
 *          给 Audit::Graph/BeginPass/EndPass（Audit 内部 no-op）。本实现用
 *          thread_local lastBeginIsVulkan 记录"当前线程的审计绑定是否
 *          Vulkan 设备"——非 Vulkan（或从未 Begin）时同样传 nullptr，
 *          行为与旧平台门逐位一致。
 */

#include "Graphics/RHI/Core/RHIPassTimestampAudit.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPassTimestampAudit.h"
#include "VulkanBackendLinks.h"

namespace primal::graphics::rhi::pass_timestamp {

namespace {

// 当前线程的 Audit 绑定是否为 Vulkan 设备（镜像旧 RenderGraph.cpp 的
// `platform==Vulkan ? cast : nullptr` 平台门）。Audit::Begin 只对 Vulkan
// 设备成功，因此 Begin 置 true / 其他情形置 false 即可精确复刻。
bool& LastBeginIsVulkan() {
    static thread_local bool value = false;
    return value;
}

class VulkanPassTimestampEntry final : public Entry {
public:
    u64 Begin(RHIDeviceBase* device, RHICommandBuffer* cmd,
              u64 command, u32 slot, u64 fence, u64 frame) override {
        LastBeginIsVulkan() = false;
        if (!device || device->GetPlatform() != RHIPlatform::Vulkan) return 0;
        auto* vkDevice = static_cast<VulkanDevice*>(device);
        auto* vkCmd = static_cast<VulkanCommandBuffer*>(cmd);
        const u64 ticket = instance.Begin(vkDevice, vkCmd, command, slot, fence, frame);
        // ticket!=0 => Audit 已绑定该 Vulkan 设备；0 => 未启用/被拒（Graph
        // 保持 no-op，与旧路径一致）。
        LastBeginIsVulkan() = ticket != 0;
        return ticket;
    }

    void Graph(RHICommandBuffer* cmd) override {
        instance.Graph(CastIfBound(cmd));
    }

    int BeginPass(RHICommandBuffer* cmd, const char* name, bool graphics) override {
        return instance.BeginPass(CastIfBound(cmd), name, graphics);
    }

    void EndPass(RHICommandBuffer* cmd, int index) override {
        instance.EndPass(CastIfBound(cmd), index);
    }

    void Commit(u32 slot, u64 cmd, u64 fence, u64 frame, u64 ticket,
                bool ended, bool submitted) override {
        instance.Commit(slot, cmd, fence, frame, ticket, ended, submitted);
    }

    void ReleaseAfterIdle(RHIDeviceBase* device) override {
        if (!device || device->GetPlatform() != RHIPlatform::Vulkan) return;
        auto* vkDevice = static_cast<VulkanDevice*>(device);
        LastBeginIsVulkan() = false;
        instance.ReleaseAfterIdle(vkDevice);
    }

private:
    // 仅当本线程 Audit 已绑定 Vulkan 设备时向下转型；否则传 nullptr
    //（Audit 内部 active_ 未置位时对 nullptr 一律 no-op——与旧平台门等价）。
    static VulkanCommandBuffer* CastIfBound(RHICommandBuffer* cmd) {
        return LastBeginIsVulkan() ? static_cast<VulkanCommandBuffer*>(cmd) : nullptr;
    }
};

// 静态注册：RenderGraph / 测试经中立 GetEntry() 分派。
VulkanPassTimestampEntry gVulkanPassTimestampEntry;
const bool gVulkanPassTimestampEntryRegistered =
    (RegisterEntry(&gVulkanPassTimestampEntry), true);

} // namespace

/// 链接锚点（见 VulkanBackendLinks.h）：静态注册对象无符号引用时会被
/// 链接器从静态库裁剪（2026-11-03 实测：archive 含符号、二进制无），
/// GetEntry() 恒 nullptr。引用本 TU 静态对象强制拉入。幂等。
void EnsureVulkanPassTimestampEntryLinked() {
    (void)gVulkanPassTimestampEntryRegistered;
}

} // namespace primal::graphics::rhi::pass_timestamp

#endif // defined(ENABLE_VULKAN) && ENABLE_VULKAN
