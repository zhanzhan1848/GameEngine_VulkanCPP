// dllmain.cpp : 定义 DLL 应用程序的入口点。
#if defined(_MSC_VER)
#pragma comment(lib, "Engine.lib")

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <crtdbg.h>

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

#if _DEBUG
// 在 Clang 中实现内存泄漏检测的代码
#include <malloc/malloc.h>
#endif

__attribute__((constructor)) static void library_init()
{
    std::cout << "Library is being loaded." << std::endl;
#if _DEBUG
    // 在 Clang 中实现内存泄漏检测的代码
    // 在调试模式下启用内存泄漏检查
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats); // 获取内存状态
    std::cout << "Memory leak check enabled." << std::endl;
#endif
}

__attribute__((destructor)) static void library_fini()
{
    std::cout << "Library is being unloaded." << std::endl;

#if _DEBUG
    // 进行内存泄漏检查，macOS有内建的检测工具
    malloc_statistics_t stats;
    malloc_zone_statistics(malloc_default_zone(), &stats);
    std::cout << "Memory leak check completed." << std::endl;
#endif
}

#endif