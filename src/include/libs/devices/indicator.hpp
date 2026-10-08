#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libs/error.hpp"
#include "libs/protocols/indicator.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

struct LampQuery {
    libs::indicator::QueryResponse response;
    Bytes request;
    Bytes raw_response;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
    bool timed_out = false;
};

struct LampWrite {
    Bytes write_request;
    Bytes write_response;
    bool write_echo_seen = false;
    std::string model;
    libs::indicator::Channel channel{};
    libs::indicator::State state{};
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
};

struct IndicatorApplyReport {
    std::vector<libs::indicator::WriteRequest> writes;
    std::vector<LampWrite> write_results;
    std::vector<LampQuery> queries;
};

// 状态灯设备控制器。协议要求相邻命令间隔大于 50 ms，控制器内部按 gap_ms
// 强制执行；业务层只传入灯色意图。
class Indicator {
public:
    explicit Indicator(transport::Link& link, std::uint8_t address = 1,
                       std::string model = {}, unsigned timeout_ms = 650,
                       unsigned gap_ms = 60)
        : link_(link), address_(address), model_(std::move(model)),
          timeout_ms_(timeout_ms), gap_ms_(gap_ms) {}

    Result<LampWrite> apply(libs::indicator::Channel channel,
                            libs::indicator::State state,
                            bool verify = false);
    Result<IndicatorApplyReport> applyAll(
        const std::map<libs::indicator::Channel, libs::indicator::State>& states,
        bool verify = false);
    Result<std::map<libs::indicator::Channel, libs::indicator::QueryResponse>> queryAll();
    Result<libs::indicator::ModelTraits> probeModel() const;

    const std::string& model() const noexcept { return model_; }
    void set_gap_ms(unsigned gap_ms) { gap_ms_ = gap_ms; }

private:
    Result<LampQuery> query(std::uint8_t address, libs::indicator::Channel channel);
    Result<LampWrite> write(std::uint8_t address,
                            const libs::indicator::WriteRequest& request);
    void enforce_gap();

    transport::Link& link_;
    std::uint8_t address_;
    std::string model_;
    unsigned timeout_ms_;
    unsigned gap_ms_;
    std::chrono::steady_clock::time_point last_io_{};
};

}  // namespace libs::devices
