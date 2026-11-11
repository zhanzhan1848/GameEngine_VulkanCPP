/**
 * @file TestD3D12DescriptorSlotPolicy.cpp
 * @brief 描述符帧槽线性分配 + 槽纪元策略 + sampler 表内容级共享单测
 * @details R21-C（M1 硬件渐进黑帧诊断轮）/ R22-S1（2026-11-10 D3D12
 *          sampler 线性区耗尽修复）。测试对象
 *          DescriptorFrameSlotPolicy 与 SamplerTableSharingPolicy 是
 *          Windows 生产侧 D3D12DescriptorHeapManager 两个 shader-visible
 *          堆线性区策略的单一事实源（D3D12DescriptorHeaps.{h,cpp} 持有
 *          本类实例），本测试在 macOS 直接编译运行同一头文件——策略与
 *          测试不漂移。
 *
 *          覆盖：
 *          1. 容量边界：N 帧循环分配不越容量；越界显式失败不回绕。
 *          2. 逐帧回收：BeginFrame 后 bump 归零复用（非跨帧单调增长）。
 *          3. 槽纪元（epoch）——R21-C 修复的回归锁：
 *             a) "隔一整个槽周期未被触碰的 set 同槽回归"：旧判定
 *                （只比槽号）会放行陈旧区段，新判定 (slot, epoch) 必须拒绝；
 *                且用分配算术实证旧判定下两 set 区段重叠（黑帧根因族）。
 *             b) 帧内活跃 set（每帧都碰）在新判定下每帧都有效或迁移，
 *                全程无区段重叠（disjoint 不变量）。
 *          4. 分配戳语义：Alloc 后 RegionIsCurrent 用分配时刻的
 *             (slot, epoch) 判定；纪元推进即全部旧戳失效。
 *          5. sampler 表内容级共享（R22-S1 回归锁）：
 *             a) 内容键哈希：同内容同键、异内容异键、"未写"哨兵可区分；
 *             b) 条目 (slot, epoch) 时效：过期即 miss；同键处方条目
 *                Activate 后复活（"谁发现过期谁重建，每内容每槽一次"的
 *                记账面）；
 *             c) init 风暴去重：按日志算术（2 + 170×3 = 512）复现
 *                耗尽路径，再证共享路径同场景只耗 O(唯一内容数)；
 *             d) 两调用分段写入（binding 分两次 Update）同样收敛到
 *                O(唯一内容数)；
 *             e) sampler 持久槽释放 purge：引用条目消失、空闲位可复用；
 *             f) 条目表容量边界：满表降级不失败、purge 后可恢复。
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

// ============================================================================
// R22-S1：sampler 表内容级共享（SamplerTableSharingPolicy）
// ============================================================================

namespace {

/// 生产 sampler 线性区布局镜像（sampler 堆 2048 = 持久 512 + 3 槽 × 512）。
constexpr u32 kSamplerLinearBase = 512;
constexpr u32 kSamplerSlotCapacity = 512;

/// "set 的 sampler 写入解析"策略侧镜像（不含 D3D12 拷贝——决策/分配/记账）：
/// 内容命中即共享既有区间；miss 则线性分配一段并注册（每内容每槽一次）。
/// 与 Windows 侧 D3D12Device::updateDescriptorSetsImpl 的 per-call 决策同构。
struct SharedSetSim {
    std::vector<u32> src;   ///< 内容处方（kUnwritten = 未写）
    u32 snapshotBase = SamplerTableSharingPolicy::kInvalidIndex;  ///< 解析快照
};

/// 对一个 set 执行"一次 Update 调用"的解析：finalSrc = src 应用本次全部
/// sampler 写入（prescan 语义）→ 命中共享 / 分配+注册。返回 false = 线性
/// 区耗尽（等价生产侧显式失败）。
bool ResolveSamplerWrite(DescriptorFrameSlotPolicy& lin,
                         SamplerTableSharingPolicy& sharing,
                         SharedSetSim& set,
                         const std::vector<std::pair<u32, u32>>& items) {
    // prescan：应用本调用全部写入到处方副本。
    std::vector<u32> finalSrc = set.src;
    for (const auto& [idx, persistentSlot] : items) {
        if (idx < finalSrc.size()) finalSrc[idx] = persistentSlot;
    }
    const u32 count = static_cast<u32>(finalSrc.size());
    const u32 slot = lin.CurrentSlot();
    const u64 epoch = lin.SlotEpoch(slot);
    const u64 key = HashSamplerTable(finalSrc.data(), count);
    const u32 hit = sharing.FindActive(key, slot, epoch, finalSrc.data(), count);
    if (hit != SamplerTableSharingPolicy::kInvalidIndex) {
        // 内容已物化：直接共享（区间内容即 finalSrc——幂等，零新增分配）。
        set.snapshotBase = sharing.EntryBaseSlot(hit);
        set.src = finalSrc;
        return true;
    }
    u32 base = 0;
    if (!lin.Alloc(count, base)) return false;  // 显式失败（不回绕）
    const u32 entryIdx = sharing.FindByKey(key, finalSrc.data(), count);
    if (entryIdx != SamplerTableSharingPolicy::kInvalidIndex) {
        sharing.Activate(entryIdx, base, slot, epoch);   // 过期条目复活（重建落位）
    } else {
        sharing.Register(key, base, slot, epoch, finalSrc.data(), count);
    }
    set.src = finalSrc;
    set.snapshotBase = base;
    return true;
}

} // namespace

TestResult TestSamplerKeyHashSemantics() {
    const u32 a[2] = {7, 9};
    const u32 b[2] = {7, 9};
    const u32 c[2] = {9, 7};           // 顺序不同 = 内容不同
    const u32 d[2] = {7, SamplerTableSharingPolicy::kUnwritten};
    TEST_ASSERT(HashSamplerTable(a, 2) == HashSamplerTable(b, 2),
                "identical tables hash equal");
    TEST_ASSERT(HashSamplerTable(a, 2) != HashSamplerTable(c, 2),
                "entry order is part of content identity");
    TEST_ASSERT(HashSamplerTable(a, 2) != HashSamplerTable(d, 2),
                "unwritten sentinel is a distinct content state");
    TEST_ASSERT(SameSamplerTable(a, b, 2) && !SameSamplerTable(a, c, 2),
                "exact-equality comparator agrees");
    // 表长是键的一部分：1 元素表 ≠ 2 元素表的前缀。
    const u32 e[1] = {7};
    TEST_ASSERT(HashSamplerTable(a, 2) != HashSamplerTable(e, 1),
                "table length is part of the key");
    return TestResult::Passed;
}

TestResult TestSamplerSharingEntryEpochLifecycle() {
    auto lin = MakeProductionLikePolicy();
    SamplerTableSharingPolicy sharing;
    sharing.Init(16);
    lin.BeginFrame(0);  // 槽 0，纪元 1

    const u32 table[1] = {42};
    const u64 key = HashSamplerTable(table, 1);
    TEST_ASSERT(sharing.Register(key, 100, 0, lin.SlotEpoch(0), table, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "register succeeds");
    TEST_ASSERT(sharing.FindActive(key, 0, lin.SlotEpoch(0), table, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "find active hits for (slot, epoch) of registration");
    TEST_ASSERT(sharing.FindActive(key, 0, lin.SlotEpoch(0) + 1, table, 1)
                    == SamplerTableSharingPolicy::kInvalidIndex,
                "epoch mismatch = stale entry must miss");
    TEST_ASSERT(sharing.FindActive(key, 1, 0, table, 1)
                    == SamplerTableSharingPolicy::kInvalidIndex,
                "slot mismatch must miss");
    // 轮转一整圈回槽 0（纪元 2）：同键条目按处方复活一次（谁发现谁重建）。
    lin.BeginFrame(1);
    lin.BeginFrame(2);
    lin.BeginFrame(3);  // 槽 0 回归，纪元 2
    const u32 staleIdx = sharing.FindByKey(key, table, 1);
    TEST_ASSERT(staleIdx != SamplerTableSharingPolicy::kInvalidIndex,
                "stale same-key entry retained as rebuild prescription");
    sharing.Activate(staleIdx, 200, 0, lin.SlotEpoch(0));
    TEST_ASSERT(sharing.FindActive(key, 0, lin.SlotEpoch(0), table, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "activated entry is current again (new base 200)");
    TEST_ASSERT(sharing.FindActive(key, 0, lin.SlotEpoch(0) - 1, table, 1)
                    == SamplerTableSharingPolicy::kInvalidIndex,
                "old epoch of same slot still misses");
    return TestResult::Passed;
}

TestResult TestSamplerSharingInitStormDedup() {
    // 根因算术回归锁（Windows b115034 / GTX1060 实测日志）：
    //   init 期 slot 0 上，宿主预置 set 占 2 个 sampler 描述符（deferred
    //   lightSet：binding 7+9），随后每 mesh = material set 1 个 + lit set
    //   2 个。旧路径（每 set 独立区间）：2 + 170×3 = 512 恰好打满，第
    //   171 个 mesh 的 material set（need=1）失败——与日志
    //   "bump=512 need=1 capacity=512" 逐字一致。
    {
        DescriptorFrameSlotPolicy oldLin;
        oldLin.Init(kSamplerLinearBase, kSamplerSlotCapacity, kFrameSlots);
        oldLin.BeginFrame(0);
        u32 base = 0;
        TEST_ASSERT(oldLin.Alloc(2, base), "pre-seed 2 (host init sets)");
        int fullMeshes = 0;
        bool materialSetFailed = false;
        for (int m = 0; m < 400; ++m) {
            if (!oldLin.Alloc(1, base)) { materialSetFailed = true; break; }  // material set
            if (!oldLin.Alloc(2, base)) break;                               // lit set
            ++fullMeshes;
        }
        TEST_ASSERT(materialSetFailed && fullMeshes == 170 && oldLin.Bump() == 512,
                    "old per-set ranges: 2+170*3=512, mesh 171 material set (need=1) fails");
    }
    // 共享路径：同场景（200 mesh × 每 mesh 2 个 set，一次性全量写入调用）
    // 只耗 O(唯一内容数)——3 段共 5 描述符（预置 2 + material 1 + lit 2）。
    {
        DescriptorFrameSlotPolicy lin;
        lin.Init(kSamplerLinearBase, kSamplerSlotCapacity, kFrameSlots);
        lin.BeginFrame(0);
        SamplerTableSharingPolicy sharing;
        sharing.Init(256);
        // 预置段（宿主 deferred lightSet 等价物：binding7/9 两个采样器）——
        // 同样走共享解析（生产中它也是每帧同内容的共享 set）。
        SharedSetSim lightSet{{20, 21}, 0};
        TEST_ASSERT(ResolveSamplerWrite(lin, sharing, lightSet, {{0, 20}, {1, 21}}),
                    "pre-seed set resolves");
        const u32 kSamplerA = 3, kSamplerB = 5;   // 宿主共享 SamplerHandle 的持久槽
        for (int m = 0; m < 200; ++m) {
            SharedSetSim material{{kSamplerA}, 0};
            SharedSetSim lit{{kSamplerA, kSamplerB}, 0};
            TEST_ASSERT(ResolveSamplerWrite(lin, sharing, material, {{0, kSamplerA}}),
                        "material set resolves (shared)");
            TEST_ASSERT(ResolveSamplerWrite(lin, sharing, lit,
                                            {{0, kSamplerA}, {1, kSamplerB}}),
                        "lit set resolves (shared)");
        }
        TEST_ASSERT(lin.Bump() == 2 + 1 + 2,
                    "dedup: 400 sets over 3 unique contents consume 5, not 602");
        TEST_ASSERT(sharing.UniqueKeyCount() == 3, "three unique sampler contents registered");
    }
    return TestResult::Passed;
}

TestResult TestSamplerSharingTwoCallSequenceConverges() {
    // 宿主 lit set 的另一种写序：binding1/binding3 分两次 Update 调用写入。
    // 每 set 的第一次调用物化中间态 [sA, unwritten]，第二次物化终态
    // [sA, sB]——首个 set 烧 2 段（中间态+终态），其余 set 两次调用全部
    // 命中共享，零新增分配。
    DescriptorFrameSlotPolicy lin;
    lin.Init(kSamplerLinearBase, kSamplerSlotCapacity, kFrameSlots);
    lin.BeginFrame(0);
    SamplerTableSharingPolicy sharing;
    sharing.Init(256);
    const u32 kA = 3, kB = 5;
    for (int m = 0; m < 100; ++m) {
        SharedSetSim set{{SamplerTableSharingPolicy::kUnwritten,
                          SamplerTableSharingPolicy::kUnwritten}, 0};
        TEST_ASSERT(ResolveSamplerWrite(lin, sharing, set, {{0, kA}}),
                    "call 1 (binding 1) resolves");
        TEST_ASSERT(ResolveSamplerWrite(lin, sharing, set, {{1, kB}}),
                    "call 2 (binding 3) resolves");
    }
    // 中间态 1 段（2 槽）+ 终态 1 段（2 槽）= 4；100 个 set 不再各自占段。
    TEST_ASSERT(lin.Bump() == 4, "two-call write sequence converges to 2 ranges");
    TEST_ASSERT(sharing.UniqueKeyCount() == 2,
                "intermediate and final contents each registered once");
    return TestResult::Passed;
}

TestResult TestSamplerSharingPurgeOnSamplerDestroy() {
    SamplerTableSharingPolicy sharing;
    sharing.Init(16);
    const u32 t0[1] = {7};
    const u32 t1[1] = {9};
    const u32 t2[2] = {7, 9};
    sharing.Register(HashSamplerTable(t0, 1), 100, 0, 1, t0, 1);
    sharing.Register(HashSamplerTable(t1, 1), 101, 0, 1, t1, 1);
    sharing.Register(HashSamplerTable(t2, 2), 102, 0, 1, t2, 2);
    TEST_ASSERT(sharing.UniqueKeyCount() == 3, "three entries before purge");
    // 持久槽 7 的 SamplerHandle 销毁：t0 与 t2 两条处方引用它 → 全部 purge；
    // t1 不受影响。
    sharing.PurgeSlotsReferencing(7);
    TEST_ASSERT(sharing.UniqueKeyCount() == 1, "entries referencing destroyed slot purged");
    TEST_ASSERT(sharing.FindActive(HashSamplerTable(t1, 1), 0, 1, t1, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "unrelated entry survives");
    TEST_ASSERT(sharing.FindByKey(HashSamplerTable(t0, 1), t0, 1)
                    == SamplerTableSharingPolicy::kInvalidIndex,
                "purged entry is gone (no stale prescription to copy from)");
    // purge 出来的空闲位可被新内容复用（表不泄漏增长）。
    const u32 t3[1] = {50};
    TEST_ASSERT(sharing.Register(HashSamplerTable(t3, 1), 103, 0, 1, t3, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "freed entry slot reusable");
    return TestResult::Passed;
}

TestResult TestSamplerSharingCapacityDegradesNotFails() {
    SamplerTableSharingPolicy sharing;
    sharing.Init(2);
    const u32 a[1] = {1}, b[1] = {2}, c[1] = {3};
    TEST_ASSERT(sharing.Register(HashSamplerTable(a, 1), 10, 0, 1, a, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex, "entry 1");
    TEST_ASSERT(sharing.Register(HashSamplerTable(b, 1), 11, 0, 1, b, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex, "entry 2");
    TEST_ASSERT(sharing.Register(HashSamplerTable(c, 1), 12, 0, 1, c, 1)
                    == SamplerTableSharingPolicy::kInvalidIndex,
                "full table: register degrades to kInvalidIndex (caller stays private)");
    // 同键重复注册永不耗表（原位刷新，不占新槽）。
    TEST_ASSERT(sharing.Register(HashSamplerTable(a, 1), 13, 0, 1, a, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "same-key re-register refreshes in place");
    TEST_ASSERT(sharing.UniqueKeyCount() == 2, "still two unique keys");
    // purge 腾位后新内容可注册。
    sharing.PurgeSlotsReferencing(2);
    TEST_ASSERT(sharing.Register(HashSamplerTable(c, 1), 12, 0, 1, c, 1)
                    != SamplerTableSharingPolicy::kInvalidIndex,
                "after purge, capacity available again");
    return TestResult::Passed;
}

TestResult TestSamplerSharingRebuildOncePerSlotCycle() {
    // 槽轮转后多 set 共享同一内容：每内容每槽只重建一次——第一个发现过期
    // 的 set 触发重建（bump +count 一次），其余 set 命中（bump 不再增长）。
    DescriptorFrameSlotPolicy lin;
    lin.Init(kSamplerLinearBase, kSamplerSlotCapacity, kFrameSlots);
    SamplerTableSharingPolicy sharing;
    sharing.Init(16);
    const u32 kA = 3, kB = 5;
    // 帧 0（槽 0）：5 个 set 同内容两调用写入。
    lin.BeginFrame(0);
    std::vector<SharedSetSim> sets(5);
    for (auto& set : sets) {
        set.src = {SamplerTableSharingPolicy::kUnwritten,
                   SamplerTableSharingPolicy::kUnwritten};
        TEST_ASSERT(ResolveSamplerWrite(lin, sharing, set, {{0, kA}}), "f0 call1");
        TEST_ASSERT(ResolveSamplerWrite(lin, sharing, set, {{1, kB}}), "f0 call2");
    }
    TEST_ASSERT(lin.Bump() == 4, "slot0 epoch1: intermediate+final ranges only");
    // 同内容 set 解析到同一区间（共享成立的直接证据）。
    for (const auto& set : sets) {
        TEST_ASSERT(set.snapshotBase == sets[0].snapshotBase,
                    "all sets with identical content resolve to one shared range");
    }
    // 帧 1、2 推进槽，帧 3 回槽 0（纪元 2——槽 0 的全部条目过期）。
    lin.BeginFrame(1);
    lin.BeginFrame(2);
    lin.BeginFrame(3);
    // 5 个 set 依次重新解析（绑定期解析语义：无新写入，按当前处方重解析）：
    // 第一个发现过期并重建，其余命中。
    for (auto& set : sets) {
        TEST_ASSERT(ResolveSamplerWrite(lin, sharing, set, {}), "rebind resolve");
    }
    // 本轮只有终态内容 [kA, kB] 被引用（中间态 [kA, unwritten] 无 set 持有，
    // 不重建）→ 恰好 1 段 2 槽。
    TEST_ASSERT(lin.Bump() == 2,
                "slot0 epoch2: referenced content rebuilt exactly once");
    for (const auto& set : sets) {
        TEST_ASSERT(set.snapshotBase == sets[0].snapshotBase,
                    "after rebuild all sets again share one range");
    }
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
    // R22-S1：sampler 表内容级共享。
    suite->AddTestCase(TestCase("SamplerKeyHashSemantics", TestSamplerKeyHashSemantics));
    suite->AddTestCase(TestCase("SamplerSharingEntryEpochLifecycle", TestSamplerSharingEntryEpochLifecycle));
    suite->AddTestCase(TestCase("SamplerSharingInitStormDedup", TestSamplerSharingInitStormDedup));
    suite->AddTestCase(TestCase("SamplerSharingTwoCallSequenceConverges", TestSamplerSharingTwoCallSequenceConverges));
    suite->AddTestCase(TestCase("SamplerSharingPurgeOnSamplerDestroy", TestSamplerSharingPurgeOnSamplerDestroy));
    suite->AddTestCase(TestCase("SamplerSharingCapacityDegradesNotFails", TestSamplerSharingCapacityDegradesNotFails));
    suite->AddTestCase(TestCase("SamplerSharingRebuildOncePerSlotCycle", TestSamplerSharingRebuildOncePerSlotCycle));
    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterD3D12DescriptorSlotPolicyTests();
    const auto runStats = TestRunner::RunAllSuites();
    return runStats.failedTests == 0 ? 0 : 1;
}
#endif
