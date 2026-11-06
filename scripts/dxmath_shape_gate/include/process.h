// ============================================================================
// 仅供 Mac 语法门禁使用的最小 process.h 垫片。
// 覆盖仓内用法：EngineTest/UnitTests/Graphics/TestSCSurfacePages.cpp 的
// _getpid。
// ============================================================================
#pragma once

inline unsigned int _getpid()
{
    return 0;
}

inline void _endthreadex(unsigned int) {}
typedef void (__attribute__((stdcall)) *_beginthreadex_proc_t)(void*);
inline unsigned long _beginthreadex(void*, unsigned int,
                                    _beginthreadex_proc_t, void*,
                                    unsigned int, unsigned int*)
{
    return 0;
}
