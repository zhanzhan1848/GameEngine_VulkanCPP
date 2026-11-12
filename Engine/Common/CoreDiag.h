#pragma once

// =============================================================================
// CoreDiag.h — 核心层唯一的日志出口（CLI 规划 §11.2 的依赖倒置插槽）
//
// 核心层只认识这个函数指针插槽，不知道 EngineDLL、宿主、sink 的存在：
//   · EngineDLL 加载时通过 SetEmitter 注入真正的分发器（Console/RingBuffer/
//     Callback 三 sink 都住在 EngineDLL）；
//   · Engine 被独立加载（纯引擎测试/工具）时无人注入 → Emit 是空操作；
//   · 线程契约：任意线程可调；实现方保证 emit 回调轻量、不重入、不抛异常。
//
// 注意：本头文件必须保持零依赖（不用 CommonHeaders.h 的 u32 等，避免
// 头被牵进任何平台层），只依赖 <atomic>/<cstdint>/<cstdio>（cstdio 仅为
// 无发射器时的 stderr 回退，见 Emit 注释）。
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace primal::diag {

// 与 EngineDLL/DiagnosticsAPI.h 的级别枚举一致：0=TRACE..5=FATAL
inline constexpr std::uint32_t kLevelTrace = 0;
inline constexpr std::uint32_t kLevelDebug = 1;
inline constexpr std::uint32_t kLevelInfo  = 2;
inline constexpr std::uint32_t kLevelWarn  = 3;
inline constexpr std::uint32_t kLevelError = 4;
inline constexpr std::uint32_t kLevelFatal = 5;

using EmitFn = void (*)(void* userdata, std::uint32_t level,
                        const char* category, const char* message);

namespace detail {
inline std::atomic<EmitFn> g_emitFn{nullptr};
inline std::atomic<void*>  g_emitUserdata{nullptr};
} // namespace detail

// EngineDLL 在库加载时注入；宿主替换/卸载也走这里。供 EngineDLL 调用，
// 核心其余代码不要调用。
inline void SetEmitter(EmitFn fn, void* userdata)
{
    detail::g_emitUserdata.store(userdata, std::memory_order_release);
    detail::g_emitFn.store(fn, std::memory_order_release);
}

// 发射一条结构化日志。category 与命令命名空间对齐（RHI/Asset/Scene/Render…）。
// message 应为已格式化的完整文本（核心侧不做 printf，调用点已有缓冲先例）。
// 无发射器时的回退（轨道 C"行为零变化"的关键）：纯引擎二进制（集成测试/
// 独立工具）不加载 EngineDLL → 无人注入发射器 → 此时直写裸文本到 stderr，
// 与迁移前裸流的输出一致；EngineDLL 在场时走完整管线（ConsoleSink 带级别
// 前缀 + 黑匣子 + 宿主回调/事件表），不产生双份输出。
inline void Emit(std::uint32_t level, const char* category, const char* message)
{
    if (auto* fn = detail::g_emitFn.load(std::memory_order_acquire))
        fn(detail::g_emitUserdata.load(std::memory_order_relaxed), level, category, message);
    else if (message != nullptr)
        std::fprintf(stderr, "%s\n", message);
}

// R9-WIN fail-fast（Windows Vulkan tonemap 事故 2026-11-12）：「帧输出必需的
// 管线 INVALID」不允许静默跳过后死等 fence/vkAcquireNextImageKHR 到 watchdog。
// 语义与 tvsrg watchdog（r7，std::_Exit(7)）一致：诊断直写 stderr + flush +
// std::_Exit —— 不跑静态析构、不触碰可能已挂起的 GPU 资源；从任意线程调用
// 均直接终结进程。调用方负责在 message 里写全定位信息（pass/入口/路径/字节）。
// 退出码 8：避开既有占用 0=成功 / 2=tvsrg 诊断 / 3=启动器无 PPM /
// 4,5=primal CLI / 7=watchdog（Windows wrapper 语义，见 r9 报告）。
inline constexpr int kFatalPipelineExitCode = 8;

inline void FatalPipelineExit(const char* message)
{
    if (message != nullptr) std::fprintf(stderr, "%s\n", message);
    std::fflush(stderr);
    std::_Exit(kFatalPipelineExitCode);
}

} // namespace primal::diag
