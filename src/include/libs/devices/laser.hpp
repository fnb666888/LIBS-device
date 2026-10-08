#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

// 会话参数。只含每次检测允许改变的量；整机标定 0x15/0x17/0x18 不在其中。
struct LaserSessionSettings {
    unsigned discharge_current_a = 0;
    unsigned pulse_width_us = 0;
    unsigned frequency_hz = 0;
    unsigned discharge_delay_us = 0;
    unsigned q_delay_us = 0;
    unsigned q_warmup_s = 0;
};

// 0xB2 的结构化事实。字段保留设备原始极性，不做门禁换算：
// q_gate=1 表示光闸开，fault_bits!=0 表示设备报故障。
struct LaserActual {
    bool valid = false;
    std::uint8_t work_state = 0;
    std::uint8_t clock_mode = 0;
    std::uint8_t fault_bits = 0;
    std::uint8_t interlock_1 = 0;
    std::uint8_t interlock_2 = 0;
    std::uint8_t emergency_stop = 0;
    std::uint8_t overtemperature = 0;
    std::uint8_t q_gate = 0;
    std::uint8_t work_enabled = 0;
    std::uint8_t discharge_clock_enabled = 0;
    std::uint16_t ld_temperature_raw = 0;
    std::uint16_t actual_current_raw = 0;
    std::uint16_t actual_voltage_raw = 0;
    std::uint16_t heatsink_temperature_raw = 0;
    std::uint32_t discharge_counter = 0;

};

struct LaserSettings {
    std::uint16_t discharge_current_raw = 0;
    std::uint16_t discharge_pulse_width_raw = 0;
    std::uint16_t discharge_frequency_raw = 0;
    std::uint16_t discharge_delay_raw = 0;
    std::uint16_t q_delay_raw = 0;
    std::uint16_t ld_temperature_limit_raw = 0;
    std::uint16_t voltage_limit_raw = 0;
    std::uint16_t q_divider_raw = 0;
    std::uint16_t q_warmup_raw = 0;
};

struct LaserTec {
    std::uint16_t max_temperature_raw = 0;
    std::uint16_t min_temperature_raw = 0;
    std::uint16_t b_coefficient_raw = 0;
    std::uint16_t thermistor_resistance_raw = 0;
    std::uint16_t stage_voltage_raw = 0;
    std::uint16_t tec_voltage_raw = 0;
    std::uint16_t kp_raw = 0;
    std::uint16_t ki_raw = 0;
    std::uint16_t kd_raw = 0;
    std::uint16_t actual_temperature_raw = 0;
    std::uint16_t work_intensity_raw = 0;
};

// 调用方持有的操作留证。设备控制器只追加逐帧请求，业务层决定日志/响应。
struct LaserOperationTrace {
    std::vector<std::string> sent_frames;
    std::string failed_step;
    unsigned settle_ms = 0;
    bool has_before = false;
    bool has_after = false;
    LaserActual before;
    LaserActual after;
    std::uint32_t counter_before = 0;
    std::uint32_t counter_after = 0;
    std::uint32_t discharges = 0;
};

// 风冷激光器设备控制器。
//
// 设备协议、单次事务、出光动作时序和读回确认都在这里；门禁、配置读取和
// 跨设备编排留在 business。
//
// 安全禁区：0x15/0x17/0x18 无公开写入口，0x31 不进入自动流程，
// 0x34 不作为出光路径，TEC 只读。
class Laser {
public:
    explicit Laser(transport::Link& link, unsigned timeout_ms = 650)
        : link_(link), timeout_ms_(timeout_ms) {}

    Result<LaserActual> readActual(LaserOperationTrace* trace = nullptr);
    Result<LaserSettings> readSettings(LaserOperationTrace* trace = nullptr);
    Result<LaserTec> readTec(LaserOperationTrace* trace = nullptr);

    VoidResult resetFault(LaserOperationTrace* trace = nullptr);
    VoidResult clearCounter(LaserOperationTrace* trace = nullptr);

    VoidResult prepareContinuous(const LaserSessionSettings& settings,
                                 unsigned settle_timeout_ms,
                                 LaserOperationTrace& trace,
                                 unsigned poll_interval_ms = 200);
    VoidResult openQGate(LaserOperationTrace& trace);
    VoidResult startContinuous(const LaserSessionSettings& settings,
                               unsigned settle_timeout_ms,
                               LaserOperationTrace& trace,
                               unsigned poll_interval_ms = 200);
    VoidResult stopContinuous(LaserOperationTrace& trace);
    VoidResult fireOnce(const LaserSessionSettings& settings,
                        unsigned settle_timeout_ms,
                        unsigned hold_ms,
                        LaserOperationTrace& trace,
                        unsigned poll_interval_ms = 200);

    // 只表示本对象的 openQGate 成功且尚未 stopContinuous；
    // 真实状态必须以 readActual() 为准。
    bool running() const noexcept { return running_; }

private:
    struct LaserTransaction {
        Bytes request;
        Bytes response;
        unsigned elapsed_ms = 0;
        unsigned receive_errors = 0;
        std::uint64_t last_receive_tick = 0;
        bool timed_out = false;
    };

    Result<LaserTransaction> readTransaction(std::uint8_t command);
    Result<LaserTransaction> writeTransaction(std::uint8_t command,
                                              const Bytes& payload = {});
    Result<LaserTransaction> sendTransaction(std::uint8_t command,
                                             std::uint8_t value);

    VoidResult writeSession(const LaserSessionSettings& settings,
                            LaserOperationTrace& trace);
    VoidResult writeChecked(std::uint8_t command,
                            const Bytes& payload,
                            const char* step,
                            LaserOperationTrace& trace);
    Result<LaserActual> appendReadActual(LaserOperationTrace* trace);

    transport::Link& link_;
    unsigned timeout_ms_;
    bool running_ = false;
};

}  // namespace libs::devices
