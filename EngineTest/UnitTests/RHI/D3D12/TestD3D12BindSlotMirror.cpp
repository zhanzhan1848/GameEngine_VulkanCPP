/**
 * @file TestD3D12BindSlotMirror.cpp
 * @brief D3D12 命令列表槽位绑定镜像（指针+世代）单测
 * @details R21-B（Windows 二十测黑帧回归）：D3D12 的
 *          SetPipelineState/Set{Graphics,Compute}RootSignature 是命令列表
 *          状态，ID3D12GraphicsCommandList::Reset 全清。R20-A 幂等跳过按
 *          裸指针等值判定——任何"镜像未与 Reset 同步清空"的路径会把空
 *          绑定误判为已绑定 → Set* 全部被跳过 → 黑帧。本单测验证
 *          D3D12BindSlotMirror（D3D12CommandBuffer.h Windows 守卫之外的
 *          纔 C++ 模型，macOS 可直接包含）把该缺陷类机械化关闭：
 *          跨世代（Reset/Begin）后同指针不再命中幂等跳过。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/D3D12/D3D12CommandBuffer.h"

#include <cstdint>

using namespace primal::graphics::rhi::d3d12;
using namespace Engine::Test;

namespace {

// 测试替身：真实槽位是 ID3D12*/COM 指针，这里用不透明哨兵指针——模型
// 只做指针等值与世代比较，不依赖 D3D12 类型（这正是它能脱离 D3D12 设备
// 建模的原因）。
struct FakeNative {};
FakeNative g_a, g_b, g_c;
auto* const kPtrA = &g_a;
auto* const kPtrB = &g_b;
auto* const kPtrC = &g_c;

} // namespace

// ---------------------------------------------------------------------------
// 基础语义：空镜像不命中任何候选（含 nullptr）；同世代同指针命中；
// 异指针/异世代不命中。
// ---------------------------------------------------------------------------
TestResult TestMirrorBasics() {
    D3D12BindSlotMirror<FakeNative*> m;
    TEST_ASSERT(!m.Matches(kPtrA, 1), "fresh mirror matches nothing");
    TEST_ASSERT(!m.Matches(nullptr, 1), "fresh mirror does not match nullptr");

    m.Set(kPtrA, 1);
    TEST_ASSERT(m.Matches(kPtrA, 1), "same pointer + same generation matches");
    TEST_ASSERT(!m.Matches(kPtrB, 1), "different pointer does not match");
    TEST_ASSERT(!m.Matches(kPtrA, 2), "same pointer + different generation does not match");

    // nullptr 永远不算"已绑定"——即使被显式 Set 过（空值不构成幂等跳过依据）。
    D3D12BindSlotMirror<FakeNative*> n;
    n.Set(nullptr, 3);
    TEST_ASSERT(!n.Matches(nullptr, 3), "null is never considered bound");

    m.Invalidate();
    TEST_ASSERT(!m.Matches(kPtrA, 1), "Invalidate clears the match");
    TEST_ASSERT(m.generation == 0, "Invalidate resets the recorded generation");
    return TestResult::Passed;
}

// ---------------------------------------------------------------------------
// 任务核心场景：list Reset（世代递增）后，同指针不再被幂等跳过。
// 两个子案：
//   a) 规范路径——Reset 时镜像被 Invalidate（ClearBindingMirrors 语义）；
//   b) 防御路径——Reset 时镜像漏清（历史缺陷形态）：世代比较仍然拦截。
// ---------------------------------------------------------------------------
TestResult TestResetForcesReissue() {
    // (a) 规范路径：Set@gen1 → 世代递增+Invalidate → 同指针不命中。
    D3D12BindSlotMirror<FakeNative*> m;
    std::uint64_t gen = 1;
    m.Set(kPtrA, gen);
    TEST_ASSERT(m.Matches(kPtrA, gen), "issued this generation -> idempotent skip legit");

    ++gen;             // 模拟 ClearBindingMirrors 的世代递增
    m.Invalidate();    // 镜像同步清空
    TEST_ASSERT(!m.Matches(kPtrA, gen),
                "after list Reset, same pointer must be re-issued (not skipped)");
    m.Set(kPtrA, gen); // 重新下发后恢复幂等
    TEST_ASSERT(m.Matches(kPtrA, gen), "re-issued in new generation matches again");

    // (b) 防御路径：镜像漏清（Reset 发生但 Invalidate 缺失）——裸指针等值
    // 会误判"已绑定"，世代比较把跳过拦截为不命中。
    D3D12BindSlotMirror<FakeNative*> stale;
    std::uint64_t gen2 = 5;
    stale.Set(kPtrA, gen2);
    ++gen2;  // list Reset 发生，但镜像未清（缺陷形态）
    TEST_ASSERT(!stale.Matches(kPtrA, gen2),
                "generation guard catches a missed mirror clear (stale pointer "
                "across Reset must not be treated as bound)");
    return TestResult::Passed;
}

// ---------------------------------------------------------------------------
// M1 帧循环生命周期模型：每帧 Reset（世代+1 全清）→ Bind（PSO+两 RS 槽）
// → 帧内重复绑定同管线幂等跳过 → 跨帧同指针必须重新下发。
// 计数"真实下发次数"：每帧每槽恰好 1 次。
// ---------------------------------------------------------------------------
TestResult TestM1FrameLoopLifecycle() {
    D3D12BindSlotMirror<FakeNative*> pso;      // PSO 槽（gfx/compute 共用）
    D3D12BindSlotMirror<FakeNative*> gfxRs;    // graphics 根签名槽
    D3D12BindSlotMirror<FakeNative*> cmpRs;    // compute 根签名槽
    std::uint64_t gen = 0;
    int psoIssues = 0, gfxRsIssues = 0, cmpRsIssues = 0;

    // 帧循环：M1 单 graphics 管线，帧内两次 Bind（宿主偶尔重复绑定）。
    for (int frame = 0; frame < 3; ++frame) {
        // ClearBindingMirrors()：Reset/Begin 落点。
        ++gen;
        pso.Invalidate();
        gfxRs.Invalidate();
        cmpRs.Invalidate();

        for (int bind = 0; bind < 2; ++bind) {  // BindGraphicsPipeline ×2
            if (!gfxRs.Matches(kPtrA, gen)) {
                gfxRs.Set(kPtrA, gen);
                ++gfxRsIssues;
            }
            if (!pso.Matches(kPtrB, gen)) {
                pso.Set(kPtrB, gen);
                ++psoIssues;
            }
        }
        // compute 槽本帧从未绑定：不命中（EnsureComputeState 的跳过依据）。
        TEST_ASSERT(!cmpRs.Matches(kPtrA, gen),
                    "never-bound compute slot does not match in this generation");
    }

    TEST_ASSERT_EQ(3, psoIssues, "PSO issued exactly once per frame (3 frames)");
    TEST_ASSERT_EQ(3, gfxRsIssues, "graphics RS issued exactly once per frame");
    TEST_ASSERT_EQ(0, cmpRsIssues, "compute RS never issued in M1 loop");
    return TestResult::Passed;
}

// ---------------------------------------------------------------------------
// M2 双管线交替模型（depth/lit 两 pass 两根签名；PSO 槽共用）：交替绑定时
// 各槽逐次真实下发；两管线共享同签名指针时 RS 槽合法跳过（根参数保留），
// PSO 槽独立切换。
// ---------------------------------------------------------------------------
TestResult TestM2TwoPipelineAlternation() {
    D3D12BindSlotMirror<FakeNative*> pso;
    D3D12BindSlotMirror<FakeNative*> gfxRs;
    std::uint64_t gen = 0;
    int psoIssues = 0, rsIssues = 0;
    FakeNative* depthPso = kPtrA;
    FakeNative* litPso = kPtrB;

    // 帧 1：两签名独立（depth=kPtrC, lit=kPtrA）。
    ++gen; pso.Invalidate(); gfxRs.Invalidate();
    // depth pass bind
    if (!gfxRs.Matches(kPtrC, gen)) { gfxRs.Set(kPtrC, gen); ++rsIssues; }
    if (!pso.Matches(depthPso, gen)) { pso.Set(depthPso, gen); ++psoIssues; }
    // lit pass bind（签名不同 → 重设；PSO 不同 → 重设）
    if (!gfxRs.Matches(kPtrA, gen)) { gfxRs.Set(kPtrA, gen); ++rsIssues; }
    if (!pso.Matches(litPso, gen)) { pso.Set(litPso, gen); ++psoIssues; }
    TEST_ASSERT_EQ(2, rsIssues, "frame 1: two distinct signatures -> two RS issues");
    TEST_ASSERT_EQ(2, psoIssues, "frame 1: two distinct PSOs -> two PSO issues");

    // 帧 2：同指针跨帧（世代递增）→ 全部重新下发（黑帧缺陷类的正面用例）。
    ++gen; pso.Invalidate(); gfxRs.Invalidate();
    rsIssues = 0; psoIssues = 0;
    if (!gfxRs.Matches(kPtrC, gen)) { gfxRs.Set(kPtrC, gen); ++rsIssues; }
    if (!pso.Matches(depthPso, gen)) { pso.Set(depthPso, gen); ++psoIssues; }
    if (!gfxRs.Matches(kPtrA, gen)) { gfxRs.Set(kPtrA, gen); ++rsIssues; }
    if (!pso.Matches(litPso, gen)) { pso.Set(litPso, gen); ++psoIssues; }
    TEST_ASSERT_EQ(2, rsIssues, "frame 2: same pointers across Reset re-issued");
    TEST_ASSERT_EQ(2, psoIssues, "frame 2: same PSOs across Reset re-issued");

    // 共享签名形态（同帧两管线同签名）：第二次 RS 合法跳过（幂等），PSO
    // 仍独立切换——验证"多 link 不受罚、漏发必失败"的方向性。
    ++gen; pso.Invalidate(); gfxRs.Invalidate();
    rsIssues = 0; psoIssues = 0;
    if (!gfxRs.Matches(kPtrC, gen)) { gfxRs.Set(kPtrC, gen); ++rsIssues; }
    if (!pso.Matches(depthPso, gen)) { pso.Set(depthPso, gen); ++psoIssues; }
    if (!gfxRs.Matches(kPtrC, gen)) { gfxRs.Set(kPtrC, gen); ++rsIssues; }  // 共享签名
    if (!pso.Matches(litPso, gen)) { pso.Set(litPso, gen); ++psoIssues; }
    TEST_ASSERT_EQ(1, rsIssues, "shared signature: second bind idempotently skipped");
    TEST_ASSERT_EQ(2, psoIssues, "shared signature: PSO slot still switches");
    return TestResult::Passed;
}

// ---------------------------------------------------------------------------
// PushConstants 槽自愈语义：布局签名与槽现值不同（跨源布局）→ 不命中 →
// 触发切换记账；graphics/compute 两槽互不串扰。
// ---------------------------------------------------------------------------
TestResult TestPushSwitchAndSlotIndependence() {
    D3D12BindSlotMirror<FakeNative*> gfxRs, cmpRs;
    std::uint64_t gen = 7;
    gfxRs.Set(kPtrA, gen);   // 绑定管线时已设
    cmpRs.Set(kPtrB, gen);

    // 同源 push（布局签名 == 槽现值）：命中 → 不切换。
    TEST_ASSERT(gfxRs.Matches(kPtrA, gen), "same-source push keeps the slot");
    // 跨源 push（布局签名 != 槽现值）：不命中 → 切换并按当前世代记账。
    TEST_ASSERT(!gfxRs.Matches(kPtrB, gen), "divergent layout signature misses");
    gfxRs.Set(kPtrB, gen);
    TEST_ASSERT(gfxRs.Matches(kPtrB, gen), "after switch, new signature recorded");
    // compute 槽不受 graphics 槽切换影响。
    TEST_ASSERT(cmpRs.Matches(kPtrB, gen) && !cmpRs.Matches(kPtrA, gen),
                "compute slot independent of graphics slot switch");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("D3D12BindSlotMirror");
    TEST_CASE(suite, "MirrorBasics", TestMirrorBasics);
    TEST_CASE(suite, "ResetForcesReissue", TestResetForcesReissue);
    TEST_CASE(suite, "M1FrameLoopLifecycle", TestM1FrameLoopLifecycle);
    TEST_CASE(suite, "M2TwoPipelineAlternation", TestM2TwoPipelineAlternation);
    TEST_CASE(suite, "PushSwitchAndSlotIndependence", TestPushSwitchAndSlotIndependence);
    TestStats stats = suite.RunAllTests();
    return stats.failedTests == 0 ? 0 : 1;
}
