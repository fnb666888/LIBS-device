#include "libs/devices/indicator.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

namespace libs::devices {

Result<LampQuery> Indicator::query(std::uint8_t address, libs::indicator::Channel channel) {
    Bytes request;
    try {
        request = libs::indicator::make_query_request(address, channel);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }

    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    LampQuery result;
    result.request = request;
    result.raw_response = reply.bytes;
    result.elapsed_ms = reply.elapsed_ms;
    result.receive_errors = reply.receive_errors;
    result.timed_out = reply.timed_out;

    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while querying the lamp",
                               reply.bytes);

    auto decoded = libs::indicator::decode_query_response(reply.bytes, address, channel);
    if (!decoded) return decoded.error();
    result.response = decoded.value();
    return result;
}

Result<LampWrite> Indicator::write(std::uint8_t address, const libs::indicator::WriteRequest& request) {
    auto frame = libs::indicator::make_write_request(address, request);
    if (!frame) return frame.error();
    const Bytes write_frame = frame.value();

    auto exchanged = link_.exchange(write_frame, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    LampWrite result;
    result.write_request = write_frame;
    result.write_response = reply.bytes;
    result.write_echo_seen = std::search(reply.bytes.begin(), reply.bytes.end(),
                                         write_frame.begin(), write_frame.end()) != reply.bytes.end();
    result.model = request.model;
    result.channel = request.channel;
    result.state = request.state;
    result.elapsed_ms = reply.elapsed_ms;
    result.receive_errors = reply.receive_errors;

    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while writing to the lamp",
                               reply.bytes);
    return result;
}

}  // namespace libs::devices
