#include "libs/devices/rangefinder.hpp"

#include <exception>

namespace libs::devices {

Result<libs::rangefinder::Reading> Rangefinder::readRawRegisters(
        std::uint16_t start, std::uint16_t count) {
    return read(address_, start, count);
}

Result<libs::rangefinder::Reading> Rangefinder::observeOnce() {
    return readRawRegisters(libs::rangefinder::reg::kDistanceZeroOnOutOfRange, 9);
}

Result<RangeObservation> Rangefinder::readObservation() {
    namespace reg = libs::rangefinder::reg;
    const auto reading = readRawRegisters(reg::kDistanceMaxOnOutOfRange,
                                          reg::kSampleCounter - reg::kDistanceMaxOnOutOfRange + 1);
    if (!reading) return reading.error();

    RangeObservation observation;
    observation.observed_at = std::chrono::steady_clock::now();
    for (const auto& field : reading.value().registers) {
        if (field.address == reg::kDistanceMaxOnOutOfRange && field.valid)
            observation.distance_mm = field.raw;
        if (field.address == reg::kConfidence && field.valid)
            observation.confidence_percent = static_cast<std::uint8_t>(field.raw);
        if (field.address == reg::kSampleCounter)
            observation.sample_counter = field.raw;
    }
    return observation;
}

Result<libs::rangefinder::Reading> Rangefinder::read(std::uint8_t address,
                                                     std::uint16_t start,
                                                     std::uint16_t count) {
    Bytes request;
    try {
        request = libs::rangefinder::make_read_request(address, start, count);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }

    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while reading the rangefinder",
                               reply.bytes);
    if (reply.timed_out && reply.bytes.empty())
        return Error::make(ErrorKind::timeout, "rangefinder_timeout", "rangefinder did not respond");

    auto decoded = libs::rangefinder::decode_read_response(reply.bytes, address, start, count);
    if (!decoded) return decoded.error();
    return decoded.value();
}

}  // namespace libs::devices
