#include "libs/protocols/air_conditioner.hpp"

#include <algorithm>
#include <stdexcept>

namespace libs::air_conditioner {
namespace {
using Bytes = libs::Bytes;

// 手册寄存器表只定义这些状态位；10001-10034 和若干保留位没有名称。
struct StatusDef {
    std::uint16_t offset;   // 相对 10001 的位序号（bit 序号，不是字节序号）
    const char* name;
    bool alarm;
};

constexpr StatusDef kStatusDefs[] = {
    {34, "cooling",              false},  // 10035
    {35, "heater",               false},  // 10036
    {36, "indoor_fan",           false},  // 10037
    {37, "outdoor_fan",          false},  // 10038
    {41, "power",                false},  // 10042 监控开关机
    {42, "indoor_sensor_fault",  true },  // 10043
    {43, "evaporator_sensor_fault", true},  // 10044
    {44, "condenser_sensor_fault",  true},  // 10045
    {46, "humidity_sensor_fault",   true},  // 10047
    {47, "humidity_high_alarm",    true},  // 10048
    {50, "condenser_high_alarm",   true},  // 10051
    {51, "evaporator_freeze_alarm",true},  // 10052
    {52, "cooling_alarm",          true},  // 10053 压缩机 & 制冷系统
    {53, "high_temp_alarm",        true},  // 10054
    {54, "low_temp_alarm",         true},  // 10055
};

struct ParamDef {
    std::uint16_t doc;        // 30001..30035
    const char* name;
    const char* unit;
    double scale;
    bool writable;            // 手册 4xxxx 空间里存在
    bool documented;
    bool is_signed;           // 该参数按 16 位补码解释（温度、湿度、校正值）
};

constexpr ParamDef kParamDefs[] = {
    {30001, "indoor_temp",                "degC", 0.1, false, true,  true},
    {30002, "evaporator_temp",            "degC", 0.1, false, true,  true},
    {30003, "condenser_temp",             "degC", 0.1, false, true,  true},
    {30004, "reserved_30004",             "degC", 0.1, false, false, true},
    {30005, "reserved_30005",             "",     1.0, false, false, false},
    {30006, "reserved_30006",             "",     1.0, false, false, false},
    {30009, "humidity",                   "%RH",  0.1, false, true,  true},
    {30010, "reserved_30010",             "%RH",  0.1, false, false, true},
    {30011, "reserved_30011",             "%RH",  0.1, false, false, true},
    {30014, "cooling_on_setpoint",        "degC", 0.1, true,  true,  true},
    {30015, "cooling_off_setpoint",       "degC", 0.1, true,  true,  true},
    {30016, "heating_on_setpoint",        "degC", 0.1, true,  true,  true},
    {30017, "heating_off_setpoint",       "degC", 0.1, true,  true,  true},
    {30018, "indoor_high_alarm_setpoint", "degC", 0.1, true,  true,  true},
    {30019, "indoor_low_alarm_setpoint",  "degC", 0.1, true,  true,  true},
    {30020, "condenser_high_protect",     "degC", 0.1, true,  true,  true},
    {30021, "evaporator_freeze_protect",  "degC", 0.1, true,  true,  true},
    {30022, "dehumidify_on_humidity",     "%RH",  0.1, true,  true,  true},
    {30023, "dehumidify_off_humidity",    "%RH",  0.1, true,  true,  true},
    {30024, "humidity_alarm_limit",       "%RH",  0.1, true,  true,  true},
    {30025, "humidity_calibration",       "",     1.0, true,  true,  true},
    {30026, "mode",                       "",     1.0, true,  true,  false},
    {30027, "baud_rate",                  "",     1.0, true,  true,  false},
    {30028, "slave_address",              "",     1.0, true,  true,  false},
};


const ParamDef* find_param(std::uint16_t doc) {
    for (const auto& d : kParamDefs) if (d.doc == doc) return &d;
    return nullptr;
}

}  // namespace

const char* to_string(Mode mode) noexcept {
    switch (mode) {
        case Mode::standby:     return "standby";
        case Mode::cooling:     return "cooling";
        case Mode::heating:     return "heating";
        case Mode::dehumidify: return "dehumidify";
    }
    return "standby";
}

ParamRegister describe_param(std::uint16_t address, std::uint16_t raw) {
    ParamRegister r;
    r.address = address;
    r.raw = raw;
    const ParamDef* d = find_param(address);
    if (!d) {
        r.name = "unknown_raw";
        r.documented = false;
        return r;
    }
    r.name = d->name;
    r.unit = d->unit;
    r.scale = d->scale;
    r.documented = d->documented;
    r.writable = d->writable;
    r.is_signed = d->is_signed;
    // 有符号性是"这个参数本身"的属性，写进表里，不要靠单位字符串猜：
    // 30025 湿度校正值单位是空的 x1，但样本 0xFFFC 就是 -4，必须按补码解释；
    // 反过来 30026 模式 / 30027 波特率 / 30028 地址同样无单位，却是无符号的。
    if (d->is_signed)
        r.value = static_cast<double>(static_cast<std::int16_t>(raw)) * d->scale;
    else
        r.value = static_cast<double>(raw) * d->scale;
    return r;
}

Bytes make_status_request(std::uint8_t address, std::uint16_t start, std::uint16_t count) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("Modbus address must be 1..247");
    if (start < reg::kStatusFirst || start > reg::kStatusLast)
        throw std::invalid_argument("status start register must be 10001..10058");
    if (count == 0 || start + count - 1 > reg::kStatusLast)
        throw std::invalid_argument("status count must keep the request within 10001..10058");
    return modbus::make_request(address, 0x01, static_cast<std::uint16_t>(start - reg::kStatusFirst), count);
}

Bytes make_param_request(std::uint8_t address, std::uint16_t start, std::uint16_t count) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("Modbus address must be 1..247");
    if (start < reg::kIndoorTemp || start > reg::kParamLast)
        throw std::invalid_argument("parameter start register must be 30001..30035");
    if (count == 0 || count > 50 || start + count - 1 > reg::kParamLast)
        throw std::invalid_argument("parameter count must be 1..50 and stay within 30001..30035");
    return modbus::make_request(address, 0x03, static_cast<std::uint16_t>(start - reg::kIndoorTemp), count);
}

Bytes make_control_request(std::uint8_t address, std::uint16_t r, bool on) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("Modbus address must be 1..247");
    if (r != reg::kPowerState)
        throw std::invalid_argument("FC05 may only drive the power register 10042");
    // 手册 4.5 注 1 写的是 "sReg = 寄存器号 - 00001"，但厂商自己的样本 §7.11/§7.12
    // 用的偏移是 0x0029 = 41 = 10042 - 10001，即与 FC01 同一个 1xxxx 基准。
    // 按 00001 换算会得到 10041，设备不会认。这里以实测样本为准。
    return modbus::make_request(address, 0x05, static_cast<std::uint16_t>(r - reg::kStatusFirst), on ? coil::kOn : coil::kOff);
}

Result<Bytes> make_param_write_request(std::uint8_t address, const ParamWrite& w) {
    if (!modbus::is_unicast_address(address))
        return Error::make(ErrorKind::invalid_argument, "bad_address",
                           "Modbus address must be 1..247");
    const auto* d = find_param(w.reg);
    if (!d || !d->writable)
        return Error::make(ErrorKind::invalid_argument, "not_writable",
                           "register " + std::to_string(w.reg) + " is not in the documented FC06 set");
    if (w.range_min != 0 || w.range_max != 0) {
        // 范围端点按 16 位补码比较：手册里有负下限（如 30016 加热开启点 -30.0 degC
        // -> raw 0xFED4），用无符号比较会把 -100（0xFF9C）判成越界。
        const std::int32_t value = static_cast<std::int16_t>(w.raw);
        const std::int32_t lo = static_cast<std::int16_t>(w.range_min);
        const std::int32_t hi = static_cast<std::int16_t>(w.range_max);
        if (value < lo || value > hi)
            return Error::make(ErrorKind::invalid_argument, "out_of_range",
                               "raw " + std::to_string(value) + " is outside the documented range " +
                                   std::to_string(lo) + ".." + std::to_string(hi));
    }
    return modbus::make_request(address, 0x06, static_cast<std::uint16_t>(w.reg - 30001), w.raw);
}

Result<StatusReading> decode_status_response(const Bytes& capture, std::uint8_t address,
                                            std::uint16_t start, std::uint16_t count) {
    const Bytes request = make_status_request(address, start, count);
    const std::size_t expected_len = static_cast<std::size_t>((count + 7) / 8);
    auto located = modbus::locate_reply(capture, address, 0x01, request, 5 + expected_len,
                                        [expected_len](const std::uint8_t* p) {
                                            return p[2] == static_cast<std::uint8_t>(expected_len);
                                        });
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "modbus_exception",
                               "the air conditioner returned a Modbus exception (code " +
                                   std::to_string(frame.exception_code) + ")",
                               frame.bytes);

    StatusReading reading;
    reading.address = address;
    reading.start = start;
    reading.count = count;
    reading.raw_bytes.assign(frame.bytes.begin() + 3, frame.bytes.begin() + 3 + expected_len);
    // 位图以"本次请求区间的第一位"作为 byte0 bit0，所以下标必须相对 start 算。
    // 直接拿绝对偏移整除 8 只有在 start == 10001 时才对，§7.10（start = 10035）会整体错位。
    const std::uint16_t base = static_cast<std::uint16_t>(start - reg::kStatusFirst);
    for (const auto& def : kStatusDefs) {
        if (def.offset < base || def.offset >= base + count) continue;
        const std::size_t relative = static_cast<std::size_t>(def.offset - base);
        const std::size_t byte_index = relative / 8;
        const std::size_t bit_index = relative % 8;
        if (byte_index >= reading.raw_bytes.size()) continue;
        StatusBit b;
        b.address = static_cast<std::uint16_t>(reg::kStatusFirst + def.offset);
        b.active = ((reading.raw_bytes[byte_index] >> bit_index) & 1u) != 0;
        b.name = def.name;
        b.is_alarm = def.alarm;
        reading.bits.push_back(std::move(b));
    }
    return reading;
}

Result<ParameterReading> decode_param_response(const Bytes& capture, std::uint8_t address,
                                               std::uint16_t start, std::uint16_t count) {
    const Bytes request = make_param_request(address, start, count);
    const std::size_t normal_length = 5 + static_cast<std::size_t>(count) * 2;
    auto located = modbus::locate_reply(capture, address, 0x03, request, normal_length,
                                        [count](const std::uint8_t* p) {
                                            return p[2] == static_cast<std::uint8_t>(count * 2);
                                        });
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "modbus_exception",
                               "the air conditioner returned a Modbus exception (code " +
                                   std::to_string(frame.exception_code) + ")",
                               frame.bytes);

    ParameterReading reading;
    reading.address = address;
    reading.start = start;
    reading.count = count;
    reading.registers.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint16_t doc = static_cast<std::uint16_t>(start + i);
        const std::uint16_t raw = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(frame.bytes[3 + 2 * i]) << 8) | frame.bytes[4 + 2 * i]);
        reading.registers.push_back(describe_param(doc, raw));
    }
    return reading;
}

Result<ControlResponse> decode_control_response(const Bytes& capture, std::uint8_t address,
                                               std::uint16_t r, bool on) {
    const Bytes request = make_control_request(address, r, on);
    auto located = modbus::locate_reply(capture, address, 0x05, request, 8,
                                        [r](const std::uint8_t* p) {
                                            const std::uint16_t echo = static_cast<std::uint16_t>(
                                                (static_cast<std::uint16_t>(p[2]) << 8) | p[3]);
                                            return echo == static_cast<std::uint16_t>(r - reg::kStatusFirst);
                                        },
                                        /*reject_request_echo=*/false);
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "modbus_exception",
                               "the air conditioner rejected the control write (code " +
                                   std::to_string(frame.exception_code) + ")",
                               frame.bytes);
    const std::uint16_t value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame.bytes[4]) << 8) | frame.bytes[5]);
    ControlResponse out;
    out.reg = r;
    out.value = value;
    out.on = (value == coil::kOn);
    return out;
}

Result<ParamWriteResponse> decode_param_write_response(const Bytes& capture, std::uint8_t address,
                                                       const ParamWrite& w) {
    auto frame_bytes = make_param_write_request(address, w);
    if (!frame_bytes) return frame_bytes.error();
    const Bytes request = frame_bytes.value();
    const std::uint16_t offset = static_cast<std::uint16_t>(w.reg - 30001);
    auto located = modbus::locate_reply(capture, address, 0x06, request, 8,
                                        [offset](const std::uint8_t* p) {
                                            const std::uint16_t echo = static_cast<std::uint16_t>(
                                                (static_cast<std::uint16_t>(p[2]) << 8) | p[3]);
                                            return echo == offset;
                                        },
                                        /*reject_request_echo=*/false);
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "modbus_exception",
                               "the air conditioner rejected the parameter write (code " +
                                   std::to_string(frame.exception_code) + ")",
                               frame.bytes);
    ParamWriteResponse out;
    out.reg = w.reg;
    out.value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(frame.bytes[4]) << 8) | frame.bytes[5]);
    return out;
}

}  // namespace libs::air_conditioner






