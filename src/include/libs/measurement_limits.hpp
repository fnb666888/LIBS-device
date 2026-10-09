#pragma once

#include <cstdint>
#include <string_view>

namespace libs::measurement_limits {

// 纯参数元数据：UI、业务和驱动共用，不引入设备操作或业务依赖。
// 厂家量程来自 laser/air_cooled_laser_rs232_protocol.txt §3.1。
// SDK 通用积分范围为 0.002~600000 ms，具体探测器下限由 SDK 再校验；
// 本项目保留更小的 60000 ms 软件上限。未公开量程的字段明确标为软件限制。
struct Range {
    std::string_view group;
    std::string_view field;
    double lo;
    double hi;
    bool integer;
    const char* unit;
    const char* source;

    constexpr bool contains(double value) const {
        // 比较先拒绝 NaN/Inf 和越界；之后的整数转换保证在表中有限范围内。
        return value >= lo && value <= hi &&
               (!integer || value == static_cast<std::uint64_t>(value));
    }
};

inline constexpr Range kRanges[] = {
    {"laser", "discharge_current_a", 15, 100, true, "A", "激光协议 §3.1"},
    {"laser", "pulse_width_us", 50, 300, true, "us", "激光协议 §3.1"},
    {"laser", "frequency_hz", 1, 10, true, "Hz", "激光协议 §3.1"},
    {"laser", "discharge_delay_us", 0, 500, true, "us", "激光协议 §3.1"},
    {"laser", "q_delay_us", 2, 500, true, "us", "激光协议 §3.1"},
    {"laser", "q_warmup_s", 0, 3600, true, "s", "软件限制，厂家协议未标量程"},
    {"laser", "settle_timeout_ms", 1, 600000, true, "ms", "软件等待上限"},
    {"laser", "emission_hold_ms", 0, 600000, true, "ms", "软件会话上限"},
    {"laser", "min_discharges", 1, 100000, true, "次", "软件判据上限"},
    {"spectrometer", "integration_ms", 0.002, 60000, false, "ms", "SDK 通用下限与软件上限；探测器另由 SDK 校验"},
    {"spectrometer", "capture_timeout_ms", 1, 600000, true, "ms", "软件采集等待上限"},
    {"spectrometer", "integration_delay_ns", 0, 1000000, false, "ns", "软件延时上限，SDK 使用 FPGA 计数"},
};

constexpr const Range* find(std::string_view group, std::string_view field) {
    for (const auto& range : kRanges)
        if (range.group == group && range.field == field) return &range;
    return nullptr;
}

constexpr bool accepts(std::string_view group, std::string_view field, double value) {
    const auto* range = find(group, field);
    return range && range->contains(value);
}

}  // namespace libs::measurement_limits
