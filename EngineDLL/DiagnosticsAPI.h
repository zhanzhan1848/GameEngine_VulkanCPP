#pragma once

// =============================================================================
// DiagnosticsAPI.h — 日志 L0（Docs/CLI_Agent_Interface_Plan.md §11.2/§11.4）
//
// 依赖倒置边界：EngineDLL（本文件 + DiagnosticsAPI.cpp）持有 LogDispatch 与
// 三个 sink（Console/RingBuffer/Callback）；核心层将来只依赖一个发射函数
// primal::engine_dll::diag::Emit（见 §11.3 轨道 B/C 的重定向），对宿主、
// sink、回调一无所知。宿主（编辑器/CLI/脚本）通过下面的 C ABI 三件套接管
// 日志去向，全部为追加导出，不改动任何既有导出签名。
//
// 日志级别（与引擎 RHIDebug.h 的 DebugLevel 数值一致）：
//   0=TRACE 1=DEBUG 2=INFO 3=WARN 4=ERROR 5=FATAL
// =============================================================================

#include "Common.h"

#include <cstdint>

// 日志级别常量（与引擎 RHIDebug.h 的 DebugLevel 数值一致）。
namespace primal::engine_dll::diag {
inline constexpr std::uint32_t kLevelTrace = 0;
inline constexpr std::uint32_t kLevelDebug = 1;
inline constexpr std::uint32_t kLevelInfo  = 2;
inline constexpr std::uint32_t kLevelWarn  = 3;
inline constexpr std::uint32_t kLevelError = 4;
inline constexpr std::uint32_t kLevelFatal = 5;
} // namespace primal::engine_dll::diag

// 宿主回调。线程契约：在引擎发出日志的线程上同步触发（任意线程）；不得阻塞、
// 不得重入本模块（Install/SetLevel/SetFilter/CopyRecentLogs）；不得抛异常跨越
// C ABI 边界。category/message 生命周期仅限本次调用，宿主需自拷。
using primal_log_callback = void(*)(void* userdata, std::uint32_t level,
                                    const char* category, const char* message);

extern "C" {

// 注册（callback=nullptr 即卸载）。成功返回 1。同一时刻只支持一个回调——
// 多消费者需求由宿主侧扇出，引擎侧保持零状态。
EDITOR_INTERFACE std::uint32_t InstallLogCallback(primal_log_callback callback, void* userdata);

// 级别阈值：低于 level 的日志被丢弃（对全部 sink 生效）。默认 2（INFO）。
EDITOR_INTERFACE void SetLogLevel(std::uint32_t level);

// 类别白名单，逗号分隔（如 "RHI,Asset,Render"）；空串/null = 不过滤。
// 类别与命令命名空间对齐（规划 §11.2：结构化字段是给 LLM 的）。
EDITOR_INTERFACE void SetCategoryFilter(const char* commaSeparatedCategories);

// 黑匣子（RingBufferSink，最近 2048 条，Release 常开）导出。把最多 bufferSize
// 字节的文本（每行一条，"HH:MM:SS.mmm [LEVEL][category] message"）写入 buffer，
// 返回写入字节数；buffer 为空或尺寸不足返回 0（用 bufferSize=0 调用可探询所需
// 大小，返回值为所需字节数）。线程契约：任意线程。
EDITOR_INTERFACE std::uint32_t CopyRecentLogs(char* buffer, std::uint32_t bufferSize);

// FileSink（L2）：统一日志文件 + 按尺寸轮转。path=null/空串关闭；maxBytes=0
// 取默认 8 MiB；maxFiles=0 取默认 3（.log → .1 → .2 …，最旧被覆盖）。
// 宿主在项目打开后调用一次即可；失败仅 stderr 提示，不影响日志其余去向。
EDITOR_INTERFACE void SetLogFile(const char* path, std::uint32_t maxBytes, std::uint32_t maxFiles);

// 同步排空后台 flush 队列（FileSink/宿主回调/事件表）。宿主退出前、以及需要
// "日志已落盘/已送达"保证的断言前调用。线程契约：任意线程（与 worker 互斥）。
EDITOR_INTERFACE void FlushLogQueue();

} // extern "C"

namespace primal::engine_dll::diag {

// 核心层（轨道 B/C 迁移后）唯一的发射入口。没人装回调时它也不是空操作——
// ConsoleSink/RingBuffer 常开。level 取值为文件头注释的 0..5。
void Emit(std::uint32_t level, const char* category, const char* fmt, ...);

// 库加载时（DllMain.cpp constructor/DllMain）把本模块的分发器注入核心的
// primal::diag 插槽——此后核心层的 primal::diag::Emit 直达本模块三 sink。
void RegisterCoreEmitter();

// EngineDLL 每渲染一帧推进一次（FrameAPI.RenderFrame / PipelineRenderFrame*
// 导出内调用）——on_log 的 fields.frame 由此而来。
void AdvanceFrameCounter();

// 崩溃快照（已发布的双缓冲指针，无锁可读；供 CrashHandler.cpp 在信号处理内
// 使用）。布局 [u32 len][text...]，len 为含长度头的总字节数。
const void* GetCrashSnapshot(std::uint32_t* outLen);

// 崩溃最小写入路径（仅 async-signal-safe 调用：open/write/close/int→ascii）。
// pathPrefix 为预计算好的 "目录/primal-crash-" 前缀；signal 为信号编号，
// faultAddr 可为 null。写完调用方负责恢复默认处置并 re-raise。
void WriteCrashDumpMin(const char* pathPrefix, int signal, const void* faultAddr);

// 预计算崩溃转储目录（SetLogFile 的目录优先，否则系统临时目录）。安装崩溃
// 处理器时调用一次；结果为 "…/primal-crash-" 形态（不含 pid，处理时补）。
void PrepareCrashDumpPath();

// 崩溃转储路径前缀（只读；供 CrashHandler 在信号处理内拼接 pid）。
const char* GetCrashPathPrefix();

// 安装致命信号 / Windows SEH 崩溃处理器（DllMain 库加载时调用一次）：
// 写黑匣子快照 → on_crash → 恢复默认处置 re-raise。
void InstallCrashHandler();

} // namespace primal::engine_dll::diag
