#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace libs {

using Bytes = std::vector<std::uint8_t>;

// 大写、空格分隔的十六进制串，用于日志与断言。
std::string to_hex(const Bytes& bytes);

// 解析 "5A A5 1A ..." 或 "5aa51a" 形式；非法字符抛 std::invalid_argument。
Bytes from_hex(std::string_view text);

}  // namespace libs
