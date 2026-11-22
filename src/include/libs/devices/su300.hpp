#pragma once

#include "libs/error.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

enum class Su300Direction { forward, reverse, stopped, unknown };

struct Su300Status {
    Su300Direction direction = Su300Direction::unknown;
    std::uint16_t fault_code = 0;
    std::uint16_t reference_centihz = 0;
    std::uint16_t output_centihz = 0;
    std::uint16_t inputs = 0;
    std::uint16_t run_word = 0;
    std::uint16_t flags_word = 0;
    std::uint16_t direction_word = 0;
    bool communication_control = false;
    bool jogging = false;
    bool jog_direction_from_run_word = false;

    bool stopped() const { return direction == Su300Direction::stopped && output_centihz == 0; }
    bool confirms_jog(Su300Direction requested) const {
        return direction == requested && jogging && output_centihz != 0;
    }
};

struct Su300JogProfile {
    double forward_hz = 0;
    double reverse_hz = 0;
    unsigned deceleration_ms = 0;
    unsigned communication_timeout_ms = 0;
    std::uint16_t communication_loss_word = 0;
};

// SU300 manual chapter 4. The caller serializes transactions.
class Su300 {
public:
    Su300(transport::Link& link, std::uint8_t address, unsigned timeout_ms)
        : link_(link), address_(address), timeout_ms_(timeout_ms) {}

    Result<Su300Status> read_status();
    Result<Su300JogProfile> read_jog_profile();
    VoidResult set_jog_frequency(unsigned tenths_hz);
    Result<std::uint16_t> read_stop_watchdog();
    VoidResult set_communication_watchdog(std::uint16_t deciseconds);
    VoidResult jog(Su300Direction direction);
    VoidResult stop();

private:
    Result<Bytes> transaction(std::uint8_t function, std::uint16_t reg, std::uint16_t value);
    VoidResult send_control(std::uint16_t value);
    transport::Link& link_;
    std::uint8_t address_;
    unsigned timeout_ms_;
};

}  // namespace libs::devices
