#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/laser.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

// 风冷激光器驱动。
//
// 职责：把"读某个寄存器""写一条指令"变成一次串口事务，并给出结构化结果。
// 不职责：不判断设备是否允许出光、不代替硬件联锁、不做工程量标定。
// 那类决策属于上层业务模块，本驱动只提供事实。

struct LaserTransaction {
    libs::laser::Frame frame;      // 解码成功时才有意义
    Bytes request;
    Bytes response;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
    std::uint64_t last_receive_tick = 0;
    bool timed_out = false;
};

class Laser {
public:
    // link 必须已打开。驱动不拥有它。
    explicit Laser(transport::Link& link, unsigned timeout_ms = 650)
        : link_(link), timeout_ms_(timeout_ms) {}

    // 读 B1（设置）/ B2（实际状态）/ BC（TEC）。
    Result<LaserTransaction> read(std::uint8_t command);

    // 裸写一帧。是否该写、写什么，完全由调用方决定。
    // 本方法不做任何策略判断，也不阻止任何指令码。
    Result<LaserTransaction> write(std::uint8_t command, const Bytes& payload = {});

    // 只发不等的写（用于 0x21..0x24 这类不需要回读的状态位）。
    // 仍然返回回包，用于留证。
    Result<LaserTransaction> send(std::uint8_t command, std::uint8_t value);

private:
    transport::Link& link_;
    unsigned timeout_ms_;
};

}  // namespace libs::devices
