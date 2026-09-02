#pragma once

// =============================================================================
// CrashHandler.h — 崩溃处理挂钩（CLI 规划 §11.2 L2 / §12 on_crash）。
// 实现见 CrashHandler.cpp；InstallCrashHandler 由 DllMain 库加载时调用。
// =============================================================================

#include "Common.h"

namespace primal::engine_dll::diag {

void InstallCrashHandler();

} // namespace primal::engine_dll::diag
