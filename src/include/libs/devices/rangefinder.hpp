#pragma once

#include <cstdint>
#include <string>

#include "libs/error.hpp"
#include "libs/protocols/rangefinder.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

// 测距仪驱动：一次功能码 0x03 读寄存器。
struct RangefinderRead {
    libs::rangefinder::Reading reading;
    Bytes request;
    Bytes response;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
    bool timed_out = false;
};

class Rangefinder {
public:
    explicit Rangefinder(transport::Link& link, unsigned timeout_ms = 650)
        : link_(link), timeout_ms_(timeout_ms) {}

    // 只读。不提供写寄存器的入口——本驱动只覆盖已确认的读流程。
    Result<RangefinderRead> read(std::uint8_t address, std::uint16_t start, std::uint16_t count);

private:
    transport::Link& link_;
    unsigned timeout_ms_;
};

}  // namespace libs::devices
