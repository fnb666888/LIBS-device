#pragma once

// 统一错误与返回值。
//
// 三层（protocols / transport / devices）共用同一套错误分类和 Result<T>，
// 这样调用方只需要看一个 ok，不需要区分"这个错误来自编解码还是来自串口"。
//
// 设计约定：
//   * ok() == false 时 error() 一定有 kind / code / message，可直接写日志；
//   * raw 保留出错的原始字节（协议帧、USB 回包），便于事后复盘；
//   * 只有编程错误（参数越界、索引非法）才抛异常，设备/协议失败一律走 Result。

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace libs {

enum class ErrorKind {
    none,
    invalid_argument,  // 调用方给的参数或配置不合法
    unsupported,       // 该命令/功能在本层不被支持
    protocol,          // 帧不完整、CRC 错、回包不匹配、异常响应
    transport,         // 串口/USB/SDK 打开、读写或驱动层失败
    device,            // 设备返回了合法但不满足请求的状态
    timeout,
    internal,
};

const char* to_string(ErrorKind kind) noexcept;

struct Error {
    ErrorKind kind = ErrorKind::none;
    std::string code;
    std::string message;
    std::vector<std::uint8_t> raw;

    bool ok() const noexcept { return kind == ErrorKind::none; }

    static Error make(ErrorKind kind, std::string code, std::string message) {
        return Error{kind, std::move(code), std::move(message), {}};
    }

    static Error with_raw(ErrorKind kind, std::string code, std::string message,
                          std::vector<std::uint8_t> bytes) {
        return Error{kind, std::move(code), std::move(message), std::move(bytes)};
    }

    explicit operator bool() const noexcept { return !ok(); }
};

// Result<T> 成功时带一个值，失败时带一个 Error。不使用异常做控制流。
template <class T>
class Result {
public:
    // 默认构造是**成功**，不是失败。
    //
    // 这不是为了好看——而是因为全库都在用 `return VoidResult();`
    // 表达「无返回值且成功」。若默认构造是失败，那些位置
    // 会静默地把「成功」变成「error.kind == none 的失败」，
    // 而调用方看 ok() 又会走到取值一侧。
    //
    // 对没有默认构造的 T，这个构造会正常不可用（需要带值构造）——
    // 这正好是我们想要的信号：要么给值，要么给 error，不要靠默认构造。
    Result() : value_(T{}) {}

    static Result success(T value) {
        Result r;
        r.value_ = std::move(value);
        return r;
    }
    static Result failure(Error error) {
        Result r;
        r.error_ = std::move(error);
        return r;
    }
    // 允许 `return Error::make(...)` 和 `return value;` 两种直接返回写法。
    Result(T value) : value_(std::move(value)) {}       // NOLINT(google-explicit-constructor)
    Result(Error error) : error_(std::move(error)) {}    // NOLINT(google-explicit-constructor)

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    // value() 在失败时抛出，调用前应先检查 ok()。
    const T& value() const& { return *value_; }
    T& value() & { return *value_; }
    T&& value() && { return std::move(*value_); }
    const T* operator->() const { return &*value_; }
    T* operator->() { return &*value_; }

    const Error& error() const noexcept { return error_; }

private:
    std::optional<T> value_{};
    Error error_{};
};

// 无返回值的操作（发送固定序列、清理等）。
using VoidResult = Result<std::monostate>;

}  // namespace libs


