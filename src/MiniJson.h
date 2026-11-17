// ============================================================
// MiniJson —— 够用即可的 JSON 解析器（仅供本项目读配置/清单）
//
// 输入 UTF-8 字节流，字符串统一解码为 UTF-16（std::wstring），
// 供 Win32 / MediaEngine API 直接使用。支持：
//   对象 / 数组 / 字符串（含 \uXXXX 代理对）/ 数字 / true / false / null
// 不支持：注释、尾逗号、超大整数精确表示（数量级无碍）。
// ============================================================

#pragma once

#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <cstdlib>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::wstring str;
    std::vector<Value> arr;
    std::vector<std::pair<std::wstring, Value>> obj;  // 保序存储

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsString() const { return type == Type::String; }

    // 对象字段查找；不存在返回 nullptr
    const Value* Find(const wchar_t* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    std::wstring GetStr(const wchar_t* key, const wchar_t* def = L"") const {
        const Value* v = Find(key);
        return (v && v->IsString()) ? v->str : def;
    }

    double GetNum(const wchar_t* key, double def = 0.0) const {
        const Value* v = Find(key);
        return (v && v->type == Type::Number) ? v->num : def;
    }

    bool GetBool(const wchar_t* key, bool def = false) const {
        const Value* v = Find(key);
        return (v && v->type == Type::Bool) ? v->b : def;
    }
};

// ------------------------------------------------------------
// 内部解析器：单向扫描，出错返回 false（out 内容不保证）
// ------------------------------------------------------------
class Parser {
public:
    explicit Parser(const std::string& utf8) : s_(utf8) {}

    bool Parse(Value& out) {
        // 跳过 UTF-8 BOM（Windows 下手写/编辑器保存的 JSON 常见）
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;
        }
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return pos_ == s_.size();
    }

private:
    static constexpr int kMaxDepth = 32;  // apples.json 深度 ≤ 4，留足余量

    const std::string& s_;
    size_t pos_ = 0;

    void SkipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool Match(char c) {
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return false;
        SkipWs();
        if (pos_ >= s_.size()) return false;

        char c = s_[pos_];
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            out.type = Value::Type::String;
            return ParseString(out.str);
        }
        if (c == 't') return ParseKeyword([&] {
            out.type = Value::Type::Bool;
            out.b = true;
        });
        if (c == 'f') return ParseKeyword([&] {
            out.type = Value::Type::Bool;
            out.b = false;
        });
        if (c == 'n') return ParseKeyword([&] { out.type = Value::Type::Null; });
        return ParseNumber(out);
    }

    template <typename F>
    bool ParseKeyword(F apply) {
        // 关键字均为 ASCII，直接按字节匹配
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            apply();
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            apply();
            return true;
        }
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            apply();
            return true;
        }
        return false;
    }

    bool ParseObject(Value& out, int depth) {
        ++pos_;  // '{'
        out.type = Value::Type::Object;
        SkipWs();
        if (Match('}')) return true;
        for (;;) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return false;
            std::wstring key;
            if (!ParseString(key)) return false;
            if (!Match(':')) return false;
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            if (Match(',')) continue;
            return Match('}');
        }
    }

    bool ParseArray(Value& out, int depth) {
        ++pos_;  // '['
        out.type = Value::Type::Array;
        SkipWs();
        if (Match(']')) return true;
        for (;;) {
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            if (Match(',')) continue;
            return Match(']');
        }
    }

    bool ParseString(std::wstring& out) {
        ++pos_;  // '"'
        out.clear();
        std::string utf8;  // 先收集 UTF-8 字节，统一转宽
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return Utf8ToWide(utf8, out);
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) return false;
                char e = s_[pos_++];
                switch (e) {
                case '"':  utf8.push_back('"'); break;
                case '\\': utf8.push_back('\\'); break;
                case '/':  utf8.push_back('/'); break;
                case 'b':  utf8.push_back('\b'); break;
                case 'f':  utf8.push_back('\f'); break;
                case 'n':  utf8.push_back('\n'); break;
                case 'r':  utf8.push_back('\r'); break;
                case 't':  utf8.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ParseHex4(cp)) return false;
                    // 代理对：\uD800-\uDBFF 后跟 \uDC00-\uDFFF
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                        s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        unsigned lo = 0;
                        size_t save = pos_;
                        pos_ += 2;
                        if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            pos_ = save;
                        }
                    }
                    AppendCodePoint(utf8, cp);
                    break;
                }
                default:
                    return false;
                }
            } else if (c < 0x20) {
                return false;  // 控制字符不允许裸出现
            } else {
                utf8.push_back(static_cast<char>(c));
                ++pos_;
            }
        }
        return false;  // 字符串未闭合
    }

    bool ParseHex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_ + i];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        pos_ += 4;
        return true;
    }

    static void AppendCodePoint(std::string& utf8, unsigned cp) {
        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseNumber(Value& out) {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '-' || c == '+') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) return false;
        out.type = Value::Type::Number;
        out.num = strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    static bool Utf8ToWide(const std::string& utf8, std::wstring& wide) {
        if (utf8.empty()) {
            wide.clear();
            return true;
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                    static_cast<int>(utf8.size()), nullptr, 0);
        if (n <= 0) return false;
        wide.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                            wide.data(), n);
        return true;
    }
};

// 解析 UTF-8 JSON 文本；失败返回 false
inline bool Parse(const std::string& utf8, Value& out) {
    Parser p(utf8);
    return p.Parse(out);
}

}  // namespace json
