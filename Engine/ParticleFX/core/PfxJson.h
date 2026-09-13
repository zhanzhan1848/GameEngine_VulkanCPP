#pragma once

// PfxJson.h — PFX 资产用精简 JSON 解析器（递归下降 + 行号报错）。
//
// 决议修订（§10 账本 10.2）：架构文档 §7-N1 原定 vendor nlohmann/single-header，
// 但引擎全局 -fno-exceptions 编译使 nlohmann 的异常错误模型不可用
// （JSON_NOEXCEPTION 模式丢弃行号报错能力——恰是 vendor 的理由），
// 故自研本解析器：无异常、行号/列号报错、值携带来源行号供编译期校验定位。
// 语法覆盖标准 JSON 子集：object/array/string(转义)/number/true/false/null。

#include "Common/CommonHeaders.h"

#include <memory>
#include <string>
#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::json {

class value {
public:
    enum class type : u8 { null_v, boolean, number, string_v, array_v, object_v };

    type get_type() const { return type_; }
    bool is_object() const { return type_ == type::object_v; }
    bool is_array() const { return type_ == type::array_v; }
    bool is_string() const { return type_ == type::string_v; }
    bool is_number() const { return type_ == type::number; }
    bool is_bool() const { return type_ == type::boolean; }
    bool is_null() const { return type_ == type::null_v; }

    // 来源行号（1 起；编译期校验错误定位用）。
    u32 line() const { return line_; }

    bool as_bool() const { return bool_; }
    double as_number() const { return number_; }
    f32 as_f32() const { return static_cast<f32>(number_); }
    u32 as_u32() const;                       // 负数/超范围截断为 0
    const std::string& as_string() const { return string_; }

    // ---- array ----
    size_t array_size() const { return array_.size(); }
    const value* array_at(size_t i) const { return i < array_.size() ? &array_[i] : nullptr; }

    // ---- object（保持插入序；重复键后者覆盖）----
    // 未命中返回 nullptr。
    const value* find(const char* key) const;
    size_t object_size() const { return object_.size(); }
    const std::string& key_at(size_t i) const { return object_[i].first; }
    const value* value_at(size_t i) const { return &object_[i].second; }

private:
    type type_{ type::null_v };
    u32 line_{ 0 };
    bool bool_{ false };
    double number_{ 0.0 };
    std::string string_;
    std::vector<value> array_;
    std::vector<std::pair<std::string, value>> object_;

    friend struct parser;   // 定义于 PfxJson.cpp（json 命名空间内，非匿名）
};

struct parse_result {
    bool ok{ false };
    std::unique_ptr<value> root;
    std::string error;      // 为空 = 成功
    u32 line{ 0 };
    u32 column{ 0 };
};

// 解析 UTF-8 JSON 文本。根必须是 object/array/任意值均可。
parse_result parse(const char* text, size_t length);

} // namespace primal::pfx::json

#endif // !DISABLE_PARTICLE_FX
