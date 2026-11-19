#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "libs/error.hpp"
#include "libs/protocols/rangefinder.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

struct RangeObservation {
    // 设备响应成功但值无效时为空。通信失败由 Result 表达。
    std::optional<std::uint16_t> distance_mm;
    std::optional<std::uint8_t> confidence_percent;
    std::uint16_t sample_counter = 0;
    std::chrono::steady_clock::time_point observed_at;
};

// 测距仪提供观测事实，有料和可测窗口由业务层判断。
class Rangefinder {
public:
    explicit Rangefinder(transport::Link& link, std::uint8_t address = 1,
                         unsigned timeout_ms = 650)
        : link_(link), address_(address), timeout_ms_(timeout_ms) {}

    Result<libs::rangefinder::Reading> readRawRegisters(std::uint16_t start,
                                                        std::uint16_t count);
    Result<libs::rangefinder::Reading> observeOnce();
    Result<RangeObservation> readObservation();

private:
    Result<libs::rangefinder::Reading> read(std::uint8_t address,
                                            std::uint16_t start,
                                            std::uint16_t count);

    transport::Link& link_;
    std::uint8_t address_;
    unsigned timeout_ms_;
};

}  // namespace libs::devices
