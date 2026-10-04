#pragma once

// SU-300 矢量变频器 Modbus-RTU 协议。
//
// 说明书第 4 章规定：RTU、功能码 03/06、默认地址 01、寄存器按大端发送，
// CRC16(Modbus) 低字节在前。协议层只做编解码，不打开串口，也不决定电机
// 是否可以运动；运动门禁由上层在读取故障/运行状态后完成。

#include <cstdint>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/modbus_rtu.hpp"

namespace libs::su300 {

using Bytes = libs::Bytes;

namespace reg {
constexpr std::uint16_t kControlCommand = 0x2000;
constexpr std::uint16_t kFrequencyCommand = 0x2001;
constexpr std::uint16_t kExternalCommand = 0x2002;
constexpr std::uint16_t kFault = 0x2100;
constexpr std::uint16_t kOperation = 0x2101;
constexpr std::uint16_t kSetFrequency = 0x2102;
constexpr std::uint16_t kOutputFrequency = 0x2103;
constexpr std::uint16_t kOutputCurrent = 0x2104;
constexpr std::uint16_t kBusVoltage = 0x2105;
constexpr std::uint16_t kOutputVoltage = 0x2106;
constexpr std::uint16_t kAnalogInput = 0x2107;
constexpr std::uint16_t kCurrentCount = 0x2109;
constexpr std::uint16_t kMotorSpeed = 0x210A;
constexpr std::uint16_t kTemperature = 0x210D;
constexpr std::uint16_t kPidFeedback = 0x210E;
constexpr std::uint16_t kPidSetting = 0x210F;
constexpr std::uint16_t kPulseInput = 0x2111;
constexpr std::uint16_t kCurrentFault = 0x2112;
constexpr std::uint16_t kTimingValue = 0x2113;
constexpr std::uint16_t kInputTerminal = 0x2114;
constexpr std::uint16_t kOutputTerminal = 0x2115;
constexpr std::uint16_t kStatus = 0x2116;
constexpr std::uint16_t kDirection = 0x2117;
}  // namespace reg

namespace command {
constexpr std::uint16_t kHalt = 0x0001;
constexpr std::uint16_t kForwardRun = 0x0012;
constexpr std::uint16_t kForwardJog = 0x0013;
constexpr std::uint16_t kReverseRun = 0x0022;
constexpr std::uint16_t kReverseJog = 0x0023;
constexpr std::uint16_t kExternalFault = 0x0001;
constexpr std::uint16_t kFaultReset = 0x0002;
}  // namespace command

// 说明书通信控制命令 2000H 的五种值。
enum class RunCommand { halt, forward, forward_jog, reverse, reverse_jog };

struct RegisterBlock {
    std::uint8_t address = 0;
    std::uint16_t start = 0;
    std::uint16_t count = 0;
    std::vector<std::uint16_t> values;
};

struct Status {
    std::uint8_t address = 0;
    std::uint16_t fault = 0;             // 2100H，0 = no abnormalities
    std::uint16_t operation = 0;         // 2101H，运行/停机参数位
    std::uint16_t set_frequency = 0;     // 2102H，Hz ×100（原码，不是百分比）
    std::uint16_t output_frequency = 0;  // 2103H，Hz ×100（原码，不是百分比）
    std::uint16_t output_current = 0;    // 2104H，工程量 ×10
    std::uint16_t bus_voltage = 0;       // 2105H，工程量 ×10
    std::uint16_t output_voltage = 0;    // 2106H，工程量 ×10
    std::uint16_t analog_input = 0;      // 2107H，保留原码
    std::uint16_t current_count = 0;     // 2109H
    std::uint16_t motor_speed = 0;       // 210AH，保留原码，不由方向推断正负
    std::uint16_t temperature = 0;       // 210DH，工程量 ×10
    std::uint16_t pid_feedback = 0;      // 210EH，保留原码
    std::uint16_t pid_setting = 0;       // 210FH，保留原码
    std::uint16_t pulse_input = 0;       // 2111H
    std::uint16_t current_fault = 0;     // 2112H，当前故障码/故障保持原码
    std::uint16_t timing_value = 0;      // 2113H，保留原码
    std::uint16_t input_terminal = 0;    // 2114H
    std::uint16_t output_terminal = 0;   // 2115H
    std::uint16_t status = 0;            // 2116H，位域
    std::uint16_t direction = 0;         // 2117H: 0 forward, 1 reverse, 2 shutdown

    // 读状态时按手册定义的寄存器区间分块；保留所有原始寄存器和原始帧，
    // 便于现场复核，且不把保留地址伪装成有效状态。
    std::vector<RegisterBlock> raw_blocks;
    std::vector<Bytes> raw_responses;

    // 2100H is the general fault code; 2112H is the current fault latch.
    // Treat either non-zero value as unsafe to start or keep running.
    bool faulted() const noexcept { return fault != 0 || current_fault != 0; }
    bool running() const noexcept { return (operation & 0x0001u) != 0; }
    bool stopped() const noexcept { return direction == 2 || (operation & 0x0002u) != 0; }

    double set_frequency_hz() const noexcept {
        return static_cast<double>(set_frequency) / 100.0;
    }
    double output_frequency_hz() const noexcept {
        return static_cast<double>(output_frequency) / 100.0;
    }
};

Bytes make_read_request(std::uint8_t address, std::uint16_t start, std::uint16_t count);
Bytes make_write_request(std::uint8_t address, std::uint16_t reg, std::uint16_t value);

Result<RegisterBlock> decode_read_response(const Bytes& capture, std::uint8_t address,
                                           std::uint16_t start, std::uint16_t count);
VoidResult decode_write_response(const Bytes& capture, std::uint8_t address,
                                 std::uint16_t reg, std::uint16_t value);

Bytes make_run_command_request(std::uint8_t address, RunCommand command);
Bytes make_frequency_request(std::uint8_t address, std::int16_t percent_x100);
Bytes make_fault_reset_request(std::uint8_t address);
std::uint16_t run_command_value(RunCommand command);
Result<Status> decode_status(const RegisterBlock& block);
Result<Status> decode_status(const std::vector<RegisterBlock>& blocks);

}  // namespace libs::su300
