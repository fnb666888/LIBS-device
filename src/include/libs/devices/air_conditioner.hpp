#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/protocols/air_conditioner.hpp"
#include "libs/transport/serial.hpp"

namespace libs::devices {

// 空调驱动（协议 V2，Modbus RTU 变体）。
//
// 只做"把一次 Modbus 事务跑完并解码"。不判断当前温度是否该停机、不做超温保护
// 策略、不替上层决定何时启停压缩机——那是业务模块的事。
// 需要注意的设备行为（手册 §6.2 备注）：模式控制必须持续下发，连续 100 s
// 收不到就自动切回自动运行。本驱动不实现"保持"，需要时由调用方周期调用 set_mode。

struct StatusRead {
    libs::air_conditioner::StatusReading reading;
    Bytes request;
    Bytes response;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
    bool timed_out = false;
};

struct ParamRead {
    libs::air_conditioner::ParameterReading reading;
    Bytes request;
    Bytes response;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;
    bool timed_out = false;
};

struct ControlWrite {
    libs::air_conditioner::ControlResponse response;
    Bytes request;
    Bytes raw;
    unsigned elapsed_ms = 0;
};

struct ParamWrite {
    libs::air_conditioner::ParamWriteResponse response;
    Bytes request;
    Bytes raw;
    unsigned elapsed_ms = 0;
};

// 一次轮询的两段结果。任一段失败都保留另一段，不使用旧值冒充本次值。
struct AirConditionerPoll {
    bool status_ok = false;
    bool params_ok = false;
    std::string status_error;
    std::string params_error;
    libs::air_conditioner::StatusReading status;
    libs::air_conditioner::ParameterReading params;
};

class AirConditioner {
public:
    // link 必须已打开，驱动不拥有它。timeout_ms 沿用其他设备驱动的默认值。
    explicit AirConditioner(transport::Link& link, std::uint8_t address = 1,
                            unsigned timeout_ms = 650)
        : link_(link), address_(address), timeout_ms_(timeout_ms) {}

    Result<libs::air_conditioner::StatusReading> readStatus();
    Result<libs::air_conditioner::StatusReading> readStatus(std::uint16_t start,
                                                            std::uint16_t count);
    Result<libs::air_conditioner::ParameterReading> readParams(
        std::uint16_t start = libs::air_conditioner::reg::kIndoorTemp,
        std::uint16_t count = 26);
    Result<libs::air_conditioner::ParamRegister> readParam(std::uint16_t reg);
    Result<AirConditionerPoll> poll();

    Result<ParamWrite> setMode(libs::air_conditioner::Mode mode);
    Result<ControlWrite> setPower(bool on);
    Result<ParamWrite> setSetpoint(std::uint16_t reg, double value);
    Result<ParamWrite> writeParams(
        const std::vector<libs::air_conditioner::ParamWrite>& writes);
    Result<ParamWrite> writeParam(const libs::air_conditioner::ParamWrite& write);

    void holdMode(libs::air_conditioner::Mode mode);
    Result<bool> keepalive(std::uint64_t now_ms, unsigned keepalive_ms);
    const std::optional<libs::air_conditioner::Mode>& heldMode() const noexcept {
        return held_mode_;
    }
    std::uint64_t lastModeSentMs() const noexcept { return last_mode_sent_ms_; }

    static std::vector<libs::air_conditioner::StatusBit> activeAlarms(
        const libs::air_conditioner::StatusReading& status);

    // FC01：读运行状态位（开关机、制冷/加热、风机、各类告警）。
    //
    // 默认区间是 10035..10058（24 位 = 3 字节），而不是从 10001 起：手册把
    // 10001-10034 定义为保留，读它们只会浪费位图空间。默认区间正好覆盖
    // 手册登记的全部命名状态位。要读整张表（10001..10058，8 字节）请显式传参。
    Result<StatusRead> read_status(std::uint8_t address,
                                   std::uint16_t start = libs::air_conditioner::reg::kCoolingOn,
                                   std::uint16_t count = 24);

    // FC03：读运行参数（柜内/蒸发器/冷凝器温度、湿度、各设定点、模式、波特率…）。
    Result<ParamRead> read_params(std::uint8_t address,
                                  std::uint16_t start = libs::air_conditioner::reg::kIndoorTemp,
                                  // 26 而不是 25：30026「运行模式」正好差一个落在窗口外，
                                  // 于是界面上模式永远空白。读到模式为止，不算多余往返
                                  // （协议允许单次最多 50 个寄存器）。
                                  std::uint16_t count = 26);

    // FC05：写开关机状态位（10042）。
    Result<ControlWrite> set_power(std::uint8_t address, bool on);

    // FC06：写运行参数。范围越界由协议层的 ParamWrite::range_min/max 判定，
    // 一个字节都不会发出去。设备地址/波特率（30028/30027）也走这里，
    // 但改完会立刻改变本链路参数——调用方要清楚自己在做什么。
    Result<ParamWrite> write_param(std::uint8_t address,
                                   const libs::air_conditioner::ParamWrite& write);

    // 便捷封装：按手册变比把工程量写成 raw 再下发。超范围返回 invalid_argument。
    Result<ParamWrite> set_setpoint(std::uint8_t address, std::uint16_t reg, double value);

    // 便捷封装：FC06 写模式（30026）。手册要求持续下发，本方法只发一次。
    Result<ParamWrite> set_mode(std::uint8_t address, libs::air_conditioner::Mode mode);

private:
    transport::Link& link_;
    std::uint8_t address_;
    unsigned timeout_ms_;
    std::optional<libs::air_conditioner::Mode> held_mode_;
    std::uint64_t last_mode_sent_ms_ = 0;
};

}  // namespace libs::devices
