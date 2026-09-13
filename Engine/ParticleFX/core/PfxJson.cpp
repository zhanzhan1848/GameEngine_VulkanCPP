// PfxJson.cpp — 见 PfxJson.h。

#include "PfxJson.h"

#include <cstdlib>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::json {

// friend 声明要求此类型与 value 内声明一致：json 命名空间内（不可匿名）。
struct parser {
    const char* text;
    size_t length;
    size_t pos{ 0 };
    u32 line{ 1 };
    u32 column{ 1 };
    std::string error;
    u32 error_line{ 0 };
    u32 error_column{ 0 };

    bool fail(const char* message) {
        if (error.empty()) {
            error = message;
            error_line = line;
            error_column = column;
        }
        return false;
    }

    bool at_end() const { return pos >= length; }
    char peek() const { return text[pos]; }

    void advance() {
        if (text[pos] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
        ++pos;
    }

    void skip_ws() {
        while (!at_end()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                advance();
            } else if (c == '/' && pos + 1 < length && text[pos + 1] == '/') {
                // 行注释容忍（资产手写友好；标准 JSON 不含，写出端不产生）
                while (!at_end() && peek() != '\n') advance();
            } else {
                break;
            }
        }
    }

    bool parse_value(value& out, u32 depth) {
        if (depth > 64) return fail("nesting too deep (max 64)");
        skip_ws();
        if (at_end()) return fail("unexpected end of input");

        out.line_ = line;
        const char c = peek();
        if (c == '{') return parse_object(out, depth);
        if (c == '[') return parse_array(out, depth);
        if (c == '"') return parse_string_value(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return fail("unexpected character");
    }

    bool parse_object(value& out, u32 depth) {
        out.type_ = value::type::object_v;
        advance();   // '{'
        skip_ws();
        if (!at_end() && peek() == '}') {
            advance();
            return true;
        }
        while (true) {
            skip_ws();
            if (at_end()) return fail("unterminated object");
            if (peek() != '"') return fail("expected string key");
            value key;
            if (!parse_string_value(key)) return false;
            skip_ws();
            if (at_end() || peek() != ':') return fail("expected ':'");
            advance();
            value item;
            if (!parse_value(item, depth + 1)) return false;
            // 重复键：后者覆盖（保持首次插入位置）
            bool replaced = false;
            for (auto& kv : out.object_) {
                if (kv.first == key.string_) {
                    kv.second = std::move(item);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) out.object_.emplace_back(std::move(key.string_), std::move(item));
            skip_ws();
            if (at_end()) return fail("unterminated object");
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == '}') {
                advance();
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(value& out, u32 depth) {
        out.type_ = value::type::array_v;
        advance();   // '['
        skip_ws();
        if (!at_end() && peek() == ']') {
            advance();
            return true;
        }
        while (true) {
            value item;
            if (!parse_value(item, depth + 1)) return false;
            out.array_.push_back(std::move(item));
            skip_ws();
            if (at_end()) return fail("unterminated array");
            if (peek() == ',') {
                advance();
                continue;
            }
            if (peek() == ']') {
                advance();
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parse_string_value(value& out) {
        out.type_ = value::type::string_v;
        return parse_string_raw(out.string_);
    }

    bool parse_string_raw(std::string& out) {
        advance();   // '"'
        while (true) {
            if (at_end()) return fail("unterminated string");
            const char c = peek();
            if (c == '"') {
                advance();
                return true;
            }
            if (c == '\\') {
                advance();
                if (at_end()) return fail("unterminated escape");
                const char e = peek();
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        // \uXXXX —— 仅支持 BMP 直接编码（资产键名/名字段为 ASCII，足够）
                        if (pos + 4 >= length) return fail("bad \\u escape");
                        u32 code = 0;
                        for (int i = 1; i <= 4; ++i) {
                            const char h = text[pos + i];
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (u32)(h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (u32)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (u32)(h - 'A' + 10);
                            else return fail("bad \\u escape");
                        }
                        // UTF-8 编码
                        if (code < 0x80) {
                            out.push_back((char)code);
                        } else if (code < 0x800) {
                            out.push_back((char)(0xC0 | (code >> 6)));
                            out.push_back((char)(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (code >> 12)));
                            out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (code & 0x3F)));
                        }
                        for (int i = 0; i < 4; ++i) advance();
                        advance();
                        continue;
                    }
                    default:
                        return fail("bad escape character");
                }
                advance();
                continue;
            }
            if ((unsigned char)c < 0x20) return fail("control character in string");
            out.push_back(c);
            advance();
        }
    }

    bool parse_bool(value& out) {
        if (pos + 4 <= length && std::memcmp(text + pos, "true", 4) == 0) {
            out.type_ = value::type::boolean;
            out.bool_ = true;
            for (int i = 0; i < 4; ++i) advance();
            return true;
        }
        if (pos + 5 <= length && std::memcmp(text + pos, "false", 5) == 0) {
            out.type_ = value::type::boolean;
            out.bool_ = false;
            for (int i = 0; i < 5; ++i) advance();
            return true;
        }
        return fail("invalid literal");
    }

    bool parse_null(value& out) {
        if (pos + 4 <= length && std::memcmp(text + pos, "null", 4) == 0) {
            out.type_ = value::type::null_v;
            for (int i = 0; i < 4; ++i) advance();
            return true;
        }
        return fail("invalid literal");
    }

    bool parse_number(value& out) {
        out.type_ = value::type::number;
        const size_t start = pos;
        if (peek() == '-') advance();
        while (!at_end() && peek() >= '0' && peek() <= '9') advance();
        if (!at_end() && peek() == '.') {
            advance();
            while (!at_end() && peek() >= '0' && peek() <= '9') advance();
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (!at_end() && (peek() == '+' || peek() == '-')) advance();
            while (!at_end() && peek() >= '0' && peek() <= '9') advance();
        }
        out.number_ = std::strtod(text + start, nullptr);
        return true;
    }
};


u32 value::as_u32() const {
    if (type_ != type::number || number_ < 0.0 || number_ > 4294967295.0) return 0;
    return (u32)number_;
}

const value* value::find(const char* key) const {
    for (const auto& kv : object_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

parse_result parse(const char* text, size_t length) {
    parse_result result;
    if (!text || length == 0) {
        result.error = "empty input";
        return result;
    }
    parser p{text, length};
    value root;
    if (!p.parse_value(root, 0)) {
        result.error = p.error;
        result.line = p.error_line;
        result.column = p.error_column;
        return result;
    }
    p.skip_ws();
    if (!p.at_end()) {
        result.error = "trailing content after root value";
        result.line = p.line;
        result.column = p.column;
        return result;
    }
    result.root = std::make_unique<value>();
    *result.root = std::move(root);
    result.ok = true;
    return result;
}

} // namespace primal::pfx::json

#endif // !DISABLE_PARTICLE_FX
