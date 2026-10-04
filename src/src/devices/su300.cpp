#include "libs/devices/su300.hpp"

#include <array>
#include <exception>
#include <utility>

namespace libs::devices {
namespace {
using libs::su300::decode_read_response;
using libs::su300::decode_status;
using libs::su300::decode_write_response;
using libs::su300::make_read_request;
using libs::su300::run_command_value;
using libs::su300::reg::kControlCommand;
using libs::su300::reg::kExternalCommand;
using libs::su300::reg::kFrequencyCommand;
using libs::su300::command::kFaultReset;
}

Result<Su300Transaction> Su300::write(const Bytes& request, std::uint16_t reg, std::uint16_t value) {
    const auto exchanged = link_.exchange(request, timeout_ms_, idle_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();
    Su300Transaction result;
    result.request = request;
    result.response = reply.bytes;
    result.elapsed_ms = reply.elapsed_ms;
    result.receive_errors = reply.receive_errors;
    result.timed_out = reply.timed_out;
    if (reply.timed_out)
        return Error::with_raw(ErrorKind::transport, "su300_timeout",
                               "SU300 response timed out before a complete FC06 reply", reply.bytes);
    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "su300_uart_errors",
                               "SU300 serial parity/framing/overrun error", reply.bytes);
    const VoidResult decoded = decode_write_response(reply.bytes, request[0], reg, value);
    if (!decoded) return decoded.error();
    result.echo_confirmed = true;
    return result;
}

Result<Su300Transaction> Su300::run(libs::su300::RunCommand command) {
    Bytes request;
    try {
        request = libs::su300::make_run_command_request(address_, command);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "su300_bad_command", e.what());
    }
    return write(request, kControlCommand, run_command_value(command));
}

Result<Su300Transaction> Su300::stop() {
    return run(libs::su300::RunCommand::halt);
}

Result<Su300Transaction> Su300::set_frequency_percent(std::int16_t percent_x100) {
    Bytes request;
    try {
        request = libs::su300::make_frequency_request(address_, percent_x100);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "su300_bad_frequency", e.what());
    }
    return write(request, kFrequencyCommand,
                 static_cast<std::uint16_t>(percent_x100));
}

Result<Su300Transaction> Su300::reset_fault() {
    Bytes request;
    try {
        request = libs::su300::make_fault_reset_request(address_);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "su300_bad_reset", e.what());
    }
    return write(request, kExternalCommand, kFaultReset);
}

Result<libs::su300::Status> Su300::read_status() {
    // Reserved registers 2108H, 210BH-210CH and 2110H are deliberately not
    // included. Some firmware rejects a read crossing those holes, and filling
    // them with zero would turn unknown values into false status facts.
    constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 4> ranges{{
        {libs::su300::reg::kFault, 8},          // 2100H..2107H
        {libs::su300::reg::kCurrentCount, 2},   // 2109H..210AH
        {libs::su300::reg::kTemperature, 3},    // 210DH..210FH
        {libs::su300::reg::kPulseInput, 7},     // 2111H..2117H
    }};

    std::vector<libs::su300::RegisterBlock> blocks;
    std::vector<Bytes> raw_responses;
    blocks.reserve(ranges.size());
    raw_responses.reserve(ranges.size());
    for (const auto& [start, count] : ranges) {
        Bytes request;
        try {
            request = make_read_request(address_, start, count);
        } catch (const std::exception& e) {
            return Error::make(ErrorKind::invalid_argument, "su300_bad_status_request", e.what());
        }
        const auto exchanged = link_.exchange(request, timeout_ms_, idle_ms_);
        if (!exchanged) return exchanged.error();
        const auto& reply = exchanged.value();
        if (reply.timed_out)
            return Error::with_raw(ErrorKind::transport, "su300_timeout",
                                   "SU300 status response timed out before a complete FC03 reply",
                                   reply.bytes);
        if (reply.receive_errors)
            return Error::with_raw(ErrorKind::transport, "su300_uart_errors",
                                   "SU300 serial parity/framing/overrun error", reply.bytes);
        const auto block = decode_read_response(reply.bytes, address_, start, count);
        if (!block) return block.error();
        blocks.push_back(block.value());
        raw_responses.push_back(reply.bytes);
    }

    const auto decoded = decode_status(blocks);
    if (!decoded) return decoded.error();
    auto status = decoded.value();
    status.raw_blocks = std::move(blocks);
    status.raw_responses = std::move(raw_responses);
    return status;
}

}  // namespace libs::devices
