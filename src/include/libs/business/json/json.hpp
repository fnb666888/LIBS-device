#pragma once

// 业务层的最小 JSON 读写。
//
// 为什么自己写而不用第三方：守护进程与 CLI 之间要传结构化值，本层只依赖
// 标准库，跨进程接口因此不引入任何 ABI 风险（见报告 §5.2 裁决：services/
// 从第一天起就是跨进程可调用的）。
//
// 能力边界（重要）：
//   * 只处理业务层真正需要的形状：对象、数组、字符串、数字、bool、null；
//   * 数值一律按 double 存取——业务层不传递 int64 精度以上的要求；
//   * 解析严格：多余字符、坏转义、尾随逗号一律报错，不做"尽力解析"。
//
// 这不是通用 JSON 库。不要往里加注释、格式化、模式校验。

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace business::json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

enum class Type { null, boolean, number, string, array, object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}                       // NOLINT
    Value(bool v) : type_(Type::boolean), bool_(v) {}          // NOLINT
    Value(double v) : type_(Type::number), number_(v) {}        // NOLINT
    Value(int v) : type_(Type::number), number_(static_cast<double>(v)) {}   // NOLINT
    Value(std::string v) : type_(Type::string), string_(std::move(v)) {}       // NOLINT
    Value(const char* v) : type_(Type::string), string_(v ? v : "") {}         // NOLINT
    Value(Array v) : type_(Type::array), array_(std::move(v)) {}               // NOLINT
    Value(Object v) : type_(Type::object), object_(std::move(v)) {}             // NOLINT

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::null; }
    bool is_bool() const noexcept { return type_ == Type::boolean; }
    bool is_number() const noexcept { return type_ == Type::number; }
    bool is_string() const noexcept { return type_ == Type::string; }
    bool is_array() const noexcept { return type_ == Type::array; }
    bool is_object() const noexcept { return type_ == Type::object; }

    bool as_bool(bool fallback = false) const noexcept { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const noexcept { return is_number() ? number_ : fallback; }
    const std::string& as_string() const noexcept { return string_; }
    const Array& as_array() const noexcept { return array_; }
    const Object& as_object() const noexcept { return object_; }
    Array& as_array() noexcept { return array_; }
    Object& as_object() noexcept { return object_; }

    // 对象成员访问。缺失时返回共享的空 nullptr 值，不抛异常。
    const Value& operator[](const std::string& key) const noexcept;
    bool has(const std::string& key) const noexcept { return is_object() && object_.count(key) != 0; }

    void set(const std::string& key, Value value) { object_[key] = std::move(value); }
    void push_back(Value value) { array_.push_back(std::move(value)); }

    // 人类可读的名字，用于错误信息。
    const char* type_name() const noexcept;

private:
    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

struct ParseError {
    bool ok = true;
    std::string message;
    std::size_t offset = 0;
};

// 解析一个完整 JSON 文本。尾随的非空白字符视为错误。
ParseError parse(const std::string& text, Value& out);

// 序列化。indent > 0 时缩进美化；0 时输出紧凑单行。
std::string stringify(const Value& value, int indent = 0);

// 便捷构造。
Value object();
Value array();

}  // namespace business::json