#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"

namespace libs::modbus {

// 通用 Modbus RTU 编解码：纯函数，无 I/O、无 sleep、不碰串口配置。
// 设备专属的寄存器映射放在 protocols/rangefinder.hpp 与 protocols/indicator.hpp。

using Bytes = libs::Bytes;

std::uint16_t crc16(const std::uint8_t* bytes, std::size_t size);

// 地址 1..247。广播（0）被拒绝：广播没有可验证的回包。
bool is_unicast_address(std::uint8_t address) noexcept;

// 组帧：功能码 3/6 时 value 分别是寄存器号/寄存器数量，功能码 5/16 时是线圈值。
Bytes make_request(std::uint8_t address, std::uint8_t function,
                   std::uint16_t reg, std::uint16_t value);

struct Frame {
    Bytes bytes;
    std::size_t offset = 0;   // 在原始捕获流中的位置
    bool exception = false;   // true 表示这是异常响应（功能码 | 0x80）
    std::uint8_t exception_code = 0;
};

// 在捕获的字节流里定位唯一一条匹配响应。
//
// 处理三种真实世界的串口噪声：
//   * 请求回显：USB/485 转换器经常把请求原样送回来，不能当响应；
//   * 前后垃圾字节：跳过；
//   * CRC 错误 / 地址功能码不符 / 异常帧缺异常码：一律不认。
// 一条事务里出现多条匹配响应时返回错误，因为无法判断哪条才是本次的响应。
//
// valid_normal_header 让调用方表达"正常响应的头部长什么样"（字节数、寄存器号、状态字）。
//
// reject_request_echo 必须由调用方按协议决定，不能一律当成 true：
// 读类响应（FC01/FC03）和请求长度不同，靠 valid_normal_header 就能区分；
// 但写类响应（FC05/FC06）按 Modbus 规定是把请求原样回送，两者字节完全相同，
// 此时若仍做回显剔除，真实响应会被当成回显丢掉。空调协议就属于后者，
// 所以它的写响应必须传 false。
Result<Frame> locate_reply(const Bytes& capture,
                           std::uint8_t address, std::uint8_t function,
                           const Bytes& exact_request, std::size_t normal_length,
                           const std::function<bool(const std::uint8_t*)>& valid_normal_header,
                           bool reject_request_echo = true);

}  // namespace libs::modbus

