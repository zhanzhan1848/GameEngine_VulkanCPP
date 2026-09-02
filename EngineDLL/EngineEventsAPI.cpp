// =============================================================================
// EngineEventsAPI.cpp — 宿主事件表实现（CLI 规划 §12）。
//
// v1 接线范围：
//   · on_log         —— 与 InstallLogCallback 的 CallbackSink 并行，全部日志锁外触发；
//   · on_device_lost —— 诊断分发按消息标记识别（Dawn 探测点已在轨道 B 重定向）；
//   · on_assert / on_crash / on_progress —— 表内占位，v1 不触发（见头文件线程契约）。
// =============================================================================

#include "EngineEventsAPI.h"

#include "DiagnosticsAPI.h"

#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace primal::engine_dll::events {

namespace {

std::mutex g_mutex;
primal_engine_events_v1 g_events{};
bool g_installed = false;

} // anonymous namespace

void FireLog(std::uint32_t level, const char* category, const char* message, std::uint64_t frame)
{
    primal_engine_events_v1 local{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed || g_events.on_log == nullptr) return;
        local = g_events;
    }

    primal_log_fields fields{};
    fields.frame = frame; // EngineDLL 每渲染一帧推进（FrameAPI/PipelineRenderFrame 导出）
    fields.thread_id = static_cast<std::int64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    local.on_log(local.userdata, level, category, message, &fields);
}

void FireDeviceLost(const char* backend, const char* detail)
{
    primal_engine_events_v1 local{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installed || g_events.on_device_lost == nullptr) return;
        local = g_events;
    }
    local.on_device_lost(local.userdata, backend, detail);
}

void FireCrash(const primal_crash_info* info)
{
    // 崩溃线程上下文（§12.3：宿主承诺 async-signal-safe）。这里只做表拷贝与
    // 单次间接调用，无锁——g_events 装好后只读；安装/卸载与崩溃竞争的窗口
    // 极小且崩溃语义下可接受。
    if (!g_installed || g_events.on_crash == nullptr || info == nullptr) return;
    g_events.on_crash(g_events.userdata, info);
}

} // namespace primal::engine_dll::events

extern "C" {

EDITOR_INTERFACE std::uint32_t InstallEngineEvents(const primal_engine_events_v1* events)
{
    using namespace primal::engine_dll::events;
    std::lock_guard<std::mutex> lock(g_mutex);

    if (events == nullptr)
    {
        g_installed = false;
        std::memset(&g_events, 0, sizeof(g_events));
        return 1;
    }
    if (events->version != PRIMAL_EVENTS_V1 || events->size < sizeof(primal_engine_events_v1))
        return 0; // 未知版本/小于本引擎认知的表：按"事件不可用"处理，调用方降级

    // 按宿主声明的 size 截断拷贝（§12.3）：本引擎认知的 v1 是 64 字节；宿主
    // 传来更大的表（未来的 v1.x 字段）时多余字段安全忽略。
    std::memset(&g_events, 0, sizeof(g_events));
    std::memcpy(&g_events, events, sizeof(primal_engine_events_v1));
    g_installed = true;
    return 1;
}

} // extern "C"
