#include "libs/protocols/rangefinder.hpp"

#include <stdexcept>

namespace libs::rangefinder {
namespace {
using Bytes = libs::Bytes;
}  // namespace

Register describe_register(std::uint16_t address, std::uint16_t raw) {
    Register r;
    r.address = address;
    r.raw = raw;
    switch (address) {
        case reg::kDistanceZeroOnOutOfRange:
            r.name = "distance_zero_on_out_of_range"; r.unit = "mm"; r.valid = raw != 0; break;
        case reg::kDistanceMaxOnOutOfRange:
            r.name = "distance_max_on_out_of_range";  r.unit = "mm"; r.valid = raw != 0xFFFF; break;
        case reg::kDistanceRaw:
            r.name = "advanced_distance_raw"; r.unit = "mm"; break;   // 0.1 mm 标称，仅部分型号支持
        case reg::kReflectionPeak:   r.name = "reflection_peak_raw"; break;
        case reg::kConfidence:       r.name = "confidence"; r.unit = "percent"; r.valid = raw <= 100; break;
        case reg::kVelocity:         r.name = "velocity_raw"; r.unit = "mm/s"; break;
        case reg::kTemperature:      r.name = "temperature_raw"; r.unit = "degC"; break;
        case reg::kSampleCounter:    r.name = "sample_counter_raw"; break;
        case reg::kSlaveAddress:     r.name = "slave_address_raw"; break;
        case 0x0209: case 0x020C:    r.name = "reserved"; break;
        case 0x020A:                 r.name = "reflection_threshold_raw"; break;
        case 0x020B:                 r.name = "confidence_threshold_raw"; break;
        case 0x020D:                 r.name = "application_distance_lower_raw"; break;
        case 0x020E:                 r.name = "application_distance_upper_raw"; break;
        case 0x020F:                 r.name = "filter_coefficient_raw"; break;
        case 0x0210:                 r.name = "filter_threshold_raw"; break;
        default:                     r.name = "unknown_raw"; break;
    }
    return r;
}

Bytes make_read_request(std::uint8_t address, std::uint16_t start, std::uint16_t count) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("Modbus address must be 1..247");
    if (count == 0 || count > 125 || static_cast<std::uint32_t>(start) + count > 65536)
        throw std::invalid_argument("register range must hold 1..125 registers without wrapping");
    return modbus::make_request(address, 0x03, start, count);
}

Result<Reading> decode_read_response(const Bytes& capture, std::uint8_t address,
                                     std::uint16_t start, std::uint16_t count) {
    const Bytes request = make_read_request(address, start, count);
    const std::size_t normal_length = 5 + static_cast<std::size_t>(count) * 2;
    auto located = modbus::locate_reply(capture, address, 0x03, request, normal_length,
                                        [count](const std::uint8_t* p) {
                                            // 正常响应：字节数 == 2*count。
                                            // 注意：功能码 0x03 的响应体里没有"起始寄存器"回显，
                                            // 只有 字节数 + 数据。早期版本误把请求的 start 当作回显来校验，
                                            // 结果是真实设备返回的合法帧一律被判为不匹配。
                                            return p[2] == static_cast<std::uint8_t>(count * 2);
                                        });
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "modbus_exception",
                               "the rangefinder returned a Modbus exception (code " +
                                   std::to_string(frame.exception_code) + ")",
                               frame.bytes);

    Reading reading;
    reading.address = address;
    reading.start = start;
    reading.count = count;
    reading.registers.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint16_t addr = static_cast<std::uint16_t>(start + i);
        const std::uint16_t raw = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(frame.bytes[3 + 2 * i]) << 8) | frame.bytes[4 + 2 * i]);
        reading.registers.push_back(describe_register(addr, raw));
    }
    return reading;
}

}  // namespace libs::rangefinder
