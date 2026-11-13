// ============================================================================
// 仅供 Mac 语法门禁使用的最小 intrin.h 垫片（不是 MSVC <intrin.h>）。
// 覆盖面（按仓内 grep）：
//   - Engine/Utilities/Math.h _MSC_VER 分支：_mm_crc32_u64
//   - Engine/TaskScheduler/TaskScheduler.cpp _WIN32 分支（防御性）：__rdtsc、
//     _mm_pause
//   - Library Internals 示例同款：__cpuid（防御性）
// ============================================================================
#pragma once
#include <cstdint>

// SSE4.2 CRC（x86 真机上由 MSVC intrin.h 提供；门禁只声明形状）
inline unsigned int _mm_crc32_u64(std::uint64_t _CRC, std::uint64_t _V)
{
    // 门禁不生成代码；给一个可编译的占位表达式
    return (unsigned int)(_CRC ^ _V);
}

inline unsigned int _mm_crc32_u8(std::uint32_t _CRC, std::uint8_t _V)
{
    return _CRC ^ (unsigned int)_V;
}

inline unsigned long long __rdtsc()
{
    return 0;
}

inline void _mm_pause()
{
}

inline void __cpuid(int _CPUInfo[4], int _Function_id)
{
    _CPUInfo[0] = _CPUInfo[1] = _CPUInfo[2] = _CPUInfo[3] = 0;
}
