#include "libs/protocols/modbus_rtu.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace libs::modbus {

std::uint16_t crc16(const std::uint8_t* bytes, std::size_t size) {
    if (size && !bytes) throw std::invalid_argument("null CRC buffer");
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc >> 1) ^ ((crc & 1) ? 0xa001 : 0));
        }
    }
    return crc;
}

bool is_unicast_address(std::uint8_t address) noexcept {
    return address >= 1 && address <= 247;
}

Bytes make_request(std::uint8_t address, std::uint8_t function,
                   std::uint16_t reg, std::uint16_t value) {
    if (!is_unicast_address(address))
        throw std::invalid_argument("Modbus address must be 1..247; broadcast is not supported");
    Bytes bytes{address, function,
                static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg),
                static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    const auto crc = crc16(bytes.data(), bytes.size());
    bytes.push_back(static_cast<std::uint8_t>(crc & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(crc >> 8));
    return bytes;
}

Result<Frame> locate_reply(const Bytes& capture,
                           std::uint8_t address, std::uint8_t function,
                           const Bytes& exact_request, std::size_t normal_length,
                           const std::function<bool(const std::uint8_t*)>& valid_normal_header,
                           bool reject_request_echo) {
    if (capture.size() > 65536)
        return Error::make(ErrorKind::transport, "capture_too_large",
                           "Serial capture exceeds 64 KiB; the frame is not trustworthy");
    if (!valid_normal_header)
        throw std::invalid_argument("locate_reply requires a normal-header validator");

    std::vector<Frame> found;
    for (std::size_t i = 0; i < capture.size(); ++i) {
        // USB/RS485 适配器会回送请求；一个 8 字节的请求回显绝不能被当成状态字。
        if (reject_request_echo && capture.size() - i >= exact_request.size() &&
            std::equal(exact_request.begin(), exact_request.end(), capture.begin() + i)) {
            i += exact_request.size() - 1;
            continue;
        }
        if (capture.size() - i < 2 || capture[i] != address) continue;
        const bool exception = capture[i + 1] == static_cast<std::uint8_t>(function | 0x80);
        if (!exception && capture[i + 1] != function) continue;
        const std::size_t length = exception ? 5 : normal_length;
        if (capture.size() - i < length) continue;
        if (exception ? capture[i + 2] == 0 : !valid_normal_header(capture.data() + i)) continue;
        const auto expected = crc16(capture.data() + i, length - 2);
        const auto actual = static_cast<std::uint16_t>(capture[i + length - 2] |
                                                      (static_cast<std::uint16_t>(capture[i + length - 1]) << 8));
        if (expected != actual) continue;
        found.push_back(Frame{Bytes(capture.begin() + i, capture.begin() + i + length),
                              i, exception, exception ? static_cast<std::uint8_t>(capture[i + 2]) : std::uint8_t{0}});
        i += length - 1;
    }
    if (found.empty())
        return Error::with_raw(ErrorKind::protocol, "no_matching_reply",
                               "No CRC-valid matching response: incomplete/corrupt frame, "
                               "wrong address/function/count, or request echo only",
                               capture);
    if (found.size() != 1)
        return Error::with_raw(ErrorKind::protocol, "ambiguous_reply",
                               "Multiple CRC-valid matching responses in one transaction; "
                               "the response is ambiguous",
                               capture);
    return Result<Frame>::success(found.front());
}

}  // namespace libs::modbus




