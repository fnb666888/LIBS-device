#pragma once

#include <cstdint>

#include "libs/error.hpp"
#include "libs/protocols/su300.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

struct Su300Transaction {
    Bytes request;
    Bytes response;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
    bool timed_out = false;
    bool echo_confirmed = false;  // FC06 response exactly echoed address/register/value
};

// SU300 变频器驱动。所有操作都是同步请求-响应，调用方必须在设备互斥锁内调用。
// 驱动不假定“写成功就已经到位”：启动/停止后应通过 read_status() 回读运行状态和故障码。
class Su300 {
public:
    explicit Su300(transport::Link& link, std::uint8_t address = 1, unsigned timeout_ms = 650,
                   unsigned idle_ms = 80)
        : link_(link), address_(address), timeout_ms_(timeout_ms), idle_ms_(idle_ms) {}

    Result<Su300Transaction> run(libs::su300::RunCommand command);
    Result<Su300Transaction> stop();
    Result<Su300Transaction> set_frequency_percent(std::int16_t percent_x100);
    Result<Su300Transaction> reset_fault();
    Result<libs::su300::Status> read_status();

private:
    Result<Su300Transaction> write(const Bytes& request, std::uint16_t reg, std::uint16_t value);

    transport::Link& link_;
    std::uint8_t address_;
    unsigned timeout_ms_;
    unsigned idle_ms_;
};

}  // namespace libs::devices
