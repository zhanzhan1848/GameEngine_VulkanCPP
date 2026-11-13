// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12BackendLinks.cpp — capture/timestamp 观测的 D3D12 特化占位（M1）。
#include "D3D12BackendLinks.h"

#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include "Engine/Common/CoreLogStream.h"
#include "Graphics/RHI/Core/RHIPassTimestampAudit.h"
#include "Graphics/RenderGraph/RHICaptureBridge.h"

namespace primal::graphics::rhi {

// ============================================================================
// RHIPassTimestampAudit：显式 not-supported Entry
// ============================================================================

namespace {

/// M1 占位 Entry：Begin 返回 0（RHIPassTimestampAudit.h 契约 =
/// "未启用/被拒"），其余方法 no-op。渲染图统计走 RG 自身 query pool 路径
/// 不受影响。M3+（timestamp query 接通）替换为真实实现。
class D3D12PassTimestampEntryStub final : public pass_timestamp::Entry {
public:
    u64 Begin(RHIDeviceBase*, RHICommandBuffer*, u64, u32, u64, u64) override {
        return 0;  // 显式拒绝：M1 无 D3D12 timestamp 观测
    }
    void Graph(RHICommandBuffer*) override {}
    int BeginPass(RHICommandBuffer*, const char*, bool) override { return -1; }
    void EndPass(RHICommandBuffer*, int) override {}
    void Commit(u32, u64, u64, u64, u64, bool, bool) override {}
    void ReleaseAfterIdle(RHIDeviceBase*) override {}
};

D3D12PassTimestampEntryStub gD3D12TimestampStub;

bool gD3D12LinksLogged = false;

} // namespace

void EnsureD3D12BackendLinksLinked() {
    // 幂等注册：timestamp stub Entry（显式 not-supported）。
    pass_timestamp::RegisterEntry(&gD3D12TimestampStub);

    if (!gD3D12LinksLogged) {
        gD3D12LinksLogged = true;
        // capture：不注册 resolver —— BackendCaptureSupported(D3D12)==false
        // 是中立层的显式"后端不支持"路径（capture 层据此干净跳过）。注册一个
        // 恒 false 的 resolver 会把门翻成 true，让 capture 层产出空证据集
        // ——比不注册更糟。M3+（capture 观测充实）替换为真实 resolver。
        PRIMAL_LOG_STREAM(Info, "RHI")
            << "[D3D12BackendLinks] observation backends: pass-timestamp="
            << "NOT-SUPPORTED(M1 stub, Begin->0); texture-capture="
            << "NOT-SUPPORTED(M1; resolver unregistered by design)" << std::endl;
    }
}

} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
