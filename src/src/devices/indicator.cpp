#include "libs/devices/indicator.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

#include "libs/protocols/modbus_rtu.hpp"

namespace libs::devices {

void Indicator::enforce_gap() {
    if (gap_ms_ == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (last_io_ != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_io_).count();
        if (elapsed < static_cast<long long>(gap_ms_))
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(gap_ms_) - elapsed));
    }
    last_io_ = std::chrono::steady_clock::now();
}

Result<LampWrite> Indicator::apply(libs::indicator::Channel channel,
                                   libs::indicator::State state,
                                   bool verify) {
    const libs::indicator::WriteRequest request{model_, channel, state};
    const auto written = write(address_, request);
    if (!written) return written.error();
    if (!verify) return written;

    const auto queried = query(address_, channel);
    if (!queried) return queried.error();
    if (queried.value().response.state != state)
        return Error::make(ErrorKind::device, "indicator_state_mismatch",
                           "状态灯写后读回的状态与请求不一致");
    return written;
}

Result<IndicatorApplyReport> Indicator::applyAll(
        const std::map<libs::indicator::Channel, libs::indicator::State>& states,
        bool verify) {
    IndicatorApplyReport report;
    const auto model = probeModel();
    if (!model) return model.error();
    const auto supported = [&](libs::indicator::Channel channel) {
        using Ch = libs::indicator::Channel;
        if (channel == Ch::blue || channel == Ch::white) return model.value().has_blue_white;
        if (channel == Ch::buzzer) return !model.value().rgb_only;
        return true;
    };
    // Validate the entire intent before changing the hardware. Off on a
    // nonexistent channel is satisfied by the all-off reset, never a write.
    for (const auto& item : states) {
        if (item.second == libs::indicator::State::off) continue;
        const libs::indicator::WriteRequest request{model_, item.first, item.second};
        const auto frame = libs::indicator::make_write_request(address_, request);
        if (!frame) return frame.error();
        report.writes.push_back(request);
    }
    if (!libs::modbus::is_unicast_address(address_))
        return Error::make(ErrorKind::invalid_argument, "bad_address", "Modbus address must be 1..247");
    report.reset_request = libs::modbus::make_request(address_, 0x05, 0, 0);
    enforce_gap();
    const auto reset = link_.exchange(report.reset_request, timeout_ms_);
    if (!reset) return reset.error();
    report.reset_response = reset.value().bytes;
    if (reset.value().receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial error while clearing the lamp", report.reset_response);
    report.reset_echo_seen = std::search(report.reset_response.begin(), report.reset_response.end(),
        report.reset_request.begin(), report.reset_request.end()) != report.reset_response.end();
    for (const auto& request : report.writes) {
        const auto written = write(address_, request);
        if (!written) return written.error();
        report.write_results.push_back(written.value());
    }
    if (verify) {
        for (const auto channel : {libs::indicator::Channel::red, libs::indicator::Channel::yellow,
                                  libs::indicator::Channel::green, libs::indicator::Channel::blue,
                                  libs::indicator::Channel::white, libs::indicator::Channel::buzzer}) {
            if (!supported(channel)) continue;
            const auto item = states.find(channel);
            const auto expected = item == states.end() ? libs::indicator::State::off : item->second;
            const auto queried = query(address_, channel);
            if (!queried) return queried.error();
            if (queried.value().response.state != expected)
                return Error::make(ErrorKind::device, "indicator_state_mismatch",
                                   "状态灯写后读回的状态与请求不一致");
            report.queries.push_back(queried.value());
        }
    }
    return report;
}

Result<std::map<libs::indicator::Channel, libs::indicator::QueryResponse>>
Indicator::queryAll() {
    const auto model = probeModel();
    if (!model) return model.error();
    static const libs::indicator::Channel channels[] = {
        libs::indicator::Channel::red,
        libs::indicator::Channel::yellow,
        libs::indicator::Channel::green,
        libs::indicator::Channel::blue,
        libs::indicator::Channel::white,
        libs::indicator::Channel::buzzer,
    };
    std::map<libs::indicator::Channel, libs::indicator::QueryResponse> out;
    for (const auto channel : channels) {
        if ((channel == libs::indicator::Channel::blue || channel == libs::indicator::Channel::white)
            && !model.value().has_blue_white) continue;
        if (channel == libs::indicator::Channel::buzzer && model.value().rgb_only) continue;
        const auto queried = query(address_, channel);
        if (!queried) return queried.error();
        out.emplace(channel, queried.value().response);
    }
    return out;
}

Result<libs::indicator::ModelTraits> Indicator::probeModel() const {
    libs::indicator::ModelTraits traits;
    if (!libs::indicator::lookup_model(model_, traits))
        return Error::make(ErrorKind::unsupported, "unknown_indicator_model",
                           "未登记的状态灯型号：" + model_);
    return traits;
}

Result<LampQuery> Indicator::query(std::uint8_t address, libs::indicator::Channel channel) {
    Bytes request;
    try {
        request = libs::indicator::make_query_request(address, channel);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "bad_request", e.what());
    }

    enforce_gap();
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

    enforce_gap();
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
