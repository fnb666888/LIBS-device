#include "libs/business/json/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace business::json {
namespace {

const Value& null_value() {
    static const Value v;
    return v;
}

void escape_into(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // UTF-8 多字节序列原样透传。业务层的字符串是中文设备名与
                    // 路径，原样透传比转成 \uXXXX 更可读，也不改变字节。
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void number_into(double v, std::string& out) {
    if (std::isnan(v) || std::isinf(v)) {
        // JSON 没有 NaN/Inf。业务层的"未知"必须走 null，不能序列化成
        // 一个解析不回来的字面量（纪律 2：未知 != 正常）。
        out += "null";
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
}

void write(const Value& v, std::string& out, int indent, int depth) {
    const bool pretty = indent > 0;
    const std::string pad = pretty ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string pad_close = pretty ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();

    switch (v.type()) {
        case Type::null:    out += "null"; return;
        case Type::boolean: out += v.as_bool() ? "true" : "false"; return;
        case Type::number:  number_into(v.as_number(), out); return;
        case Type::string:  escape_into(v.as_string(), out); return;
        case Type::array: {
            const Array& a = v.as_array();
            if (a.empty()) { out += "[]"; return; }
            out.push_back('[');
            bool first = true;
            for (const Value& item : a) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) { out.push_back('\n'); out += pad; }
                write(item, out, indent, depth + 1);
            }
            if (pretty) { out.push_back('\n'); out += pad_close; }
            out.push_back(']');
            return;
        }
        case Type::object: {
            const Object& o = v.as_object();
            if (o.empty()) { out += "{}"; return; }
            out.push_back('{');
            bool first = true;
            for (const auto& kv : o) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) { out.push_back('\n'); out += pad; }
                escape_into(kv.first, out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                write(kv.second, out, indent, depth + 1);
            }
            if (pretty) { out.push_back('\n'); out += pad_close; }
            out.push_back('}');
            return;
        }
    }
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool run(Value& out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        if (pos_ != s_.size()) return fail("trailing characters after the top-level value");
        return true;
    }

    const ParseError& error() const { return err_; }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(const std::string& message) {
        if (err_.ok) { err_.ok = false; err_.message = message; err_.offset = pos_; }
        return false;
    }

    void skip_ws() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool literal(const char* text) {
        const std::size_t n = std::char_traits<char>::length(text);
        if (s_.compare(pos_, n, text) != 0) return false;
        pos_ += n;
        return true;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting is too deep");
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': {
                std::string str;
                if (!parse_string(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't': if (literal("true"))  { out = Value(true);  return true; } return fail("invalid literal");
            case 'f': if (literal("false")) { out = Value(false); return true; } return fail("invalid literal");
            case 'n': if (literal("null"))  { out = Value();      return true; } return fail("invalid literal");
            default:  return parse_number(out);
        }
    }

    bool parse_object(Value& out, int depth) {
        ++pos_;  // '{'
        Object obj;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; out = Value(std::move(obj)); return true; }
        for (;;) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a quoted member name");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':' after the member name");
            ++pos_;
            skip_ws();
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            obj[key] = std::move(v);
            skip_ws();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; break; }
            return fail("expected ',' or '}' in the object");
        }
        out = Value(std::move(obj));
        return true;
    }

    bool parse_array(Value& out, int depth) {
        ++pos_;  // '['
        Array arr;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; out = Value(std::move(arr)); return true; }
        for (;;) {
            skip_ws();
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skip_ws();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; break; }
            return fail("expected ',' or ']' in the array");
        }
        out = Value(std::move(arr));
        return true;
    }

    static void append_utf8(unsigned cp, std::string& out) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("invalid hex digit in \\u escape");
        }
        return true;
    }

    bool parse_string(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= s_.size()) return fail("unterminated escape");
            switch (s_[pos_++]) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {          // high surrogate
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned lo = 0;
                            if (!parse_hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                return fail("invalid low surrogate");
                            }
                        } else {
                            return fail("lone high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("lone low surrogate");
                    }
                    append_utf8(cp, out);
                    break;
                }
                default: return fail("unknown escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Value& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
        }
        if (!any) return fail("expected a value");
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
            bool exp_digits = false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; exp_digits = true; }
            if (!exp_digits) return fail("exponent has no digits");
        }
        out = Value(std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr));
        return true;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    ParseError err_{};
};

}  // namespace

const Value& Value::operator[](const std::string& key) const noexcept {
    if (!is_object()) return null_value();
    const auto it = object_.find(key);
    return it == object_.end() ? null_value() : it->second;
}

const char* Value::type_name() const noexcept {
    switch (type_) {
        case Type::null:    return "null";
        case Type::boolean: return "boolean";
        case Type::number:  return "number";
        case Type::string:  return "string";
        case Type::array:   return "array";
        case Type::object:  return "object";
    }
    return "unknown";
}

ParseError parse(const std::string& text, Value& out) {
    Parser p(text);
    if (!p.run(out)) {
        Value discard;
        out = discard;
        return p.error();
    }
    return ParseError{};
}

std::string stringify(const Value& value, int indent) {
    std::string out;
    write(value, out, indent < 0 ? 0 : indent, 0);
    return out;
}

Value object() { return Value(Object{}); }
Value array()  { return Value(Array{}); }

}  // namespace business::json