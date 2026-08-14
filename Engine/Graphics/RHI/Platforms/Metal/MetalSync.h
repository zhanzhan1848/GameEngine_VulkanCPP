/**
 * @file MetalSync.h
 * @brief Metal 同步原语实现
 * @details 封装 MTLSharedEvent 和 MTLFence 实现 GPU-CPU 和 GPU-GPU 同步
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

/**
 * @brief Metal 同步对象
 * @details 封装 MTL::SharedEvent 用于 CPU-GPU 和 GPU-GPU 同步
 */
class MetalSync {
public:
    /**
     * @brief 构造函数
     * @param device Metal 设备指针
     */
    explicit MetalSync(MTL::Device* device);

    /**
     * @brief 析构函数
     */
    ~MetalSync();

    /**
     * @brief 获取原生 SharedEvent 对象
     */
    MTL::SharedEvent* GetNativeEvent() const { return event_; }

    /**
     * @brief 获取当前信号值
     * @deprecated 语义模糊(无法区分"读当前值"和"读已 signal 的值")。
     *             新代码使用 GetSignaledValue()(同实现,显式语义)。
     */
    u64 GetValue() const;

    /**
     * @brief 设置信号值 (CPU 端)
     * @deprecated 语义模糊。新代码使用 Signal()(显式 producer 语义)。
     */
    void SetValue(u64 value);

    /**
     * @brief 读取当前 signaled value
     * @details 返回 GPU 最后 ack 的 signaledValue(CPU 设置或 GPU encodeSignalEvent 后的最终值)。
     *          注意:GPU 端 encodeSignalEvent 在 cmdbuf 提交并执行后才推进,
     *          因此本调用在 cmdbuf 未完成时返回的是旧值。
     */
    u64 GetSignaledValue() const;

    /**
     * @brief CPU 端 signal — 设置 signaled value(CPU-visible to GPU)
     * @details 立即生效。GPU 端可通过 encodeWaitForEvent 等待该值。
     *          用于 host 作为 producer 的场景(例如神经渲染 host-side fence)。
     */
    void Signal(u64 value);

    /**
     * @brief CPU 端 wait — 阻塞直到 signaledValue >= value
     * @param value 目标 signaledValue
     * @param timeoutMs 超时(毫秒)。0xFFFFFFFF 视为无限等待。
     * @return true 如果 signaledValue 已达到 value;false 超时
     *
     * @details 使用 MTLSharedEventListener + dispatch_semaphore 实现。
     *          fast path:若当前 signaledValue 已 >= value,立即返回。
     *          注意:不要在 GPU cmdbuf 还在排队但未执行时盲目等待自己将要 encodeSignal 的值
     *          — 那会导致死锁。本调用只等待 signaledValue 的实际推进。
     */
    bool Wait(u64 value, u32 timeoutMs = 0xFFFFFFFFu);

    /**
     * @brief 释放原生事件的所有权
     * @return 原生事件指针，调用者负责释放
     */
    MTL::SharedEvent* DetachNativeEvent();

private:
    MTL::SharedEvent* event_{nullptr}; ///< Metal 共享事件对象
};

} // namespace primal::graphics::rhi
