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
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>

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

    // std::thread::id：与事件表 fields.thread_id 同一 hash 形态
    LogStream& operator<<(std::thread::id v)
    {
        buffer_ += std::to_string(std::hash<std::thread::id>{}(v));
        return *this;
    }

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
            appendInteger(static_cast<long long>(v));
        else if constexpr (std::is_integral_v<T>)
        {
            if constexpr (std::is_signed_v<T>)
                appendInteger(static_cast<long long>(v));
            else
                appendInteger(static_cast<unsigned long long>(v));
        }
        else
            buffer_ += std::to_string(v);
        return *this;
    }

    // ios_base 级操纵符：std::hex / std::dec / std::oct（粘性，与 ostream 一致）。
    LogStream& operator<<(std::ios_base& (*manip)(std::ios_base&))
    {
        if (manip == &std::hex) base_ = 16;
        else if (manip == &std::dec) base_ = 10;
        else if (manip == &std::oct) base_ = 8;
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

    void appendInteger(long long v)
    {
        char text[32];
        switch (base_)
        {
        case 16: std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(v)); break;
        case 8:  std::snprintf(text, sizeof(text), "%llo", static_cast<unsigned long long>(v)); break;
        default: std::snprintf(text, sizeof(text), "%lld", v); break;
        }
        buffer_ += text;
    }

    void appendInteger(unsigned long long v)
    {
        char text[32];
        switch (base_)
        {
        case 16: std::snprintf(text, sizeof(text), "%llx", v); break;
        case 8:  std::snprintf(text, sizeof(text), "%llo", v); break;
        default: std::snprintf(text, sizeof(text), "%llu", v); break;
        }
        buffer_ += text;
    }

    std::uint32_t level_;
    const char* category_;
    std::string buffer_;
    int base_ = 10;      // std::hex/dec/oct 粘性基数
    bool flushed_ = false;
};

// std::fixed + std::setprecision(n) 的等价格式化（%.*f）。
// 迁移人工批次用：`<< std::fixed << std::setprecision(2) << v` → `<< primal::log::Fixed(2, v)`。
inline std::string Fixed(int precision, double v)
{
    char text[128];
    std::snprintf(text, sizeof(text), "%.*f", precision, v);
    return std::string(text);
}

} // namespace primal::log

// category 必须显式给出（脚本按目录注入；与命令命名空间对齐，规划 §11.2）。
#define PRIMAL_LOG_STREAM(level, category) \
    ::primal::log::LogStream(::primal::diag::kLevel##level, category)
