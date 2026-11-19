#include "libs/devices/air_conditioner.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <limits>

namespace libs::devices {
namespace {

// 手册 6.2 给的 FC06 可写范围（raw 单位，degC x10 / %RH x10 / x1）。
// 表里没列的（模式/波特率/地址）不在此限，调用方自己保证。
struct RangeTableEntry { std::uint16_t reg; std::uint16_t lo; std::uint16_t hi; };
constexpr RangeTableEntry kRanges[] = {
    {30014, 200, 550},   // 20.0 ~ 55.0 degC
    {30015, 150, 350},   // 15 ~ 35
    {30016, 0xFED4, 150},// -30 ~ 15  degC (0xFED4 = -300 补码)
    {30017, 0, 230},     // 0 ~ 23
    {30018, 300, 600},   // 30 ~ 60
    {30019, 0xFF9C, 100},// -10 ~ 10  (0xFF9C = -100 补码)
    {30022, 0, 1000},
    {30023, 0, 1000},
    {30024, 0, 1000},
};

bool find_range(std::uint16_t reg, std::uint16_t& lo, std::uint16_t& hi) {
    for (const auto& e : kRanges) {
        if (e.reg == reg) { lo = e.lo; hi = e.hi; return true; }
    }
    return false;
}

namespace reg = libs::air_conditioner::reg;
struct SetpointField {
    std::uint16_t address;
    double AirConditionerSetpoints::*member;
};
constexpr std::array<SetpointField, 6> kSetpointFields{{
    {reg::kCoolingOnSetpoint, &AirConditionerSetpoints::cooling_on_c},
    {reg::kCoolingOffSetpoint, &AirConditionerSetpoints::cooling_off_c},
    {reg::kHeatingOnSetpoint, &AirConditionerSetpoints::heating_on_c},
    {reg::kHeatingOffSetpoint, &AirConditionerSetpoints::heating_off_c},
    {reg::kDehumidifyOnHumidity, &AirConditionerSetpoints::dehumidify_on_percent},
    {reg::kDehumidifyOffHumidity, &AirConditionerSetpoints::dehumidify_off_percent},
}};
constexpr auto kFirstSetpoint = kSetpointFields.front().address;
constexpr auto kSetpointSpan = kSetpointFields.back().address - kFirstSetpoint + 1;

Result<libs::air_conditioner::ParamWrite> encode_setpoint(std::uint16_t address, double value) {
    const auto def = libs::air_conditioner::describe_param(address, 0);
    if (def.name == "unknown_raw")
        return Error::make(ErrorKind::invalid_argument, "unknown_register", "unknown setpoint register");
    if (!def.writable)
        return Error::make(ErrorKind::invalid_argument, "not_writable", def.name + " is read-only");

    const double scaled = value / def.scale;
    const auto lo = def.is_signed ? std::numeric_limits<std::int16_t>::min() : 0;
    const auto hi = def.is_signed ? std::numeric_limits<std::int16_t>::max()
                                  : std::numeric_limits<std::uint16_t>::max();
    if (!std::isfinite(scaled) || scaled < lo || scaled > hi)
        return Error::make(ErrorKind::invalid_argument, "out_of_range", def.name + " is outside the register range");

    libs::air_conditioner::ParamWrite write{};
    write.reg = address;
    write.raw = static_cast<std::uint16_t>(std::llround(scaled));
    if (find_range(address, write.range_min, write.range_max)) {
        const double minimum = static_cast<std::int16_t>(write.range_min) * def.scale;
        const double maximum = static_cast<std::int16_t>(write.range_max) * def.scale;
        if (value < minimum || value > maximum)
            return Error::make(ErrorKind::invalid_argument, "out_of_range", def.name + " is outside the documented range");
    }
    return write;
}

AirConditionerSetpoints decode_setpoints(const libs::air_conditioner::ParameterReading& reading) {
    AirConditionerSetpoints values{};
    for (const auto& field : kSetpointFields)
        values.*field.member = reading.registers[field.address - kFirstSetpoint].value;
    return values;
}

}  // namespace

Result<libs::air_conditioner::StatusReading> AirConditioner::readStatus() {
    const auto result = read_status(address_);
    if (!result) return result.error();
    return result.value().reading;
}

Result<libs::air_conditioner::StatusReading> AirConditioner::readStatus(
        std::uint16_t start, std::uint16_t count) {
    const auto result = read_status(address_, start, count);
    if (!result) return result.error();
    return result.value().reading;
}

Result<libs::air_conditioner::ParameterReading> AirConditioner::readParams(
        std::uint16_t start, std::uint16_t count) {
    const auto result = read_params(address_, start, count);
    if (!result) return result.error();
    return result.value().reading;
}

Result<libs::air_conditioner::ParamRegister> AirConditioner::readParam(
        std::uint16_t reg) {
    const auto reading = readParams(reg, 1);
    if (!reading) return reading.error();
    if (reading.value().registers.empty())
        return Error::make(ErrorKind::protocol, "ac_param_absent",
                           "参数读取成功但响应中没有寄存器");
    return reading.value().registers.front();
}

Result<AirConditionerPoll> AirConditioner::poll() {
    AirConditionerPoll out;
    const auto status = read_status(address_);
    if (status) {
        out.status_ok = true;
        out.status = status.value().reading;
    } else {
        out.status_error = status.error().message;
    }

    const auto params = read_params(address_);
    if (params) {
        out.params_ok = true;
        out.params = params.value().reading;
    } else {
        out.params_error = params.error().message;
    }
    return out;
}

AirConditionerEnvironment AirConditioner::readEnvironment() {
    const auto polled = poll();
    AirConditionerEnvironment environment;
    if (!polled) {
        environment.status_error = polled.error().message;
        environment.parameters_error = polled.error().message;
        return environment;
    }
    const auto& reading = polled.value();
    environment.status_error = reading.status_error;
    environment.parameters_error = reading.params_error;
    for (const auto& bit : reading.status.bits) {
        if (bit.address == reg::kPowerState) environment.power_on = bit.active;
        if (bit.address == reg::kCoolingOn) environment.cooling = bit.active;
        if (bit.address == reg::kHeaterOn) environment.heating = bit.active;
        if (bit.is_alarm && bit.active) environment.active_alarms.push_back(bit.name);
    }
    for (const auto& field : reading.params.registers) {
        if (field.address == reg::kIndoorTemp) environment.cabinet_temperature_c = field.value;
        if (field.address == reg::kHumidity) environment.humidity_percent = field.value;
        if (field.address == reg::kMode) environment.mode_code = field.raw;
    }
    return environment;
}

Result<AirConditionerSetpoints> AirConditioner::readSetpoints() {
    const auto reading = readParams(kFirstSetpoint, kSetpointSpan);
    if (!reading) return reading.error();
    return decode_setpoints(reading.value());
}

Result<AirConditionerSetpoints> AirConditioner::applySetpoints(const AirConditionerSetpoints& target) {
    std::array<libs::air_conditioner::ParamWrite, kSetpointFields.size()> writes{};
    for (std::size_t i = 0; i < writes.size(); ++i) {
        const auto& field = kSetpointFields[i];
        const auto encoded = encode_setpoint(field.address, target.*field.member);
        if (!encoded) return encoded.error();
        writes[i] = encoded.value();
    }

    const auto before = readParams(kFirstSetpoint, kSetpointSpan);
    if (!before) return before.error();
    bool changed = false;
    for (const auto& write : writes) {
        if (before.value().registers[write.reg - kFirstSetpoint].raw == write.raw) continue;
        const auto written = writeParam(write);
        if (!written) {
            auto error = written.error();
            error.message = libs::air_conditioner::describe_param(write.reg, 0).name +
                            ": " + error.message + "; some setpoints may already have changed";
            return error;
        }
        changed = true;
    }
    if (!changed) return decode_setpoints(before.value());

    const auto after = readParams(kFirstSetpoint, kSetpointSpan);
    if (!after) return after.error();
    for (const auto& write : writes) {
        const auto& actual = after.value().registers[write.reg - kFirstSetpoint];
        if (actual.raw != write.raw)
            return Error::make(ErrorKind::device, "ac_setpoint_mismatch", actual.name +
                               " readback does not match the requested value");
    }
    return decode_setpoints(after.value());
}

Result<ParamWrite> AirConditioner::setMode(libs::air_conditioner::Mode mode) {
    return set_mode(address_, mode);
}

Result<ControlWrite> AirConditioner::setPower(bool on) {
    return set_power(address_, on);
}

Result<ParamWrite> AirConditioner::setSetpoint(std::uint16_t reg, double value) {
    return set_setpoint(address_, reg, value);
}

Result<ParamWrite> AirConditioner::writeParams(
        const std::vector<libs::air_conditioner::ParamWrite>& writes) {
    Result<ParamWrite> last =
        Error::make(ErrorKind::invalid_argument, "empty_param_write",
                    "至少需要一个参数写入");
    for (const auto& write : writes) {
        last = write_param(address_, write);
        if (!last) return last.error();
    }
    return last;
}

Result<ParamWrite> AirConditioner::writeParam(
        const libs::air_conditioner::ParamWrite& write) {
    return write_param(address_, write);
}

void AirConditioner::holdMode(libs::air_conditioner::Mode mode) {
    held_mode_ = mode;
    last_mode_sent_ms_ = 0;
}

Result<bool> AirConditioner::keepalive(std::uint64_t now_ms,
                                       unsigned keepalive_ms) {
    if (!held_mode_.has_value()) return false;
    if (keepalive_ms >= 100000) {
        return Error::make(ErrorKind::invalid_argument, "ac_keepalive_too_slow",
                           "模式保持间隔必须显著小于设备上限 100 s（手册 §6.2）");
    }
    if (last_mode_sent_ms_ != 0 &&
        now_ms - last_mode_sent_ms_ < static_cast<std::uint64_t>(keepalive_ms)) {
        return false;
    }
    const auto written = set_mode(address_, *held_mode_);
    if (!written) return written.error();
    last_mode_sent_ms_ = now_ms;
    return true;
}

std::vector<libs::air_conditioner::StatusBit> AirConditioner::activeAlarms(
        const libs::air_conditioner::StatusReading& status) {
    std::vector<libs::air_conditioner::StatusBit> out;
    for (const auto& bit : status.bits) {
        if (bit.is_alarm && bit.active) out.push_back(bit);
    }
    return out;
}

Result<StatusRead> AirConditioner::read_status(std::uint8_t address, std::uint16_t start,
                                               std::uint16_t count) {
    StatusRead out;
    try {
        out.request = libs::air_conditioner::make_status_request(address, start, count);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }
    auto exchanged = link_.exchange(out.request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();
    out.response = reply.bytes;
    out.elapsed_ms = reply.elapsed_ms;
    out.receive_errors = reply.receive_errors;
    out.timed_out = reply.timed_out;
    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while reading the air conditioner",
                               reply.bytes);
    auto decoded = libs::air_conditioner::decode_status_response(reply.bytes, address, start, count);
    if (!decoded) return decoded.error();
    out.reading = std::move(decoded.value());
    return out;
}

Result<ParamRead> AirConditioner::read_params(std::uint8_t address, std::uint16_t start,
                                              std::uint16_t count) {
    ParamRead out;
    try {
        out.request = libs::air_conditioner::make_param_request(address, start, count);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }
    auto exchanged = link_.exchange(out.request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();
    out.response = reply.bytes;
    out.elapsed_ms = reply.elapsed_ms;
    out.receive_errors = reply.receive_errors;
    out.timed_out = reply.timed_out;
    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while reading the air conditioner",
                               reply.bytes);
    auto decoded = libs::air_conditioner::decode_param_response(reply.bytes, address, start, count);
    if (!decoded) return decoded.error();
    out.reading = std::move(decoded.value());
    return out;
}

Result<ControlWrite> AirConditioner::set_power(std::uint8_t address, bool on) {
    ControlWrite out;
    try {
        out.request = libs::air_conditioner::make_control_request(address,
                            libs::air_conditioner::reg::kPowerState, on);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }
    auto exchanged = link_.exchange(out.request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    out.raw = exchanged.value().bytes;
    out.elapsed_ms = exchanged.value().elapsed_ms;
    auto decoded = libs::air_conditioner::decode_control_response(exchanged.value().bytes, address,
                            libs::air_conditioner::reg::kPowerState, on);
    if (!decoded) return decoded.error();
    out.response = std::move(decoded.value());
    return out;
}

Result<ParamWrite> AirConditioner::write_param(std::uint8_t address, const libs::air_conditioner::ParamWrite& w) {
    ParamWrite out;
    auto framed = libs::air_conditioner::make_param_write_request(address, w);
    if (!framed) return framed.error();
    out.request = framed.value();
    auto exchanged = link_.exchange(out.request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    out.raw = exchanged.value().bytes;
    out.elapsed_ms = exchanged.value().elapsed_ms;
    auto decoded = libs::air_conditioner::decode_param_write_response(exchanged.value().bytes, address, w);
    if (!decoded) return decoded.error();
    out.response = std::move(decoded.value());
    return out;
}

Result<ParamWrite> AirConditioner::set_setpoint(std::uint8_t address, std::uint16_t reg, double value) {
    const auto write = encode_setpoint(reg, value);
    if (!write) return write.error();
    return write_param(address, write.value());
}

Result<ParamWrite> AirConditioner::set_mode(std::uint8_t address, libs::air_conditioner::Mode mode) {
    libs::air_conditioner::ParamWrite w;
    w.reg = libs::air_conditioner::reg::kMode;
    w.raw = static_cast<std::uint16_t>(mode);
    return write_param(address, w);
}

}  // namespace libs::devices


