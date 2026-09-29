#pragma once

#include <cstdint>
#include <string>

#include "libs/error.hpp"
#include "libs/protocols/indicator.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

// 状态灯驱动。
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
    bool write_echo_seen = false;      // 转换器回送的写帧，不等于设备已执行
    std::string model;
    libs::indicator::Channel channel{};
    libs::indicator::State state{};
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
};

class Indicator {
public:
    // 协议要求相邻命令间隔大于 50 ms，驱动器默认按 60 ms 等待。
    explicit Indicator(transport::Link& link, unsigned timeout_ms = 650, unsigned gap_ms = 60)
        : link_(link), timeout_ms_(timeout_ms), gap_ms_(gap_ms) {}

    Result<LampQuery> query(std::uint8_t address, libs::indicator::Channel channel);

    // 发一帧写命令，并把命令本身记进结果。是否随后查询确认由调用方决定。
    Result<LampWrite> write(std::uint8_t address, const libs::indicator::WriteRequest& request);

    void set_gap_ms(unsigned gap_ms) { gap_ms_ = gap_ms; }

private:
    transport::Link& link_;
    unsigned timeout_ms_;
    unsigned gap_ms_;
};

}  // namespace libs::devices
