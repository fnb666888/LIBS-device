#include "libs/protocols/laser.hpp"

#include <algorithm>
#include <stdexcept>

namespace libs::laser {
namespace {

constexpr std::size_t kHeaderBytes = 5;   // 5A A5 len 00 cmd
constexpr std::size_t kCrcBytes = 2;
// CRC 覆盖 [地址, 命令, 数据...]，不含帧头 5A A5 / 长度，也不含 CRC 自身。
// 编码与解码必须用同一个表达式：曾因两处各写各的导致本地组装的帧解码必然校验失败。
constexpr std::size_t kCrcSkipBytes = 3;
std::size_t crc_covered(std::size_t frame_size) { return frame_size - kCrcSkipBytes - kCrcBytes; }

std::uint16_t be16(const Bytes& d, std::size_t at) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(d.at(at)) << 8) | d.at(at + 1));
}

std::uint32_t be32(const Bytes& d, std::size_t at) {
    return (static_cast<std::uint32_t>(d.at(at)) << 24)
         | (static_cast<std::uint32_t>(d.at(at + 1)) << 16)
         | (static_cast<std::uint32_t>(d.at(at + 2)) << 8)
         | static_cast<std::uint32_t>(d.at(at + 3));
}

// 读响应的固定负载长度（数据段，不含帧头帧尾）。
std::size_t payload_length(std::uint8_t command) {
    switch (command) {
        case 0xB1: return 18;
        case 0xB2: return 22;
        case 0xBC: return 20;
        default: return 0;
    }
}

const char* kind_of(std::uint8_t command) {
    switch (command) {
        case 0xB1: return "ld_settings";
        case 0xB2: return "ld_actual";
        default: return "tec";
    }
}

Field quantity(std::uint16_t raw, const char* name, const char* unit, double scale) {
    return Field{raw, name, unit, scale, true};
}

// 32 位量。放电计数器是协议里唯一的 32 位字段，单独给一个工厂函数，
// 就是为了让调用方**没有机会**把它截成 16 位（曾经真的截过一次）。
Field quantity32(std::uint32_t raw, const char* name, const char* unit, double scale) {
    Field f{};
    f.raw32 = raw;
    f.raw = static_cast<std::uint16_t>(raw & 0xFFFF);   // 兼容旧的 16 位通道
    f.name = name;
    f.unit = unit;
    f.scale = scale;
    f.scaled = true;
    f.wide = true;
    return f;
}

// 协议标注为 NC、或换算语义未确认的字段：保留 raw，但 scaled=false，
// 调用方据此知道 value 不代表已标定的工程量。
Field unscaled(std::uint16_t raw, const char* name, const char* unit, double candidate_scale) {
    Field f{raw, name, unit, candidate_scale, false};
    return f;
}

Field bit_field(std::uint8_t raw, const char* name, bool known) {
    Field f{};
    f.raw = raw;
    f.name = name;
    f.scaled = known;
    return f;
}

}  // namespace

bool is_read_command(std::uint8_t command) noexcept {
    for (const auto c : kReadCommands)
        if (c == command) return true;
    return false;
}

std::uint16_t crc16(const std::uint8_t* bytes, std::size_t size) {
    if (size && !bytes) throw std::invalid_argument("null CRC buffer");
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xa001)
                            : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}

Bytes make_frame(std::uint8_t command, const Bytes& payload) {
    if (payload.size() > 251) throw std::invalid_argument("laser payload exceeds 251 bytes");
    Bytes frame{0x5A, 0xA5, static_cast<std::uint8_t>(payload.size() + kCrcBytes + 2), 0x00, command};
    frame.insert(frame.end(), payload.begin(), payload.end());
    // 先占住两个 CRC 字节再回填：这样编码与解码都走同一个 crc_covered() 表达式，
    // 不会出现"本地能组帧、decode_frame 却校验失败"或越界读一字节的情况。
    frame.push_back(0x00);
    frame.push_back(0x00);
    const auto crc = crc16(frame.data() + kCrcSkipBytes, crc_covered(frame.size()));
    frame[frame.size() - 2] = static_cast<std::uint8_t>(crc);
    frame[frame.size() - 1] = static_cast<std::uint8_t>(crc >> 8);
    return frame;
}

Bytes make_read_request(std::uint8_t command) {
    if (!is_read_command(command))
        throw std::invalid_argument("read request is limited to B1, B2 and BC");
    return make_frame(command, {});
}

Bytes make_write_frame(std::uint8_t command, const Bytes& payload) {
    return make_frame(command, payload);
}

Result<Frame> decode_frame(const Bytes& bytes, std::uint8_t expected_command) {
    if (!is_read_command(expected_command))
        return Error::make(ErrorKind::unsupported, "unsupported_command",
                           "Only B1, B2 and BC read responses are decoded by this function");
    if (bytes.size() < kHeaderBytes + kCrcBytes)
        return Error::with_raw(ErrorKind::protocol, "truncated",
                               "A complete laser frame needs at least seven bytes", bytes);
    if (bytes[0] != 0x5A || bytes[1] != 0xA5)
        return Error::with_raw(ErrorKind::protocol, "bad_header",
                               "The response must begin with 5A A5", bytes);
    if (bytes[2] < 4 || bytes.size() != static_cast<std::size_t>(bytes[2]) + 3)
        return Error::with_raw(ErrorKind::protocol, "bad_length",
                               "The length byte must match the actual frame size exactly", bytes);
    if (bytes[3] != 0x00)
        return Error::with_raw(ErrorKind::protocol, "bad_address",
                               "The transcribed protocol uses device address 00", bytes);
    if (bytes[4] != expected_command)
        return Error::with_raw(ErrorKind::protocol, "unexpected_command",
                               "The response command does not match the pending request", bytes);

    const auto expected_crc = crc16(bytes.data() + kCrcSkipBytes, crc_covered(bytes.size()));
    const auto actual_crc = static_cast<std::uint16_t>(bytes[bytes.size() - 2]) |
                            (static_cast<std::uint16_t>(bytes.back()) << 8);
    if (expected_crc != actual_crc)
        return Error::with_raw(ErrorKind::protocol, "crc_mismatch",
                               "CRC must cover the address, command and payload", bytes);

    const std::size_t expected_payload = payload_length(expected_command);
    if (bytes.size() != expected_payload + kHeaderBytes + kCrcBytes)
        return Error::with_raw(ErrorKind::protocol, "bad_payload_length",
                               "Read responses have fixed payload lengths; an echo is not a response",
                               bytes);

    Frame frame;
    frame.command = static_cast<Command>(expected_command);
    frame.kind = kind_of(expected_command);
    frame.payload.assign(bytes.begin() + kHeaderBytes, bytes.end() - kCrcBytes);
    const Bytes& d = frame.payload;

    if (expected_command == 0xB1) {
        frame.fields = {
            quantity(be16(d, 0),  "discharge_current",   "A",    1.0),
            quantity(be16(d, 2),  "discharge_pulse_width", "us", 1.0),
            quantity(be16(d, 4),  "discharge_frequency", "Hz",   1.0),
            quantity(be16(d, 6),  "discharge_delay",     "us",   1.0),
            quantity(be16(d, 8),  "q_delay",             "us",   1.0),
            unscaled(be16(d, 10), "ld_temperature_limit", "degC", 0.1),
            quantity(be16(d, 12), "voltage_limit",       "V",    1.0),
            quantity(be16(d, 14), "q_divider",           "",     1.0),
            quantity(be16(d, 16), "q_warmup",            "s",    1.0),
        };
        frame.notes = "B1 data[11] is marked NC; q_divider range conflicts with the single-trigger requirement";
    } else if (expected_command == 0xB2) {
        frame.fields = {
            bit_field(d[0], "work_state", true),
            bit_field(d[1], "clock_mode", true),
            bit_field(d[2], "fault_bits", true),
            bit_field(d[3], "interlock_1", d[3] <= 1),
            bit_field(d[4], "interlock_2", d[4] <= 1),
            bit_field(d[5], "emergency_stop", d[5] <= 1),
            bit_field(d[6], "overtemperature", d[6] <= 1),
            unscaled(be16(d, 7),  "ld_temperature",      "degC", 0.1),
            quantity(be16(d, 9),   "actual_current",      "A",    1.0),
            quantity(be16(d, 11),  "actual_voltage",      "V",    1.0),
            quantity32(be32(d, 13), "discharge_counter", "", 1.0),
            unscaled(be16(d, 17), "heatsink_temperature", "degC", 0.1),
            bit_field(d[19], "q_gate", d[19] <= 1),
            bit_field(d[20], "work_enabled", d[20] <= 1),
            bit_field(d[21], "discharge_clock_enabled", d[21] <= 1),
        };
        frame.notes = "temperature fields are NC / signedness unconfirmed; discharge_counter may wrap at 16 bits";
    } else {
        frame.fields = {
            unscaled(be16(d, 0),  "tec_max_temperature",   "degC", 0.1),
            unscaled(be16(d, 2),  "tec_min_temperature",   "degC", 0.1),
            quantity(be16(d, 4),  "tec_b_coefficient",     "",     1.0),
            quantity(be16(d, 6),  "thermistor_resistance", "kohm", 0.1),
            quantity(be16(d, 8),  "stage_voltage",         "V",    0.1),
            quantity(be16(d, 10), "tec_voltage",           "V",    0.1),
            quantity(be16(d, 12), "kp",                    "",     0.1),
            unscaled(be16(d, 14), "ki",                    "",     0.01),
            quantity(be16(d, 16), "kd",                    "",     0.1),
            unscaled(be16(d, 18), "tec_actual_temperature", "degC", 0.1),
            unscaled(be16(d, 20), "work_intensity",        "",     1.0),
        };
        frame.notes = "Ki read scale (0.01) conflicts with the documented write scale (0.1); temperature signedness unspecified";
    }
    return Result<Frame>::success(std::move(frame));
}

FrameScan find_frame(const Bytes& capture) {
    for (std::size_t i = 0; i + kHeaderBytes <= capture.size(); ++i) {
        if (capture[i] != 0x5A || capture[i + 1] != 0xA5) continue;
        const std::size_t length = static_cast<std::size_t>(capture[i + 2]) + 3;
        if (capture[i + 2] < 4) return FrameScan{FrameStatus::invalid, i, 0, "length byte below the 4-byte minimum"};
        if (i + length > capture.size())
            return FrameScan{FrameStatus::partial, i, length, "frame header seen, payload not complete yet"};
        const Bytes candidate(capture.begin() + static_cast<std::ptrdiff_t>(i),
                              capture.begin() + static_cast<std::ptrdiff_t>(i + length));
        const auto expected_crc = crc16(candidate.data() + kCrcSkipBytes, crc_covered(candidate.size()));
        const auto actual_crc = static_cast<std::uint16_t>(candidate[candidate.size() - 2]) |
                                (static_cast<std::uint16_t>(candidate.back()) << 8);
        if (expected_crc != actual_crc)
            return FrameScan{FrameStatus::invalid, i, length, "CRC mismatch"};
        if (!is_read_command(candidate[4]))
            return FrameScan{FrameStatus::invalid, i, length, "not a read response command"};
        if (candidate.size() != payload_length(candidate[4]) + kHeaderBytes + kCrcBytes)
            return FrameScan{FrameStatus::invalid, i, length, "payload length does not match the command"};
        return FrameScan{FrameStatus::complete, i, length, ""};
    }
    return FrameScan{FrameStatus::invalid, 0, 0, "no 5A A5 header found"};
}

}  // namespace libs::laser



