#pragma once

#include <cstdint>
#include <string>

#include "libs/error.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/modbus_rtu.hpp"

namespace libs::rangefinder {

// TOF101 Modbus RTU 保持寄存器：纯编解码，无 I/O。
//
// 寄存器语义取自《TOF101_Modbus通信协议说明》。距离的工程量换算、哨兵值含义
// 与符号约定在本层不做任何判断——这里只把 raw 交出来，由使用方决定怎么解释。

using Bytes = libs::Bytes;

// 常用寄存器（厂家文档地址，不减一）。
namespace reg {
constexpr std::uint16_t kDistanceZeroOnOutOfRange = 0x0200;
constexpr std::uint16_t kDistanceMaxOnOutOfRange  = 0x0201;
constexpr std::uint16_t kDistanceRaw              = 0x0202;
constexpr std::uint16_t kReflectionPeak           = 0x0203;
constexpr std::uint16_t kConfidence               = 0x0204;
constexpr std::uint16_t kVelocity                 = 0x0205;
constexpr std::uint16_t kTemperature              = 0x0206;
constexpr std::uint16_t kSampleCounter            = 0x0207;
constexpr std::uint16_t kSlaveAddress             = 0x0208;
}  // namespace reg

// 功能码 0x03 读保持寄存器。
// address 必须是 1..247；count 1..125 且不越界，否则抛 std::invalid_argument。
Bytes make_read_request(std::uint8_t address = 1, std::uint16_t start = reg::kDistanceZeroOnOutOfRange,
                        std::uint16_t count = 17);

struct Register {
    std::uint16_t address = 0;
    std::uint16_t raw = 0;
    std::string name;      // 厂家文档里的寄存器名，未知地址为 "unknown_raw"
    std::string unit;      // 文档标注的单位，未知时为空
    // 仅当厂家文档明确给出该寄存器的无效哨兵值时才可能为 false。
    // 默认 true：没有文档依据时，本层不做任何有效性判断（那是上层的事）。
    bool valid = true;
};

struct Reading {
    std::uint8_t address = 0;
    std::uint16_t start = 0;
    std::uint16_t count = 0;
    std::vector<Register> registers;
};

// 解码功能码 0x03 的响应。帧定位、CRC、回显剔除交给 modbus::locate_reply。
// 异常响应返回错误（ErrorKind::device），并保留异常码。
Result<Reading> decode_read_response(const Bytes& capture, std::uint8_t address,
                                     std::uint16_t start, std::uint16_t count);

// 寄存器名/单位说明，纯查表。
Register describe_register(std::uint16_t address, std::uint16_t raw);

}  // namespace libs::rangefinder

