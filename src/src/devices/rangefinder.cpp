#include "libs/devices/rangefinder.hpp"

#include <exception>

namespace libs::devices {

Result<RangefinderRead> Rangefinder::read(std::uint8_t address, std::uint16_t start, std::uint16_t count) {
    Bytes request;
    try {
        request = libs::rangefinder::make_read_request(address, start, count);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }

    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    RangefinderRead result;
    result.request = request;
    result.response = reply.bytes;
    result.elapsed_ms = reply.elapsed_ms;
    result.receive_errors = reply.receive_errors;
    result.timed_out = reply.timed_out;

    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while reading the rangefinder",
                               reply.bytes);

    auto decoded = libs::rangefinder::decode_read_response(reply.bytes, address, start, count);
    if (!decoded) return decoded.error();
    result.reading = std::move(decoded.value());
    return result;
}

}  // namespace libs::devices
