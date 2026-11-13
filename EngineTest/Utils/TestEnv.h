/**
 * @file TestEnv.h
 * @brief 测试用环境变量读写的可移植封装
 *
 * R3（Windows 首编）：TestVulkan* 系列单测此前直接调用 POSIX setenv/unsetenv
 * ——MSVC 无此符号（C3861）。本头提供同语义封装：
 *   - POSIX：setenv(name, value, overwrite=1) / unsetenv
 *   - MSVC ：_putenv_s（写 CRT 环境块，与引擎侧 std::getenv 同块）；
 *            置空值在 MSVC CRT 语义下即删除该变量（等价 unsetenv）。
 * 返回值语义对齐：成功 0，失败非 0。
 */
#pragma once

#include <cstdlib>

namespace primaltest {

/// 设置环境变量（覆盖已有值）。成功返回 0。
inline int SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}

/// 删除环境变量（不存在也视为成功）。成功返回 0。
inline int UnsetEnv(const char* name) {
#if defined(_WIN32)
    // MSVC CRT：置空值即从 CRT 环境块移除该变量。
    return _putenv_s(name, "");
#else
    return unsetenv(name);
#endif
}

}  // namespace primaltest
