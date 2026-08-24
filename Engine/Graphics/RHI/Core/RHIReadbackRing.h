/**
 * @file RHIReadbackRing.h
 * @brief Fenced 异步回读 ring（VT M0.1）
 * @details N 帧轮转的 Readback buffer + fence 池，纯组合现有 RHI 原语：
 *          CreateBuffer(Readback) / CreateSync / Submit(signalFence) /
 *          WaitForSync / MapBuffer —— 零 per-backend 代码，三后端行为一致。
 *
 * 动机（VT feedback 链路）：GPU 每帧把 feedback/验证数据拷进 ring 的当前
 * slot 并 fence 信号；CPU 在若干帧后按 fence 就绪情况异步 Map 读取。
 * ring 尺寸 ≥ 帧内 in-flight 数，slot 复用前其 fence 已被等待，无竞争。
 *
 * 用法（每帧）：
 *   const u32 slot = ring.slot_for_frame(frameIndex);
 *   cmd: CopyBuffer(src, ring.buffer(slot)) 或 CopyTextureToBuffer(...);
 *   QueueSubmitInfo si{...}; ring.attach_fence(si, slot); device->Submit(si);
 *   // 稍后（通常下一帧或下下帧）：
 *   if (ring.wait(device, slot, timeout_ms)) {
 *       if (void* p = ring.map(device, slot)) { 读 p; ring.unmap(device, slot); }
 *   }
 *
 * 同步语义：
 *   - Vulkan：fence 在 submitImpl 内部 submit 前 reset，重复使用安全；
 *   - Metal：SharedEvent 每次 submit 递增目标值，WaitForSync 等最近信号；
 *   - Dawn：CPU 原子模拟（现有 DawnSync 语义）。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-21
 */

#pragma once

#include "RHITypes.h"

namespace primal::graphics::rhi {

class RHIDeviceBase;

class rhi_readback_ring {
public:
    static constexpr u32 kDefaultRingSize = MAX_FRAMES_IN_FLIGHT;

    rhi_readback_ring() = default;
    ~rhi_readback_ring() { /* 资源由 shutdown 显式释放（需要 device 指针）*/ }

    rhi_readback_ring(const rhi_readback_ring&) = delete;
    rhi_readback_ring& operator=(const rhi_readback_ring&) = delete;
    rhi_readback_ring(rhi_readback_ring&&) = delete;
    rhi_readback_ring& operator=(rhi_readback_ring&&) = delete;

    /// 创建 ringSize 个 Readback buffer（每 slot bufferSize 字节）+ fence。
    /// ringSize 应 ≥ 帧内 in-flight 数（默认 MAX_FRAMES_IN_FLIGHT=3）。
    bool initialize(RHIDeviceBase* device, u64 bufferSize, u32 ringSize = kDefaultRingSize);

    /// 释放全部 buffer/fence。调用前应确保无 in-flight submit 引用本 ring。
    void shutdown(RHIDeviceBase* device);

    bool is_initialized() const { return ringSize_ > 0; }
    u32  ring_size() const { return ringSize_; }
    u64  buffer_size() const { return bufferSize_; }

    /// 帧号 → slot 映射（frameIndex % ringSize）
    u32 slot_for_frame(u32 frameIndex) const { return frameIndex % ringSize_; }

    /// slot 的回读 buffer（作为 CopyBuffer/CopyTextureToBuffer 的目标）
    ResourceHandle buffer(u32 slot) const;

    /// slot 的 fence（调用方 submit 作 signalFence 用）
    SyncHandle fence(u32 slot) const { return slot < ringSize_ ? fences_[slot] : handles::INVALID_SYNC; }

    /// 把 slot 的 fence 挂到提交信息上（GPU 完成信号）
    void attach_fence(QueueSubmitInfo& info, u32 slot) const;

    /// 等待 slot fence 信号（不阻塞渲染线程的轮询用小超时）
    bool wait(RHIDeviceBase* device, u32 slot, u32 timeoutMs) const;

    /// fence 就绪后映射读取；用完 unmap
    void* map(RHIDeviceBase* device, u32 slot);
    void  unmap(RHIDeviceBase* device, u32 slot);

private:
    ResourceHandle buffers_[MAX_FRAMES_IN_FLIGHT] = {
        handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
    SyncHandle fences_[MAX_FRAMES_IN_FLIGHT] = {
        handles::INVALID_SYNC, handles::INVALID_SYNC, handles::INVALID_SYNC};
    u32 ringSize_    = 0;
    u64 bufferSize_  = 0;
};

} // namespace primal::graphics::rhi
