#include "libs/protocols/su300.hpp"

#include <map>
#include <stdexcept>
#include <utility>

namespace libs::su300 {
namespace {

std::uint16_t as_u16(const Bytes& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                      static_cast<std::uint16_t>(bytes[offset + 1]));
}

std::uint16_t command_value(RunCommand command) {
    switch (command) {
        case RunCommand::halt: return command::kHalt;
        case RunCommand::forward: return command::kForwardRun;
        case RunCommand::forward_jog: return command::kForwardJog;
        case RunCommand::reverse: return command::kReverseRun;
        case RunCommand::reverse_jog: return command::kReverseJog;
    }
    throw std::invalid_argument("invalid SU300 run command");
}

}  // namespace

Bytes make_read_request(std::uint8_t address, std::uint16_t start, std::uint16_t count) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("SU300 Modbus address must be 1..247");
    if (count == 0 || count > 125 || static_cast<std::uint32_t>(start) + count > 65536)
        throw std::invalid_argument("SU300 register range must hold 1..125 registers without wrapping");
    return modbus::make_request(address, 0x03, start, count);
}

Bytes make_write_request(std::uint8_t address, std::uint16_t reg, std::uint16_t value) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("SU300 Modbus address must be 1..247");
    return modbus::make_request(address, 0x06, reg, value);
}

Result<RegisterBlock> decode_read_response(const Bytes& capture, std::uint8_t address,
                                           std::uint16_t start, std::uint16_t count) {
    const Bytes request = make_read_request(address, start, count);
    const std::size_t normal_length = 5 + static_cast<std::size_t>(count) * 2;
    auto located = modbus::locate_reply(capture, address, 0x03, request, normal_length,
                                        [count](const std::uint8_t* p) {
                                            return p[2] == static_cast<std::uint8_t>(count * 2);
                                        });
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "su300_modbus_exception",
                               "SU300 returned a Modbus exception (code " +
                                   std::to_string(frame.exception_code) + ")", frame.bytes);

    RegisterBlock result;
    result.address = address;
    result.start = start;
    result.count = count;
    result.values.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
        result.values.push_back(as_u16(frame.bytes, 3 + static_cast<std::size_t>(i) * 2));
    return result;
}

VoidResult decode_write_response(const Bytes& capture, std::uint8_t address,
                                 std::uint16_t reg, std::uint16_t value) {
    const Bytes request = make_write_request(address, reg, value);
    auto located = modbus::locate_reply(
        capture, address, 0x06, request, 8,
        [reg, value](const std::uint8_t* p) {
            return as_u16(Bytes{p[2], p[3]}, 0) == reg && as_u16(Bytes{p[4], p[5]}, 0) == value;
        }, false);
    if (!located) return located.error();
    if (located.value().exception)
        return Error::with_raw(ErrorKind::device, "su300_modbus_exception",
                               "SU300 returned a Modbus exception while writing", located.value().bytes);
    return VoidResult();
}

Bytes make_run_command_request(std::uint8_t address, RunCommand command_value_) {
    return make_write_request(address, reg::kControlCommand, command_value(command_value_));
}

std::uint16_t run_command_value(RunCommand command_value_) {
    return command_value(command_value_);
}

Bytes make_frequency_request(std::uint8_t address, std::int16_t percent_x100) {
    if (percent_x100 < -10000 || percent_x100 > 10000)
        throw std::invalid_argument("SU300 communication frequency must be -10000..10000 (0.01%)");
    return make_write_request(address, reg::kFrequencyCommand,
                              static_cast<std::uint16_t>(percent_x100));
}

Bytes make_fault_reset_request(std::uint8_t address) {
    return make_write_request(address, reg::kExternalCommand, command::kFaultReset);
}

Result<Status> decode_status(const RegisterBlock& block) {
    return decode_status(std::vector<RegisterBlock>{block});
}

Result<Status> decode_status(const std::vector<RegisterBlock>& blocks) {
    // The status map contains reserved holes (2108H, 210BH-210CH, 2110H).
    // Build an address map from the explicitly read blocks instead of indexing
    // one synthetic 0x2100..0x2117 range and accidentally consuming reserves.
    std::map<std::uint16_t, std::uint16_t> values;
    std::uint8_t address = 0;
    for (const RegisterBlock& block : blocks) {
        if (block.count != block.values.size() || block.count == 0)
            return Error::make(ErrorKind::invalid_argument, "status_block_shape",
                               "SU300 status block count does not match its values");
        if (!modbus::is_unicast_address(block.address))
            return Error::make(ErrorKind::invalid_argument, "status_address_invalid",
                               "SU300 status block address must be 1..247");
        if (address == 0) address = block.address;
        else if (block.address != address)
            return Error::make(ErrorKind::invalid_argument, "status_address_mismatch",
                               "SU300 status blocks use different device addresses");
        const std::uint32_t end = static_cast<std::uint32_t>(block.start) + block.count;
        if (end > 65536)
            return Error::make(ErrorKind::invalid_argument, "status_block_range",
                               "SU300 status block wraps the register address space");
        for (std::uint16_t i = 0; i < block.count; ++i) {
            const std::uint16_t reg_addr = static_cast<std::uint16_t>(block.start + i);
            const auto [it, inserted] = values.emplace(reg_addr, block.values[i]);
            if (!inserted && it->second != block.values[i])
                return Error::make(ErrorKind::invalid_argument, "status_duplicate_register",
                                   "SU300 status blocks contain conflicting duplicate registers");
        }
    }
    const auto get = [&values](std::uint16_t address) -> Result<std::uint16_t> {
        const auto it = values.find(address);
        if (it == values.end())
            return Error::make(ErrorKind::invalid_argument, "status_register_missing",
                               "SU300 status response omitted register 0x" +
                                   libs::to_hex(Bytes{static_cast<std::uint8_t>(address >> 8),
                                                      static_cast<std::uint8_t>(address)}));
        return Result<std::uint16_t>::success(it->second);
    };

    const std::uint16_t required[] = {
        reg::kFault, reg::kOperation, reg::kSetFrequency, reg::kOutputFrequency,
        reg::kOutputCurrent, reg::kBusVoltage, reg::kOutputVoltage, reg::kAnalogInput,
        reg::kCurrentCount, reg::kMotorSpeed, reg::kTemperature, reg::kPidFeedback,
        reg::kPidSetting, reg::kPulseInput, reg::kCurrentFault, reg::kTimingValue,
        reg::kInputTerminal, reg::kOutputTerminal, reg::kStatus, reg::kDirection,
    };
    for (const std::uint16_t address_value : required) {
        const Result<std::uint16_t> found = get(address_value);
        if (!found) return found.error();
    }

    Status s;
    s.address = address;
    s.fault = values.at(reg::kFault);
    s.operation = values.at(reg::kOperation);
    s.set_frequency = values.at(reg::kSetFrequency);
    s.output_frequency = values.at(reg::kOutputFrequency);
    s.output_current = values.at(reg::kOutputCurrent);
    s.bus_voltage = values.at(reg::kBusVoltage);
    s.output_voltage = values.at(reg::kOutputVoltage);
    s.analog_input = values.at(reg::kAnalogInput);
    s.current_count = values.at(reg::kCurrentCount);
    s.motor_speed = values.at(reg::kMotorSpeed);
    s.temperature = values.at(reg::kTemperature);
    s.pid_feedback = values.at(reg::kPidFeedback);
    s.pid_setting = values.at(reg::kPidSetting);
    s.pulse_input = values.at(reg::kPulseInput);
    s.current_fault = values.at(reg::kCurrentFault);
    s.timing_value = values.at(reg::kTimingValue);
    s.input_terminal = values.at(reg::kInputTerminal);
    s.output_terminal = values.at(reg::kOutputTerminal);
    s.status = values.at(reg::kStatus);
    s.direction = values.at(reg::kDirection);
    return s;
}

}  // namespace libs::su300
