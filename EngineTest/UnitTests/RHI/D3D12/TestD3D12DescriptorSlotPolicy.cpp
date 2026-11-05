/**
 * @file TestD3D12DescriptorSlotPolicy.cpp
 * @brief 描述符帧槽线性分配 + 槽纪元策略单测
 * @details R21-C（M1 硬件渐进黑帧诊断轮）。测试对象
 *          DescriptorFrameSlotPolicy 是 Windows 生产侧
 *          D3D12DescriptorHeapManager 两个 shader-visible 堆线性区的
 *          策略单一事实源（D3D12DescriptorHeaps.{h,cpp} 持有本类实例），
 *          本测试在 macOS 直接编译运行同一头文件——策略与测试不漂移。
 *
 *          覆盖：
 *          1. 容量边界：N 帧循环分配不越容量；越界显式失败不静默回绕。
 *          2. 逐帧回收：BeginFrame 后 bump 归零复用（非跨帧单调增长）。
 *          3. 槽纪元（epoch）——R21-C 修复的回归锁：
 *             a) "隔一整个槽周期未被触碰的 set 同槽回归"：旧判定
 *                （只比槽号）会放行陈旧区段，新判定 (slot, epoch) 必须拒绝；
 *                且用分配算术实证旧判定下两 set 区段重叠（黑帧根因族）。
 *             b) 帧内活跃 set（每帧都碰）在新判定下每帧都有效或迁移，
 *                全程无区段重叠（disjoint 不变量）。
 *          4. 分配戳语义：Alloc 后 RegionIsCurrent 用分配时刻的
 *             (slot, epoch) 判定；纪元推进即全部旧戳失效。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/D3D12/D3D12DescriptorSlotPolicy.h"

#include <vector>

using namespace primal::graphics::rhi::d3d12;
using namespace Engine::Test;

namespace {

/// 生产布局镜像（D3D12DescriptorHeaps.h 容量常数的测试侧副本——
/// 主堆 16384 总量 / 4096 持久段 / 3 帧槽）。
constexpr u32 kLinearBase = 4096;
constexpr u32 kSlotCapacity = 4096;
constexpr u32 kFrameSlots = 3;

DescriptorFrameSlotPolicy MakeProductionLikePolicy() {
    DescriptorFrameSlotPolicy p;
    p.Init(kLinearBase, kSlotCapacity, kFrameSlots);
    return p;
}

/// 模拟 set 区段：分配戳 + 区段范围。
struct SetRegion {
    u32 base = DescriptorFrameSlotPolicy::kInvalidSlot;
    u32 count = 0;
    u32 ownerSlot = 0xFFFFFFFFu;
    u64 ownerEpoch = 0;
};

/// EnsureSetRegionCurrent 的策略侧镜像（不含 D3D12 拷贝——只测判定与分配）。
/// 返回 true = 发生了迁移（新分配）；false = 区段本就有效。
bool EnsureCurrent(DescriptorFrameSlotPolicy& p, SetRegion& s) {
    if (s.ownerSlot != 0xFFFFFFFFu
        && p.RegionIsCurrent(s.ownerSlot, s.ownerEpoch)) {
        return false;
    }
    if (!p.Alloc(s.count, s.base)) return false;
    s.ownerSlot = p.CurrentSlot();
    s.ownerEpoch = p.SlotEpoch(p.CurrentSlot());
    return true;
}

/// 断言一组区段两两不重叠（同帧活跃区段的 disjoint 不变量）。
bool RegionsDisjoint(const std::vector<SetRegion>& regions) {
    for (size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].base == DescriptorFrameSlotPolicy::kInvalidSlot) continue;
        for (size_t j = i + 1; j < regions.size(); ++j) {
            if (regions[j].base == DescriptorFrameSlotPolicy::kInvalidSlot) continue;
            const u32 aLo = regions[i].base, aHi = regions[i].base + regions[i].count;
            const u32 bLo = regions[j].base, bHi = regions[j].base + regions[j].count;
            if (aLo < bHi && bLo < aHi) return false;
        }
    }
    return true;
}

} // namespace

TestResult TestCapacityBoundsAndExplicitFailure() {
    auto p = MakeProductionLikePolicy();
    p.BeginFrame(0);
    u32 slot = 0;
    // 恰好占满：容量内分配成功且槽位落在该槽子区域。
    TEST_ASSERT(p.Alloc(kSlotCapacity, slot) && slot == kLinearBase,
                "exact-capacity alloc succeeds at slot region base");
    // 越界 1 个 = 显式失败（不静默回绕、不部分分配）。
    TEST_ASSERT(!p.Alloc(1, slot), "overflow by 1 fails explicitly");
    // BeginFrame 后同一槽可再分配（回收成立）。
    p.BeginFrame(0);
    TEST_ASSERT(p.Alloc(kSlotCapacity, slot), "full capacity reusable after BeginFrame");
    // 单笔超容量 = 永远失败（即使 bump 为 0）。
    p.BeginFrame(0);
    TEST_ASSERT(!p.Alloc(kSlotCapacity + 1, slot), "single alloc > slot capacity fails");
    return TestResult::Passed;
}

TestResult TestPerFrameRecycleNotMonotonic() {
    // 二十一测证据链的关键算术反驳：若分配真跨帧单调，240 帧 × 帧消耗会
    // 撞容量上限；实际策略是逐槽回收——每帧消耗恒定，累计不增长。
    auto p = MakeProductionLikePolicy();
    const u32 kPerFrameUse = 330;  // M4 证据：dstSlot 4413..4427 → 槽内偏移 ~331
    for (u32 f = 0; f < 240; ++f) {
        p.BeginFrame(f);
        u32 base = 0;
        for (u32 i = 0; i < 10; ++i) {
            TEST_ASSERT(p.Alloc(33, base), "steady-state alloc never exhausts");
        }
        TEST_ASSERT(p.Bump() == kPerFrameUse, "per-frame bump is constant");
    }
    // 240 帧后仍在容量内（单调假设下 240×330≈79200 远超 4096 必耗尽）。
    TEST_ASSERT(p.Bump() <= p.SlotCapacity(), "no cross-frame accumulation");
    return TestResult::Passed;
}

TestResult TestSlotGeometry() {
    auto p = MakeProductionLikePolicy();
    p.BeginFrame(0);
    u32 base = 0;
    TEST_ASSERT(p.Alloc(10, base) && base == kLinearBase, "slot0 region base");
    p.BeginFrame(1);
    TEST_ASSERT(p.Alloc(10, base) && base == kLinearBase + kSlotCapacity,
                "slot1 region base");
    p.BeginFrame(2);
    TEST_ASSERT(p.Alloc(10, base) && base == kLinearBase + 2 * kSlotCapacity,
                "slot2 region base");
    p.BeginFrame(3);  // 回绕到槽 0
    TEST_ASSERT(p.Alloc(10, base) && base == kLinearBase, "frame slot wraps to slot0");
    TEST_ASSERT(p.CurrentSlot() == 0, "current slot wraps");
    // frameSlot 取模：BeginFrame(7) → 槽 1。
    p.BeginFrame(7);
    TEST_ASSERT(p.CurrentSlot() == 1, "frameSlot index modulo slot count");
    return TestResult::Passed;
}

TestResult TestEpochRejectsStaleSameSlotRegion() {
    // R21-C 核心回归：set A 在帧 N（槽 s，纪元 e）分配；此后 A 未被触碰，
    // 帧轮转一整圈回到槽 s（纪元 e+1）——旧判定（只比槽号）会认为 A 的
    // 区段仍有效，但该槽已被回收重分配。
    auto p = MakeProductionLikePolicy();

    // 帧 0（槽 0）：A、B 相继分配。
    p.BeginFrame(0);
    SetRegion a{0, 10, 0xFFFFFFFFu, 0}, b{0, 10, 0xFFFFFFFFu, 0};
    TEST_ASSERT(EnsureCurrent(p, a), "A allocated at frame 0");
    const u32 aBase = a.base;
    TEST_ASSERT(EnsureCurrent(p, b), "B allocated at frame 0");

    // 帧 1、2：A、B 未被触碰（模拟 skip-pass 宿主）。
    p.BeginFrame(1);
    p.BeginFrame(2);

    // 帧 3（槽 0 回归，纪元已推进）：新 set C 分配——bump 从 0 重新开始，
    // C 会拿到 A 的旧基槽。
    p.BeginFrame(3);
    SetRegion c{0, 10, 0xFFFFFFFFu, 0};
    TEST_ASSERT(EnsureCurrent(p, c), "C allocated at frame 3");
    TEST_ASSERT(c.base == aBase,
                "recycled region hands out A's old base (recycle works)");

    // 旧判定重算（ownerSlot == currentSlot）：A 槽号匹配 → 误判有效。
    const bool oldCheckWouldPass = (a.ownerSlot == p.CurrentSlot());
    TEST_ASSERT(oldCheckWouldPass,
                "old slot-only check would pass for A (premise of the bug)");
    // 新判定 (slot, epoch)：A 的旧纪元必须被拒绝。
    TEST_ASSERT(!p.RegionIsCurrent(a.ownerSlot, a.ownerEpoch),
                "epoch check rejects stale same-slot region");
    // A 迁移：拿到与 C 不重叠的新区段（旧判定下 A 与 C 共享同段槽位）。
    TEST_ASSERT(EnsureCurrent(p, a), "A migrates to a fresh region");
    TEST_ASSERT(a.base != c.base && RegionsDisjoint({a, c}),
                "migrated A does not alias C");
    return TestResult::Passed;
}

TestResult TestEpochDisjointInvariantAcrossCycle() {
    // 每帧活跃的工作集（含隔帧跳过者）在纪元判定下全程无区段重叠。
    auto p = MakeProductionLikePolicy();
    SetRegion sets[5];
    for (int i = 0; i < 5; ++i) sets[i].count = 8;
    for (u32 f = 0; f < 12; ++f) {  // 4 个完整槽周期
        p.BeginFrame(f);
        // set 0-3 每帧活跃；set 4 每隔 3 帧活跃一次（恰同槽回归——旧判定
        // 的最险形态）。
        for (int i = 0; i < 4; ++i) EnsureCurrent(p, sets[i]);
        if (f % kFrameSlots == 0) EnsureCurrent(p, sets[4]);
        std::vector<SetRegion> live(std::begin(sets), std::end(sets));
        TEST_ASSERT(RegionsDisjoint(live),
                    "live set regions never overlap across full slot cycles");
        // 活跃 set 的戳必须全部时效（EnsureCurrent 刚校验过）。
        for (int i = 0; i < 4; ++i) {
            TEST_ASSERT(p.RegionIsCurrent(sets[i].ownerSlot, sets[i].ownerEpoch),
                        "active set stamp stays current");
        }
    }
    return TestResult::Passed;
}

TestResult TestEpochAdvancesPerSlotOnly() {
    // 纪元按槽推进：BeginFrame(0) 三次只推进槽 0 的纪元；其他槽不受影响。
    auto p = MakeProductionLikePolicy();
    p.BeginFrame(0);
    const u64 e0 = p.SlotEpoch(0), e1 = p.SlotEpoch(1), e2 = p.SlotEpoch(2);
    TEST_ASSERT(e0 == 1 && e1 == 0 && e2 == 0, "epoch advances only for entered slot");
    p.BeginFrame(0);
    TEST_ASSERT(p.SlotEpoch(0) == 2 && p.SlotEpoch(1) == 0,
                "re-entering same slot advances its epoch again");
    return TestResult::Passed;
}

TestResult TestHighWaterTracking() {
    auto p = MakeProductionLikePolicy();
    p.BeginFrame(0);
    u32 base = 0;
    p.Alloc(100, base);
    p.Alloc(50, base);
    TEST_ASSERT(p.Bump() == 150, "bump accumulates within frame");
    TEST_ASSERT(p.HighWaterFor(0) == 150, "high-water tracks in-frame peak");
    p.BeginFrame(0);
    TEST_ASSERT(p.LastCycleHighFor(0) == 150, "last-cycle high snapshotted at BeginFrame");
    TEST_ASSERT(p.Bump() == 0, "bump reset at BeginFrame");
    p.Alloc(10, base);
    TEST_ASSERT(p.HighWaterFor(0) == 150, "all-time high-water persists");
    TEST_ASSERT(p.LastCycleHighFor(0) == 150, "snapshot unchanged mid-cycle");
    return TestResult::Passed;
}

void RegisterD3D12DescriptorSlotPolicyTests() {
    auto suite = std::make_shared<TestSuite>("D3D12DescriptorSlotPolicyTests");
    suite->AddTestCase(TestCase("CapacityBoundsAndExplicitFailure", TestCapacityBoundsAndExplicitFailure));
    suite->AddTestCase(TestCase("PerFrameRecycleNotMonotonic", TestPerFrameRecycleNotMonotonic));
    suite->AddTestCase(TestCase("SlotGeometry", TestSlotGeometry));
    suite->AddTestCase(TestCase("EpochRejectsStaleSameSlotRegion", TestEpochRejectsStaleSameSlotRegion));
    suite->AddTestCase(TestCase("EpochDisjointInvariantAcrossCycle", TestEpochDisjointInvariantAcrossCycle));
    suite->AddTestCase(TestCase("EpochAdvancesPerSlotOnly", TestEpochAdvancesPerSlotOnly));
    suite->AddTestCase(TestCase("HighWaterTracking", TestHighWaterTracking));
    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterD3D12DescriptorSlotPolicyTests();
    const auto runStats = TestRunner::RunAllSuites();
    return runStats.failedTests == 0 ? 0 : 1;
}
#endif
