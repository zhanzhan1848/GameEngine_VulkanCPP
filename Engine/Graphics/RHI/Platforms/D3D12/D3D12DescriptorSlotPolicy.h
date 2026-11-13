/**
 * @file D3D12DescriptorSlotPolicy.h
 * @brief 描述符堆帧槽线性分配 + 槽纪元（epoch）策略 + sampler 表内容级
 *        共享记账——平台中立模型
 * @details R21-C（2026-11-05，M1 硬件渐进黑帧诊断轮）：DescriptorFrameSlotPolicy。
 *          R22-S1（2026-11-10，D3D12 sampler 线性区耗尽修复）：
 *          SamplerTableSharingPolicy（见该类注）。
 *
 *          两个策略类都是 D3D12DescriptorHeapManager 两个 shader-visible 堆
 *          （CBV/SRV/UAV 主堆线性区 + sampler 堆线性区）分配策略的
 *          单一事实源：Windows 生产 TU（D3D12DescriptorHeaps.cpp）直接
 *          持有本类实例，macOS 单测（TestD3D12DescriptorSlotPolicy.cpp）
 *          直接测本类——策略与测试同源，不会漂移。无任何 Windows 头，
 *          所有平台可编译（对齐 D3D12StateMapping/D3D12RootSignatureBudget
 *          中立核先例）。
 *
 *          策略语义（与 D3D12DescriptorHeaps.h 文件头 F1-5 契约一致）：
 *          - 堆布局 = [linearBase_, linearBase_ + slotCapacity_*frameSlotCount_)
 *            的帧槽线性区；每槽子区域互不重叠。
 *          - BeginFrame(frameSlot)：currentSlot_ 推进、该槽 bump 归零、
 *            该槽 epoch 自增。epoch 是槽区域的"纪元"——一轮 BeginFrame
 *            即宣告该槽上一轮全部线性分配作废（内容已被回收，重分配
 *            将从区域头开始覆写）。
 *          - Alloc(count)：当前槽内线性 bump；耗尽显式失败（false），
 *            绝不静默回绕（回绕=覆写在飞描述符）。
 *          - set 区段时效判定 RegionIsCurrent(ownerSlot, ownerEpoch)：
 *            set 在分配时记下 (slot, epoch) 戳；戳与当前 (slot, epoch)
 *            完全一致才算"区段仍有效"。R21C 修复点：旧判定只比
 *            ownerSlot == currentSlot——set 恰好隔一整个槽周期
 *            （MAX_FRAMES_IN_FLIGHT=3 帧）未被触碰时，同槽回归会通过
 *            旧判定，但其区段早已被该槽新一轮 BeginFrame 回收并分配给
 *            其他 set → 两个 set 共享同一段槽位 → 描述符互相覆写
 *            （黑帧/错图根因族）。epoch 把"同槽"细化为"同槽同纪元"，
 *            陈旧区段必然触发迁移。
 *
 *          epoch 类型 u64：BeginFrame 每帧一次自增，2^64 帧内不回绕。
 */
#pragma once

#include <cstdint>
#include <vector>

// u32/u64（自包含；对齐 D3D12Abi.h 的 "Common/PrimitiveTypes.h" 引法，
// Engine/ 与 Engine/Common/ 两个 include 根由构建系统注入）。
#include "Common/PrimitiveTypes.h"

namespace primal::graphics::rhi::d3d12 {

class DescriptorFrameSlotPolicy {
public:
    static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

    /// 线性区布局初始化（线性区总容量 = slotCapacity × frameSlotCount）。
    void Init(u32 linearBase, u32 slotCapacity, u32 frameSlotCount) {
        linearBase_ = linearBase;
        slotCapacity_ = slotCapacity;
        frameSlotCount_ = frameSlotCount == 0 ? 1 : frameSlotCount;
        currentSlot_ = 0;
        bump_ = 0;
        epoch_.assign(frameSlotCount_, 0);
        highWater_.assign(frameSlotCount_, 0);
        lastCycleHigh_.assign(frameSlotCount_, 0);
    }

    /// 帧槽推进：槽切换 + bump 归零 + 该槽纪元自增（旧纪元分配全部作废）。
    void BeginFrame(u32 frameSlot) {
        currentSlot_ = frameSlot % frameSlotCount_;
        // 进入槽的 bump_ 仍保留其上一轮终值（该槽自上轮未被触碰）——
        // 先快照为"上一轮峰值"再归零（FRAME_TRACE 逐帧消耗量对账用）。
        lastCycleHigh_[currentSlot_] = bump_;
        bump_ = 0;
        ++epoch_[currentSlot_];
    }

    /// 当前槽线性分配 count 个槽位；容量耗尽返回 false（不回绕）。
    bool Alloc(u32 count, u32& outBaseSlot) {
        if (u64(bump_) + count > slotCapacity_) return false;
        outBaseSlot = SlotBase() + bump_;
        bump_ += count;
        if (bump_ > highWater_[currentSlot_]) highWater_[currentSlot_] = bump_;
        return true;
    }

    /// set 区段时效判定：分配戳 (ownerSlot, ownerEpoch) 与当前完全一致才有效。
    /// 陈旧戳（含"同槽但旧纪元"）返回 false → 调用方必须迁移区段。
    bool RegionIsCurrent(u32 ownerSlot, u64 ownerEpoch) const {
        return ownerSlot == currentSlot_ && ownerEpoch == SlotEpoch(currentSlot_);
    }

    // ---- 查询（生产侧句柄换算 / 单测断言 / FRAME_TRACE 埋点共用）----
    u32 CurrentSlot() const { return currentSlot_; }
    u32 SlotIndexOf(u32 frameSlot) const { return frameSlot % frameSlotCount_; }
    u64 SlotEpoch(u32 slot) const {
        return slot < frameSlotCount_ ? epoch_[slot] : 0;
    }
    u32 Bump() const { return bump_; }
    u32 SlotCapacity() const { return slotCapacity_; }
    u32 LinearBase() const { return linearBase_; }
    u32 SlotBase() const { return linearBase_ + currentSlot_ * slotCapacity_; }
    u32 SlotBaseFor(u32 slot) const {
        return linearBase_ + (slot % frameSlotCount_) * slotCapacity_;
    }
    /// 指定槽历史最大 bump（high-water；越界槽返回 0）。
    u32 HighWaterFor(u32 slot) const {
        return slot < frameSlotCount_ ? highWater_[slot] : 0;
    }
    /// 指定槽上一轮（纪元）的最终 bump——逐帧实际消耗量（越界槽返回 0）。
    u32 LastCycleHighFor(u32 slot) const {
        return slot < frameSlotCount_ ? lastCycleHigh_[slot] : 0;
    }
    u32 FrameSlotCount() const { return frameSlotCount_; }

private:
    u32 linearBase_ = 0;       ///< 线性区起始槽位（堆内绝对坐标）
    u32 slotCapacity_ = 0;     ///< 每槽子区域容量
    u32 frameSlotCount_ = 1;   ///< 帧槽数（= MAX_FRAMES_IN_FLIGHT）
    u32 currentSlot_ = 0;      ///< 当前帧槽
    u32 bump_ = 0;             ///< 当前槽内 bump 偏移
    std::vector<u64> epoch_;       ///< 每槽纪元（BeginFrame 自增）
    std::vector<u32> highWater_;   ///< 每槽历史峰值 bump（诊断）
    std::vector<u32> lastCycleHigh_; ///< 每槽上一轮终值 bump（逐帧对账）
};

// ============================================================================
// SamplerTableSharingPolicy — sampler 表内容级共享记账（R22-S1，2026-11-10
// D3D12 sampler 线性区耗尽修复；平台中立，macOS 单测同源——沿 R21-C 先例）。
//
// 根因（Windows b115034 链，GTX1060，393-mesh 声明 Sponza）：set 实例的
// sampler 表区间按 set 各自 AllocSamplerRange 一段——init 期全部落 slot 0，
// 每 mesh material set 1 个 + lit set 2 个 sampler 描述符，约 170 mesh 即
// 打满每槽 512（(2048-512)/3）线性容量 → "sampler range capacity exhausted
// (bump=512 need=1 capacity=512)" → material set 创建失败 exit 1。而场景里
// 所有材质 set 的 sampler 表内容实际只有极少数唯一状态（宿主用同一批共享
// SamplerHandle 写入）——按 set 重复占段是纯浪费。
//
// 策略：内容寻址共享。内容 =（表长 + 每表项的 sampler 持久槽位或"未写"哨兵）
// 的完整序列（sampler 持久槽与 SamplerDesc 一一对应——每个 D3D12Sampler 由
// 单个 desc 创建且不可变，持久槽即内容身份）。对该序列做 64 位 FNV 键；
// 键相同**且序列精确相等**（防哈希碰撞——键只做查找加速，不做身份证明）的
// set 共享同一段线性区间。区间条目自带 (ownerSlot, ownerEpoch) 双键时效戳
// （与 R21-C set 区段同款语义）：
//   - 时效判定按**条目自身**的 (slot, epoch)，不随引用它的 set 走——多 set
//     引用同一段时，只有段自己的戳说了算；
//   - "谁发现过期谁重建"：帧槽轮转后第一个访问该内容的 set 触发按条目
//     srcSlots 处方在当前槽重建（每内容每槽只分配一次），后来者直接命中；
//   - 条目内容按键**不可变**：set 想写入不同内容（改变某表项的 sampler）
//     必须先撤回（detach）到私有区间，共享段永不被就地改写；
//   - sampler 持久槽释放（SamplerHandle 销毁）时 purge 引用该槽的全部条目
//     ——防"处方引用的持久槽被 free-list 复用成不同采样器后，重建拷错源"
//     （引用已销毁 sampler 的 set 属 Vulkan 同款 UB：spec 禁止销毁仍被
//     descriptor set 引用的 sampler）。
//
// 本类只做记账（键/条目/槽/纪元），不碰任何 D3D12 类型；描述符拷贝与
// 线性分配由 D3D12DescriptorHeapManager（Windows TU）执行。
// ============================================================================

// ---- 内容键哈希（FNV-1a 64）与精确相等比较（平台中立，单测同源）----

/// 逐 u32 混入内容键。
inline u64 MixSamplerKey(u64 h, u32 v) {
    h ^= static_cast<u64>(v);
    h *= 0x100000001B3ull;
    return h;
}

/// sampler 表内容键：表长 + 逐表项（持久槽或 kUnwritten）。
/// 键只做查找加速；身份证明永远是 SameSamplerTable 的精确相等。
inline u64 HashSamplerTable(const u32* srcSlots, u32 count) {
    u64 h = 0xcbf29ce484222325ull;
    h = MixSamplerKey(h, count);
    for (u32 i = 0; i < count; ++i) h = MixSamplerKey(h, srcSlots[i]);
    return h;
}

/// 两张 sampler 表内容精确相等（含"未写"哨兵的逐位比较）。
inline bool SameSamplerTable(const u32* a, const u32* b, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

class SamplerTableSharingPolicy {
public:
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;
    /// 表项"未写入"哨兵（= 持久槽位的 kInvalidSlot——真实持久槽恒小于容量，
    /// 该值不会与任何已分配持久槽混淆）。
    static constexpr u32 kUnwritten = 0xFFFFFFFFu;

    struct Entry {
        bool active = false;   ///< false = 空闲槽（可被 Register 复用）
        u64 key = 0;           ///< 内容键（HashSamplerTable）
        u32 baseSlot = kInvalidIndex; ///< 线性区间基槽（堆内绝对坐标）
        u32 ownerSlot = kInvalidIndex;///< 条目时效戳：属主帧槽
        u64 ownerEpoch = 0;    ///< 条目时效戳：属主纪元（R21-C 双键语义）
        u32 count = 0;         ///< 表长（= layout 的 SamplerCount）
        std::vector<u32> srcSlots;   ///< 重建处方：每表项的 sampler 持久槽/kUnwritten
    };

    void Init(u32 maxEntries) { entries_.assign(maxEntries ? maxEntries : 1, Entry{}); }

    /// 当前 (slot, epoch) 下该内容键的**活跃**条目（键匹配 + 序列精确相等）。
    /// 命中 = 调用方可直接共享 entry.baseSlot（该区间内容即所求，无需再写）。
    u32 FindActive(u64 key, u32 slot, u64 epoch,
                   const u32* srcSlots, u32 count) const {
        for (u32 i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.active && e.key == key && e.ownerSlot == slot
                && e.ownerEpoch == epoch && e.count == count
                && SameSamplerTable(e.srcSlots.data(), srcSlots, count)) {
                return i;
            }
        }
        return kInvalidIndex;
    }

    /// 该键的任意条目（含过期）——过期条目是"重建处方"的持有者：槽轮转后
    /// 按它的 srcSlots 重建并 Activate，不必新建条目（键的处方不变）。
    u32 FindByKey(u64 key, const u32* srcSlots, u32 count) const {
        for (u32 i = 0; i < entries_.size(); ++i) {
            const Entry& e = entries_[i];
            if (e.active && e.key == key && e.count == count
                && SameSamplerTable(e.srcSlots.data(), srcSlots, count)) {
                return i;
            }
        }
        return kInvalidIndex;
    }

    /// 注册（内容 → 区间）条目。同键条目存在则原位刷新（处方恒同——键 +
    /// 精确相等已证内容一致）；否则占一个空闲/从未用过槽；表满返回
    /// kInvalidIndex（调用方降级为私有区间，不共享不失败）。
    u32 Register(u64 key, u32 baseSlot, u32 slot, u64 epoch,
                 const u32* srcSlots, u32 count) {
        u32 freeIndex = kInvalidIndex;
        for (u32 i = 0; i < entries_.size(); ++i) {
            Entry& e = entries_[i];
            if (!e.active) {
                if (freeIndex == kInvalidIndex) freeIndex = i;
                continue;
            }
            if (e.key == key && e.count == count
                && SameSamplerTable(e.srcSlots.data(), srcSlots, count)) {
                e.baseSlot = baseSlot;
                e.ownerSlot = slot;
                e.ownerEpoch = epoch;
                e.srcSlots.assign(srcSlots, srcSlots + count);
                return i;
            }
        }
        if (freeIndex == kInvalidIndex) return kInvalidIndex;
        Entry& e = entries_[freeIndex];
        e.active = true;
        e.key = key;
        e.baseSlot = baseSlot;
        e.ownerSlot = slot;
        e.ownerEpoch = epoch;
        e.count = count;
        e.srcSlots.assign(srcSlots, srcSlots + count);
        return freeIndex;
    }

    /// 重建落位：条目（FindByKey 取到的同键处方持有者）指向新区间 + 新戳。
    void Activate(u32 index, u32 baseSlot, u32 slot, u64 epoch) {
        if (index < entries_.size()) {
            entries_[index].baseSlot = baseSlot;
            entries_[index].ownerSlot = slot;
            entries_[index].ownerEpoch = epoch;
        }
    }

    /// sampler 持久槽释放：purge 处方引用该槽的全部条目（防 free-list 复用
    /// 后重建拷错源，见类注）。条目槽位回收为空闲（可被新内容 Register）。
    void PurgeSlotsReferencing(u32 persistentSlot) {
        for (Entry& e : entries_) {
            if (!e.active) continue;
            for (u32 s : e.srcSlots) {
                if (s == persistentSlot) {
                    e = Entry{};  // 归零 = active=false，空闲可复用
                    break;
                }
            }
        }
    }

    /// 条目区间基槽（FindActive/FindByKey 命中后读基槽用；越界 = kInvalidIndex）。
    u32 EntryBaseSlot(u32 index) const {
        return index < entries_.size() ? entries_[index].baseSlot : kInvalidIndex;
    }

    // ---- 诊断（R22-S1 init 统计日志：唯一内容数 / 活跃共享区间数）----
    /// 注册过的唯一内容键数（含过期条目——处方仍被同键重建复用）。
    u32 UniqueKeyCount() const {
        u32 n = 0;
        for (const Entry& e : entries_) if (e.active) ++n;
        return n;
    }
    /// 指定 (slot, epoch) 下活跃的共享区间数（= 该槽实际被共享占用的线性段数）。
    u32 ActiveEntryCount(u32 slot, u64 epoch) const {
        u32 n = 0;
        for (const Entry& e : entries_)
            if (e.active && e.ownerSlot == slot && e.ownerEpoch == epoch) ++n;
        return n;
    }
    u32 Capacity() const { return static_cast<u32>(entries_.size()); }

private:
    std::vector<Entry> entries_;
};

} // namespace primal::graphics::rhi::d3d12
