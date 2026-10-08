#pragma once

#include <cstdint>

#include "libs/error.hpp"
#include "libs/protocols/rangefinder.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

// 测距仪设备控制器：只读原始寄存器。mm/cm 换算、超量程和有矿判定属于业务。
class Rangefinder {
public:
    explicit Rangefinder(transport::Link& link, std::uint8_t address = 1,
                         unsigned timeout_ms = 650)
        : link_(link), address_(address), timeout_ms_(timeout_ms) {}

    Result<libs::rangefinder::Reading> readRawRegisters(std::uint16_t start,
                                                        std::uint16_t count);
    Result<libs::rangefinder::Reading> observeOnce();

private:
    Result<libs::rangefinder::Reading> read(std::uint8_t address,
                                            std::uint16_t start,
                                            std::uint16_t count);

    transport::Link& link_;
    std::uint8_t address_;
    unsigned timeout_ms_;
};

}  // namespace libs::devices
