#pragma once

// =============================================================================
// EngineEventsAPI.h — 宿主事件表（CLI 规划 §12：一张版本化的事件表，引擎→宿主，
// 只观察；一套命令面，宿主→引擎，只修改，两者互不越界）。
//
// 形态：版本化 C 结构体 + 函数指针（Dawn/Godot GDExtension 同构）。字段只追加、
// 永不重排，废弃置 null 保留占位；宿主按 size 截断兼容旧引擎，引擎按 size 截断
// 兼容旧宿主。
//
// 与 InitializeEngine 的关系：规划 §12.1 的终态是 InitializeEngine 直接接收事件表，
// 但那是破坏性 ABI 变更（会打断所有既有宿主的 P/Invoke 镜像）；v1 采用追加导出
// InstallEngineEvents（在 InitializeEngine 之前任意时刻安装均可），签名迁移留待
// 评审后的大版本一并做。
//
// 线程契约（§12.3）：
//   · on_log        —— 引擎发出日志的线程上同步触发（任意线程）；轻量、不阻塞、
//                      不重入诊断模块；字段指针生命周期仅限本次调用；
//   · on_device_lost —— 上报线程（Dawn 为 GPU 线程的自发回调，任意线程）；
//   · on_assert/on_crash —— 预留（v1 未接线，接线后 crash 类必须 async-signal-safe）；
//   · 所有回调不得跨 C ABI 边界抛异常。
// =============================================================================

#include "Common.h"

#include <cstdint>
#include <cstddef>

extern "C" {

inline constexpr std::uint32_t PRIMAL_EVENTS_V1 = 1u;

// on_log 的结构化附加字段（v1：帧号 + 线程 id）。frame=0 表示与帧无关
// （初始化/资产等）；thread_id 为 OS 线程标识，供"UI 线程 vs 后台线程"归因。
typedef struct primal_log_fields {
    std::uint64_t frame;
    std::int64_t  thread_id;
} primal_log_fields;

// on_crash 的载荷：崩溃原因 + 黑匣子（RingBuffer 尾部）导出。v1 预留。
typedef struct primal_crash_info {
    const char*   reason;        // 可读崩溃原因，可能为 null
    const char*   blackbox;      // 黑匣子文本（"HH:MM:SS.mmm [LEVEL][category] msg" 每行一条），可能为 null
    std::uint32_t blackbox_len;  // blackbox 字节数（不含结尾 NUL）
} primal_crash_info;

typedef struct primal_engine_events_v1 {
    // === 头（按 size 截断兼容；字段只追加、永不重排）===
    std::uint32_t version;   // PRIMAL_EVENTS_V1
    std::size_t   size;      // 宿主侧 sizeof(primal_engine_events_v1)
    void*         userdata;  // 原样传回每个回调的第一个参数

    void (*on_log)(void* ud, std::uint32_t level, const char* category,
                   const char* message, const primal_log_fields* fields);
    void (*on_assert)(void* ud, const char* file, int line, const char* condition);   // v1 预留
    void (*on_crash)(void* ud, const primal_crash_info* info);                        // v1 预留
    void (*on_progress)(void* ud, const char* operation, int done, int total);        // v1 预留
    void (*on_device_lost)(void* ud, const char* backend, const char* detail);
} primal_engine_events_v1;

static_assert(sizeof(primal_engine_events_v1) == 64,
              "primal_engine_events_v1 ABI layout drifted — fields must only be appended (§12.3)");

// 安装/替换（events=nullptr 即卸载）宿主事件表。引擎只拷贝 min(size, 自身 size)
// 覆盖的字段，宿主用旧引擎遇到新字段时安全截断；version 不识别时返回 0（调用方
// 应按"事件不可用"降级，而不是重试）。同一时刻一张表——多消费者由宿主扇出。
EDITOR_INTERFACE std::uint32_t InstallEngineEvents(const primal_engine_events_v1* events);

} // extern "C"

namespace primal::engine_dll::events {

// DiagnosticsAPI 的 flush 线程转发：每条日志（已达级别/类别过滤）都会走这里。
// frame 为 EngineDLL 帧计数（RenderFrame 导出推进；0 = 与帧无关）。
void FireLog(std::uint32_t level, const char* category, const char* message, std::uint64_t frame);

// 设备丢失上报。v1 由诊断分发按消息标记识别（Dawn 轨道 B 重定向、Vulkan
// DEVICE_LOST 站点），Metal 在 macOS 无该语义；探测点全部接入后标记识别退役。
void FireDeviceLost(const char* backend, const char* detail);

// 崩溃上报（CrashHandler.cpp 在信号处理函数内调用；宿主按 §12.3 承诺
// async-signal-safe）。info 的 blackbox 指向崩溃快照，仅在本次调用内有效。
void FireCrash(const primal_crash_info* info);

} // namespace primal::engine_dll::events
