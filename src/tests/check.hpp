#pragma once

// 极简测试骨架：不引入 GoogleTest，测试就是普通可执行文件。
// 每个 CHECK 失败会打印用例名、文件、行号和实际值，进程退出码非 0 表示失败。

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include "libs/error.hpp"

namespace testing {

inline int g_failures = 0;
inline int g_checks = 0;
inline const char* g_case = "";

inline void begin_case(const char* name) {
    g_case = name;
    std::printf("  case: %s\n", name);
}

inline void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::printf("FAIL [%s] %s:%d\n      %s\n", g_case, file, line, what.c_str());
}

inline void check(bool condition, const char* file, int line, const char* expr) {
    ++g_checks;
    if (!condition) fail(file, line, std::string("expected true: ") + expr);
}

// 枚举没有 operator<<。用这个 trait 判断能不能直接流式输出，
// 不能就退回"类型相等"的信息，而不是让模板实例化失败。
template <class T, class = void>
struct Streamable : std::false_type {};

template <class T>
struct Streamable<T, std::void_t<decltype(std::declval<std::stringstream&>()
                                         << std::declval<const T&>())>> : std::true_type {};

// 枚举（ErrorKind、ApplyState 等）在库里没有 operator<<。
// 单独为 ErrorKind 补一个可打印路径：失败时能直接看出是哪种错，
// 否则只能看到两个 mangled 类型名，排查全靠猜。
inline std::string describe(libs::ErrorKind kind) { return libs::to_string(kind); }

template <class T>
void print_value(std::stringstream& ss, const T& value) {
    if constexpr (std::is_same_v<T, libs::ErrorKind>) {
        ss << libs::to_string(value);
    } else if constexpr (Streamable<T>::value) {
        ss << value;
    } else {
        ss << "<" << typeid(T).name() << " 值不可打印>";
    }
}

template <class A, class B>
void check_eq(const A& actual, const B& expected, const char* file, int line, const char* expr) {
    ++g_checks;
    if (!(actual == expected)) {
        std::stringstream ss;
        ss << expr << "\n      expected: ";
        print_value(ss, expected);
        ss << "\n      actual:   ";
        print_value(ss, actual);
        fail(file, line, ss.str());
    }
}

// Result<T> 的 operator bool 是 explicit 的（避免调用方不小心在取值处隐式转换），
// 而测试里的 CHECK(some_result) 需要它。测试设施在这里收掉这个显式转换。
template <class T>
inline bool testable(const T& value) {
    return static_cast<bool>(value);
}

struct Case {
    explicit Case(const char* name) { begin_case(name); }
};

int finish(const char* suite);

}  // namespace testing

#define CASE(name) ::testing::Case case_##name(#name)
#define CHECK(cond) ::testing::check(::testing::testable(cond), __FILE__, __LINE__, #cond)
#define CHECK_EQ(actual, expected) \
    ::testing::check_eq((actual), (expected), __FILE__, __LINE__, #actual " == " #expected)
#define CHECK_THROWS(expr)                                                        \
    do {                                                                          \
        bool thrown_ = false;                                                     \
        try { (void)(expr); } catch (const std::exception&) { thrown_ = true; }     \
        ::testing::check(thrown_, __FILE__, __LINE__, "expected throw: " #expr);  \
    } while (0)

// 不需要命令行参数的测试用 TEST_MAIN_BEGIN()；
// 需要接收参数（例如替身 DLL 的路径）用 TEST_MAIN_BEGIN_ARGV()，argc/argv 在作用域内可见。
#define TEST_MAIN_BEGIN() \
    int main() { std::printf("== %s ==\n", __FILE__);

#define TEST_MAIN_BEGIN_ARGV() \
    int main(int argc, char** argv) { std::printf("== %s ==\n", __FILE__);

#define TEST_MAIN_RETURN() \
    std::printf("\n"); return ::testing::finish(__FILE__); }

