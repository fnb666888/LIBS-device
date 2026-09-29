#include "libs/devices/laser.hpp"

#include <algorithm>

namespace libs::devices {

Result<LaserTransaction> Laser::read(std::uint8_t command) {
    if (!libs::laser::is_read_command(command))
        return Error::make(ErrorKind::invalid_argument, "bad_command",
                           "read is limited to B1, B2 and BC");

    Bytes request;
    try {
        request = libs::laser::make_read_request(command);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "request_build_failed", e.what());
    }

    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    LaserTransaction tx;
    tx.request = request;
    tx.response = reply.bytes;
    tx.elapsed_ms = reply.elapsed_ms;
    tx.receive_errors = reply.receive_errors;
    tx.last_receive_tick = reply.last_receive_tick;
    tx.timed_out = reply.timed_out;

    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while reading the laser",
                               reply.bytes);

    // 字节流里可能混有回显或噪声：逐个偏移尝试，只接受恰好一帧自洽的响应。
    unsigned complete_frames = 0;
    Error last_error = Error::make(ErrorKind::protocol, "no_frame", "no complete laser frame in the reply");
    for (std::size_t offset = 0; offset + 7 <= reply.bytes.size(); ++offset) {
        const auto scan = libs::laser::find_frame(Bytes(reply.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                                       reply.bytes.end()));
        if (scan.status == libs::laser::FrameStatus::invalid) continue;
        // find_frame 从头扫描，这里手动对齐到当前偏移。
        Bytes window(reply.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                     reply.bytes.begin() + static_cast<std::ptrdiff_t>(offset + scan.length));
        auto decoded = libs::laser::decode_frame(window, command);
        if (!decoded) { last_error = decoded.error(); continue; }
        tx.frame = std::move(decoded.value());
        ++complete_frames;
        offset += scan.length - 1;
    }

    if (complete_frames == 0) return last_error;
    if (complete_frames > 1)
        return Error::with_raw(ErrorKind::protocol, "ambiguous_reply",
                               "the reply contains more than one complete laser frame; "
                               "it is not clear which one answers this request",
                               reply.bytes);
    return tx;
}

Result<LaserTransaction> Laser::write(std::uint8_t command, const Bytes& payload) {
    Bytes request;
    try {
        request = libs::laser::make_write_frame(command, payload);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "request_build_failed", e.what());
    }
    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    LaserTransaction tx;
    tx.request = request;
    tx.response = reply.bytes;
    tx.elapsed_ms = reply.elapsed_ms;
    tx.receive_errors = reply.receive_errors;
    tx.last_receive_tick = reply.last_receive_tick;
    tx.timed_out = reply.timed_out;
    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while writing to the laser",
                               reply.bytes);
    return tx;
}

Result<LaserTransaction> Laser::send(std::uint8_t command, std::uint8_t value) {
    return write(command, Bytes{value});
}

}  // namespace libs::devices
