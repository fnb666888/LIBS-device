// 设备动作测试：FakeLink 下断言命令顺序、读回确认和禁区命令不出现。
#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "libs/devices/air_conditioner.hpp"
#include "libs/devices/indicator.hpp"
#include "libs/devices/laser.hpp"
#include "libs/devices/rangefinder.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/laser.hpp"
#include "libs/protocols/modbus_rtu.hpp"

using libs::Bytes;

namespace {

class FakeLink final : public libs::transport::Link {
public:
    std::vector<Bytes> sent;
    std::deque<libs::Result<libs::transport::Reply>> replies;

    libs::Result<libs::transport::Reply> exchange(const Bytes& request,
                                                  unsigned timeout_ms = 650,
                                                  unsigned idle_ms = 80) override {
        (void)timeout_ms;
        (void)idle_ms;
        sent.push_back(request);
        const bool is_read =
            (request.size() >= 5 && request[0] == 0x5A && request[1] == 0xA5 &&
             (request[4] == libs::laser::command::kLdSettings ||
              request[4] == libs::laser::command::kLdActual ||
              request[4] == libs::laser::command::kTec)) ||
            (request.size() >= 2 && (request[1] == 0x01 || request[1] == 0x03));
        if (is_read && !replies.empty()) {
            auto reply = replies.front();
            replies.pop_front();
            return reply;
        }
        libs::transport::Reply echo;
        echo.bytes = request;
        echo.elapsed_ms = 1;
        return echo;
    }

    std::string endpoint() const override { return "FAKE-ACTION"; }

    void queue(const Bytes& bytes) {
        libs::transport::Reply reply;
        reply.bytes = bytes;
        reply.elapsed_ms = 1;
        replies.push_back(reply);
    }
};

Bytes laser_b2(std::uint32_t counter, bool discharging, bool q_gate_open,
               std::uint8_t fault_bits = 0) {
    Bytes payload(22, 0);
    payload[0] = discharging ? 0x01 : 0x00;
    payload[2] = fault_bits;
    payload[13] = static_cast<std::uint8_t>((counter >> 24) & 0xFF);
    payload[14] = static_cast<std::uint8_t>((counter >> 16) & 0xFF);
    payload[15] = static_cast<std::uint8_t>((counter >> 8) & 0xFF);
    payload[16] = static_cast<std::uint8_t>(counter & 0xFF);
    payload[19] = q_gate_open ? 0x01 : 0x00;
    payload[20] = discharging ? 0x01 : 0x00;
    return libs::laser::make_frame(libs::laser::command::kLdActual, payload);
}

std::vector<std::uint8_t> laser_write_commands(const FakeLink& link) {
    std::vector<std::uint8_t> out;
    for (const auto& frame : link.sent) {
        if (frame.size() < 5 || frame[0] != 0x5A || frame[1] != 0xA5) continue;
        const auto command = frame[4];
        if (command == libs::laser::command::kLdSettings ||
            command == libs::laser::command::kLdActual ||
            command == libs::laser::command::kTec) {
            continue;
        }
        out.push_back(command);
    }
    return out;
}

Bytes modbus_reply(std::uint8_t address, std::uint8_t function,
                   const Bytes& payload) {
    Bytes frame{address, function, static_cast<std::uint8_t>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    const auto crc = libs::modbus::crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    return frame;
}

}  // namespace

TEST_MAIN_BEGIN()

{
    CASE(laser_prepare_open_stop_uses_the_verified_continuous_sequence);
    FakeLink link;
    link.queue(laser_b2(100, false, false));
    link.queue(laser_b2(100, false, false));
    link.queue(laser_b2(100, true, false));
    link.queue(laser_b2(103, false, false));

    libs::devices::Laser laser(link);
    libs::devices::LaserOperationTrace trace;
    libs::devices::LaserSessionSettings settings;
    settings.discharge_current_a = 80;
    settings.pulse_width_us = 200;
    settings.frequency_hz = 1;
    settings.q_delay_us = 210;

    CHECK(laser.prepareContinuous(settings, 600, trace, 1));
    CHECK(laser.openQGate(trace));
    CHECK(laser.running());
    CHECK(laser.stopContinuous(trace));
    CHECK(!laser.running());
    CHECK_EQ(trace.counter_before, std::uint32_t(100));
    CHECK_EQ(trace.counter_after, std::uint32_t(103));
    CHECK_EQ(trace.discharges, std::uint32_t(3));

    const auto commands = laser_write_commands(link);
    const std::vector<std::uint8_t> expected = {
        0x11, 0x12, 0x13, 0x14, 0x16, 0x19, 0x21, 0x24, 0x24, 0x21,
    };
    CHECK_EQ(commands.size(), expected.size());
    if (commands.size() == expected.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            CHECK_EQ(static_cast<int>(commands[i]), static_cast<int>(expected[i]));
    }
    for (const auto command : {0x15, 0x17, 0x18, 0x22, 0x23, 0x31, 0x34}) {
        CHECK(std::find(commands.begin(), commands.end(), command) == commands.end());
    }
}

{
    CASE(laser_fire_once_returns_only_after_stop_is_confirmed);
    FakeLink link;
    link.queue(laser_b2(7, false, false));
    link.queue(laser_b2(7, true, false));
    link.queue(laser_b2(8, false, false));

    libs::devices::Laser laser(link);
    libs::devices::LaserOperationTrace trace;
    libs::devices::LaserSessionSettings settings;
    settings.discharge_current_a = 80;
    settings.pulse_width_us = 200;
    settings.frequency_hz = 1;
    settings.q_delay_us = 210;

    CHECK(laser.fireOnce(settings, 100, 1, trace, 1));
    CHECK_EQ(trace.discharges, std::uint32_t(1));
    CHECK(trace.has_after);
    if (trace.has_after) {
        CHECK_EQ(static_cast<int>(trace.after.q_gate), 0);
        CHECK_EQ(static_cast<int>(trace.after.work_state), 0);
    }
}

{
    CASE(laser_reset_and_clear_are_length_four_no_payload_commands);
    FakeLink link;
    libs::devices::Laser laser(link);
    libs::devices::LaserOperationTrace trace;
    CHECK(laser.resetFault(&trace));
    CHECK(laser.clearCounter(&trace));
    CHECK_EQ(link.sent.size(), std::size_t(2));
    if (link.sent.size() == 2) {
        CHECK_EQ(static_cast<int>(link.sent[0][2]), 0x04);
        CHECK_EQ(static_cast<int>(link.sent[0][4]), 0x33);
        CHECK_EQ(static_cast<int>(link.sent[1][2]), 0x04);
        CHECK_EQ(static_cast<int>(link.sent[1][4]), 0x32);
    }
}

{
    CASE(air_conditioner_keepalive_owns_the_100_second_device_behavior);
    FakeLink link;
    libs::devices::AirConditioner device(link, 8);
    device.holdMode(libs::air_conditioner::Mode::cooling);
    CHECK(device.keepalive(1000, 60000));
    CHECK_EQ(link.sent.size(), std::size_t(1));
    const auto too_soon = device.keepalive(1001, 60000);
    CHECK(too_soon);
    if (too_soon) CHECK(!too_soon.value());
    CHECK(device.keepalive(61001, 60000));
    CHECK_EQ(link.sent.size(), std::size_t(2));
    const auto too_slow = device.keepalive(122002, 100000);
    CHECK(!too_slow);
    if (!too_slow) CHECK_EQ(too_slow.error().code, std::string("ac_keepalive_too_slow"));
}

{
    CASE(indicator_apply_all_can_verify_each_state);
    FakeLink link;
    Bytes red_reply{0x01, 0x01, 0x00, 0x01, 0x00, 0x00};
    const auto red_crc = libs::modbus::crc16(red_reply.data(), red_reply.size());
    red_reply.push_back(static_cast<std::uint8_t>(red_crc & 0xFF));
    red_reply.push_back(static_cast<std::uint8_t>(red_crc >> 8));
    Bytes green_reply{0x01, 0x01, 0x00, 0x02, 0x00, 0xFF};
    const auto green_crc = libs::modbus::crc16(green_reply.data(), green_reply.size());
    green_reply.push_back(static_cast<std::uint8_t>(green_crc & 0xFF));
    green_reply.push_back(static_cast<std::uint8_t>(green_crc >> 8));
    link.queue(red_reply);
    link.queue(green_reply);

    libs::devices::Indicator device(link, 1, "TL50-485", 650, 0);
    std::map<libs::indicator::Channel, libs::indicator::State> states;
    states[libs::indicator::Channel::red] = libs::indicator::State::off;
    states[libs::indicator::Channel::green] = libs::indicator::State::on;
    const auto report = device.applyAll(states, true);
    CHECK(report);
    if (report) {
        CHECK_EQ(report.value().writes.size(), std::size_t(2));
        CHECK_EQ(report.value().queries.size(), std::size_t(2));
    }
}

{
    CASE(rangefinder_observe_once_uses_the_documented_window);
    FakeLink link;
    Bytes values;
    for (std::uint16_t value : {10, 5000, 1234, 88, 99, 0, 25, 1, 7}) {
        values.push_back(static_cast<std::uint8_t>(value >> 8));
        values.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }
    link.queue(modbus_reply(1, 0x03, values));
    libs::devices::Rangefinder device(link, 1);
    const auto reading = device.observeOnce();
    CHECK(reading);
    if (reading) CHECK_EQ(reading.value().registers.size(), std::size_t(9));
    CHECK_EQ(link.sent.size(), std::size_t(1));
}

TEST_MAIN_RETURN()
