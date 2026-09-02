#pragma once

// =============================================================================
// CoreLogStream.h — 裸流迁移的 ostream 兼容形态（CLI 规划 §11.2/§11.3 轨道 C）
//
// 迁移惯例：
//   std::cerr << "[Foo] bar=" << v << std::endl;   →
//   PRIMAL_LOG_STREAM(Error, "RHI") << "[Foo] bar=" << v << std::endl;
//
// 语义（对齐"行为零变化"目标）：
//   · 消息在语句结束（析构）或 std::endl/std::flush 处一次性进入
//     primal::diag::Emit——ConsoleSink 同步直写 stderr，格式带
//     [PRIMAL:LEVEL][category] 前缀（迁移的预期产物）；
//   · 算术/bool/指针走快路径；其余类型回落 ostringstream（与 ostream
//     输出一致，自定义类型带 operator<< 也照常工作）；
//   · 级别映射：cerr → Error，cout → Info。
// =============================================================================

#include "CoreDiag.h"

#include <cstdio>
#include <ostream>
#include <sstream>
#include <string>

namespace primal::log {

class LogStream
{
public:
    LogStream(std::uint32_t level, const char* category)
        : level_(level), category_(category) {}

    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    // 快路径：字符串与字符
    LogStream& operator<<(const char* v) { buffer_ += v; return *this; }
    LogStream& operator<<(char v) { buffer_ += v; return *this; }
    LogStream& operator<<(const std::string& v) { buffer_ += v; return *this; }

    // bool：与 ostream 一致输出 true/false（std::to_string 会给 "1"/"0"）
    LogStream& operator<<(bool v) { buffer_ += v ? "true" : "false"; return *this; }

    // 指针：与 ostream 一致输出 0x… 形态
    LogStream& operator<<(const void* v)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%p", v);
        buffer_ += text;
        return *this;
    }

    // 浮点：%g 与 ostream 默认 6 位有效数字一致
    LogStream& operator<<(float v) { return appendNumber("%g", static_cast<double>(v)); }
    LogStream& operator<<(double v) { return appendNumber("%g", v); }
    LogStream& operator<<(long double v) { return appendNumber("%Lg", v); }

    // 算术类型走 std::to_string；非作用域枚举（VkResult 等无 scoped enum）
    // 与 ostream 一致按整型提升打印——若不单列，枚举到 long/bool/double 等
    // 多个算术重载的转换会产生歧义。（两条件必须合入同一模板：默认模板
    // 参数不参与签名区分，分开写会被判重声明。）
    template <typename T,
              typename = std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>>>
    LogStream& operator<<(T v)
    {
        if constexpr (std::is_enum_v<T>)
            buffer_ += std::to_string(static_cast<long long>(v));
        else
            buffer_ += std::to_string(v);
        return *this;
    }

    // 吞掉流操纵符：std::endl / std::flush 都视为"本条消息结束"。
    LogStream& operator<<(std::ostream& (*)(std::ostream&))
    {
        flush();
        return *this;
    }

    void flush()
    {
        if (flushed_) return;
        flushed_ = true;
        primal::diag::Emit(level_, category_, buffer_.c_str());
    }

    ~LogStream() { flush(); }

private:
    LogStream& appendNumber(const char* format, long double v)
    {
        char text[64];
        std::snprintf(text, sizeof(text), format, v);
        buffer_ += text;
        return *this;
    }

    std::uint32_t level_;
    const char* category_;
    std::string buffer_;
    bool flushed_ = false;
};

} // namespace primal::log

// category 必须显式给出（脚本按目录注入；与命令命名空间对齐，规划 §11.2）。
#define PRIMAL_LOG_STREAM(level, category) \
    ::primal::log::LogStream(::primal::diag::kLevel##level, category)
