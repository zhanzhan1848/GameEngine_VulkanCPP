// 门禁垫片：MSVC CRT 的 <malloc.h>（_aligned_malloc/_alloca 等）。
// 仅供 Mac 语法门禁，按 Windows 视角提供最小声明面。
#pragma once
#include <stdlib.h>
#include <stddef.h>
#ifndef _ALIGNED_MALLOC_SHIM
#define _ALIGNED_MALLOC_SHIM
inline void* _aligned_malloc(size_t size, size_t alignment) { void* p = nullptr; return posix_memalign(&p, alignment, size) == 0 ? p : nullptr; }
inline void _aligned_free(void* p) { free(p); }
#endif
#ifndef _alloca
#define _alloca __builtin_alloca
#endif
