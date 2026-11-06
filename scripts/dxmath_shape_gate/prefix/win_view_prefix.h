// ============================================================================
// Windows 视角前缀头（DirectXMath 形状门禁专用，经 clang++ -include 注入）。
//
// 原理（沿 R4-7 / runbook §29-A.5 的既有做法）：
//   1) 先在 Apple 配置（__APPLE__ 仍定义）下把 TU 集用到的标准库/系统头
//      全部预包含——Apple libc++ 头自身依赖 __APPLE__（availability
//      markup/线程 API #error），直接 -U__APPLE__ 编译不可行；
//   2) 然后 #undef __APPLE__/#undef __MACH__，#define _WIN32/_WIN64/_MSC_VER，
//      切到"Windows 视角"；
//   3) 目标 TU 随后 include 的标准头都已命中 include guard，不再以非 Apple
//      配置重解析；其 Windows 分支（_WIN32/_MSC_VER 门内）则被激活并被
//      本目录的垫片头（windows.h/intrin.h/crtdbg.h/DirectXMath.h…）满足。
//
// 注意：这份头属于门禁基础设施，本身不在任何真实构建中。
// ============================================================================
#pragma once

// ---- 第 1 步：Apple 配置下预包含标准头（TU 集 grep 汇总） ----
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <random>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// POSIX 头（macOS SDK 提供、不依赖 __APPLE__ 分支差异；Windows 视角下
// TU 的 POSIX 分支已关闭，这些只为 guard 缓存兜底）
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/stat.h>

// ---- 第 2 步：切换到 Windows 视角 ----
#undef __APPLE__
#undef __MACH__

#ifndef _WIN32
#define _WIN32 1
#endif
#ifndef _WIN64
#define _WIN64 1
#endif
// MSVC 版本号（VS2022 17.6 = 19.6 系列，值 1936）。作用：激活仓内
// `#if defined(_MSC_VER)` 分支（Math.h 的 intrin.h/CRC、EntityAPI/ScriptAPI
// 的 XMFLOAT 分支、DllMain 的 Windows.h 分支等）。
// 注意：编译器本身仍是 Apple clang——__clang__ 保持定义；-fms-extensions
// -fdeclspec 由 run_gate.sh 命令行提供（__declspec/__forceinline 等）。
#ifndef _MSC_VER
#define _MSC_VER 1936
#endif

// ---------------------------------------------------------------------------
// MSVC CRT 补面（Apple 配置预包含的 libc++ / macOS SDK 没有；Windows 分支
// 代码直接使用）。_countof 在 Utilities.h 中仅 !(_WIN64) 时定义，MSVC 由
// <stdlib.h> 提供——此处等价补齐，二者不会同时定义。
// ---------------------------------------------------------------------------
#include <cstddef>

template <typename T, std::size_t N>
constexpr std::size_t _countof(T (&)[N]) noexcept { return N; }

typedef int errno_t;
extern "C++" {
inline errno_t memcpy_s(void* d, std::size_t dsz, const void* s, std::size_t sz) {
    if (!d || !s || sz > dsz) return 22; // EINVAL
    __builtin_memcpy(d, s, sz);
    return 0;
}
inline errno_t memmove_s(void* d, std::size_t dsz, const void* s, std::size_t sz) {
    if (!d || !s || sz > dsz) return 22;
    __builtin_memmove(d, s, sz);
    return 0;
}
inline errno_t strcpy_s(char* d, std::size_t dsz, const char* s) {
    if (!d || !s) return 22;
    std::size_t n = 0;
    while (s[n]) { if (n + 1 >= dsz) return 34; ++n; } // ERANGE
    __builtin_memcpy(d, s, n + 1);
    return 0;
}
}

extern "C" {
inline errno_t _putenv_s(const char* name, const char* value) { (void)name; (void)value; return 0; }
inline int _stricmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        ++a; ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
inline char* _strdup(const char* s) {
    std::size_t n = 0; while (s[n]) ++n;
    char* p = static_cast<char*>(__builtin_malloc(n + 1));
    if (p) __builtin_memcpy(p, s, n + 1);
    return p;
}
}

// MSVC 安全 CRT：引擎（SurfaceCachePass 的 sscanf_s）与 stb_image（fopen_s）
// 在 _MSC_VER 分支里使用。只需声明面可解析，门禁不执行代码。
#include <cstdio>
#define sscanf_s sscanf
inline errno_t fopen_s(FILE** f, const char* name, const char* mode) {
    *f = std::fopen(name, mode);
    return *f ? 0 : 1;
}
