/**
 * @file D3D12DescriptorSlotPolicy.h
 * @brief 描述符堆帧槽线性分配 + 槽纪元（epoch）策略——平台中立模型
 * @details R21-C（2026-11-05，M1 硬件渐进黑帧诊断轮）。
 *
 *          本类是 D3D12DescriptorHeapManager 两个 shader-visible 堆
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

} // namespace primal::graphics::rhi::d3d12
