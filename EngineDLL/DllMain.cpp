// dllmain.cpp : 定义 DLL 应用程序的入口点。
#if defined(_MSC_VER)
#pragma comment(lib, "Engine.lib")

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <crtdbg.h>

// RegisterCoreEmitter/InstallCrashHandler 的声明（两者都在此头，L74/L98）。
// R13-B：MSVC 分支此前未包含任何声明头 → C2653/C3861；clang 分支本就包含。
#include "DiagnosticsAPI.h"

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
#if _DEBUG
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);// 检测内存泄露
#endif
        // 日志 L1：注入核心诊断插槽（DLL_PROCESS_ATTACH 单线程一次性，安全）。
        primal::engine_dll::diag::RegisterCoreEmitter();
        primal::engine_dll::diag::InstallCrashHandler();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

#elif defined(__clang__)
#include <iostream>
#include <crt_externs.h>
#include <stdlib.h>

#include "DiagnosticsAPI.h"
#include "Engine/Common/CoreLogStream.h"

#if _DEBUG
// 在 Clang 中实现内存泄漏检测的代码
#include <malloc/malloc.h>
#endif

__attribute__((constructor)) static void library_init()
{
    // 日志 L1（CLI 规划 §11.2/§11.4 轨道 B）：库加载即把本模块的诊断分发器
    // 注入核心插槽——核心层的 primal::diag::Emit 由此直达三 sink。
    primal::engine_dll::diag::RegisterCoreEmitter();
    primal::engine_dll::diag::InstallCrashHandler();

    PRIMAL_LOG_STREAM(Error, "Engine") << "Library is being loaded." << std::endl;
#if _DEBUG
    // 在 Clang 中实现内存泄漏检测的代码
    // 在调试模式下启用内存泄漏检查
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats); // 获取内存状态
    PRIMAL_LOG_STREAM(Error, "Engine") << "Memory leak check enabled." << std::endl;
#endif
}

__attribute__((destructor)) static void library_fini()
{
    PRIMAL_LOG_STREAM(Error, "Engine") << "Library is being unloaded." << std::endl;

#if _DEBUG
    // 进行内存泄漏检查，macOS有内建的检测工具
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats);
    PRIMAL_LOG_STREAM(Error, "Engine") << "Memory leak check completed." << std::endl;
#endif
}

#endif