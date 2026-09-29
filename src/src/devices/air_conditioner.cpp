#include "libs/devices/air_conditioner.hpp"

#include <cmath>
#include <exception>

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

}  // namespace

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
    const auto def = libs::air_conditioner::describe_param(reg, 0);
    if (def.name == "unknown_raw")
        return Error::make(ErrorKind::invalid_argument, "unknown_register",
                           "register " + std::to_string(reg) + " is not in the parameter table");
    if (!def.writable)
        return Error::make(ErrorKind::invalid_argument, "not_writable",
                           "register " + std::to_string(reg) + " is read-only");
    if (def.scale == 0.0)
        return Error::make(ErrorKind::internal, "bad_scale", "register has no scale");

    const double scaled = value / def.scale;
    if (std::fabs(scaled) > 65535.0)
        return Error::make(ErrorKind::invalid_argument, "out_of_range",
                           "value does not fit a 16-bit register after scaling");
    libs::air_conditioner::ParamWrite w;
    w.reg = reg;
    w.raw = static_cast<std::uint16_t>(static_cast<std::int32_t>(std::llround(scaled)));
    if (find_range(reg, w.range_min, w.range_max)) {
        // 有符号区间（如 -30~15）换算后要用有符号比较，raw 仍按补码存。
        const std::int32_t signed_raw = static_cast<std::int16_t>(w.raw);
        const std::int32_t lo = static_cast<std::int16_t>(w.range_min);
        const std::int32_t hi = static_cast<std::int16_t>(w.range_max);
        if (signed_raw < lo || signed_raw > hi)
            return Error::make(ErrorKind::invalid_argument, "out_of_range",
                               "value " + std::to_string(value) + " " + def.unit +
                                   " is outside the documented range");
    }
    return write_param(address, w);
}

Result<ParamWrite> AirConditioner::set_mode(std::uint8_t address, libs::air_conditioner::Mode mode) {
    libs::air_conditioner::ParamWrite w;
    w.reg = libs::air_conditioner::reg::kMode;
    w.raw = static_cast<std::uint16_t>(mode);
    return write_param(address, w);
}

}  // namespace libs::devices


