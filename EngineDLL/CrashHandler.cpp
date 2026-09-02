// =============================================================================
// CrashHandler.cpp — 崩溃处理挂钩（CLI 规划 §11.2 L2 / §12 on_crash）
//
// 崩溃时（致命信号 / Windows SEH）：
//   1. 把已发布的崩溃快照（黑匣子最近 64KB）写入 <日志目录|临时目录>/primal-crash-<pid>；
//   2. 触发事件表 on_crash（§12.3：宿主承诺 async-signal-safe）；
//   3. 恢复默认处置并 re-raise——OS 崩溃报告照常工作。
//
// 处理函数内只允许 async-signal-safe 原语：WriteCrashDumpMin 已按此约束实现
// （open/write/close + 手工格式化），本文件的处理函数只做参数转发。
// =============================================================================

#include "DiagnosticsAPI.h"
#include "EngineEventsAPI.h"

#include <cstring>
#include <initializer_list>

#if !defined(_WIN32)
#include <csignal>
#include <sys/signal.h>
#else
#include <windows.h>
#endif

namespace primal::engine_dll::diag {

#if !defined(_WIN32)
void OnFatalSignal(int signal, siginfo_t* info, void*)
{
    WriteCrashDumpMin(GetCrashPathPrefix(), signal, info != nullptr ? info->si_addr : nullptr);
    // 恢复默认处置并重发——核心转储/崩溃报告链路不受影响。
    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    sigaction(signal, &restore, nullptr);
    raise(signal);
}
#else
LONG WINAPI OnFatalException(EXCEPTION_POINTERS* info)
{
    WriteCrashDumpMin(GetCrashPathPrefix(),
                      info && info->ExceptionRecord ? static_cast<int>(info->ExceptionRecord->ExceptionCode) : 0,
                      info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    return EXCEPTION_CONTINUE_SEARCH; // 交回系统 WER 流程
}
#endif

void InstallCrashHandler()
{
    PrepareCrashDumpPath(); // 默认临时目录（SetLogFile 后会随日志目录刷新）
#if !defined(_WIN32)
    struct sigaction action{};
    action.sa_sigaction = &OnFatalSignal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        sigaction(sig, &action, nullptr);
#else
    SetUnhandledExceptionFilter(&OnFatalException);
#endif
}

} // namespace primal::engine_dll::diag
