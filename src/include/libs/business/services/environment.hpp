#pragma once

// 环境采集与联锁（能力 9）。
//
// 判定依据：范围报告 §3.9
//   3.9.1 空调状态采集 [A]
//   3.9.2 空调与检测的联锁阈值 [C]  -> 见 ac_interlock.hpp
//
// **本文件最重要的纪律：未知或通信失败绝不能表达为"正常"**（任务书 §6.1）。
// 这必须在类型层面强制——`AcSnapshot::availability` 是三态枚举：
//   available / stale（上次读数还在但已过期） / unavailable（读不到）。
// 没有"正常/异常"那种两态结构。范围报告 §3.9.1 点名了这条要求。
//
// 另外一个设备行为（手册 §6.2）：模式控制必须持续下发，超 100 s 收不到
// 自动切回自动运行。驱动只发一次，"保持循环"属于业务层，放在本类里。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"
#include "libs/devices/air_conditioner.hpp"
#include "libs/protocols/air_conditioner.hpp"

namespace business::services {

// 空调数据的可用性。**三态，不是两态。**
enum class AcAvailability {
    available,      // 本次读成功
    stale,          // 读失败但有上次读数，且还在 stale_after_ms 内
    unavailable,    // 完全没有可用读数
};
const char* to_string(AcAvailability availability) noexcept;

// 一次空调状态快照。**通信失败时也构造这个结构**——
// 状态是 unavailable，read_* 字段留空，而不是把上次值当本次值。
struct AcSnapshot {
    AcAvailability availability = AcAvailability::unavailable;
    Milliseconds wall_clock_ms = 0;
    std::string read_error;              // availability != available 时的原因

    // 工程量（已按手册变比换算）。仅 availability == available 时有意义。
    std::optional<double> indoor_temp_c;
    std::optional<double> evaporator_temp_c;
    std::optional<double> condenser_temp_c;
    std::optional<double> humidity_rh;

    // 模式。仅 available 时有意义。
    std::optional<libs::air_conditioner::Mode> mode;

    // 状态位与告警位。**原始位列表，不做"正常/异常"归并**。
    std::vector<libs::air_conditioner::StatusBit> bits;
    std::vector<libs::air_conditioner::StatusBit> active_alarms;   // is_alarm && active

    // 手动读到的参数，用于确认模式是否仍是自己设的那个。
    bool params_read = false;
};

json::Value to_json(const AcSnapshot& snapshot);

// 空调状态采集器。负责：
//   1. 轮询状态 + 参数；
//   2. 维护"上次读数 + 过期时间"，把读失败表达为 stale/unavailable；
//   3. 模式保持循环（超 keepalive_ms 未下发则重发）。
//
// **它不做联锁判定**——那是 3.9.2（C 类），在 ac_interlock.hpp。
// 子项不得互相代偿（范围报告 §7 纪律 4）：采集器"看着一切正常"不代表联锁通过。
class AirConditionerMonitor {
public:
    // stale_after_ms：读失败后多久算"过期但还能看"。默认 30 s。
    // keepalive_ms：模式保持循环的间隔，必须**显著小于 100 s**。
    // 默认 60 s——留足余量，避免设备因超时回落到自动运行。
    AirConditionerMonitor(std::uint8_t address, unsigned stale_after_ms = 30000,
                          unsigned keepalive_ms = 60000);

    // 手动保持模式：业务层声明"我们要它保持在这个模式"。
    // 之后 keepalive() 会在超时前重发。**只表达意图，不判断该不该启停压缩机**。
    void hold_mode(libs::air_conditioner::Mode mode);

    // 读一次状态+参数。**从不抛异常，也从不用旧值冒充本次值。**
    AcSnapshot poll(libs::devices::AirConditioner* device);

    // 模式保持循环：距上次下发超过 keepalive_ms 就重发。
    // 返回"是否发生了重发"，以及重发是否成功。device 为空时不重发。
    libs::Result<bool> keepalive(libs::devices::AirConditioner* device);

    // 上一次成功读到的快照。**调用方必须自己看 availability**——
    // 这个函数返回它只是为了让 UI 能"最后已知值"，
    // 不保证它新鲜。
    const AcSnapshot& last_known() const noexcept { return last_; }

    // 本次保持的目标模式；nullopt = 未声明保持。
    const std::optional<libs::air_conditioner::Mode>& held_mode() const noexcept { return held_; }

    Milliseconds last_mode_sent_at_ms() const noexcept { return last_mode_sent_; }

    // 手册给的上限，用于自检与日志。
    static constexpr unsigned kDeviceKeepaliveLimitMs = 100000;   // 手册 §6.2：100 s

private:
    std::uint8_t address_;
    unsigned stale_after_ms_;
    unsigned keepalive_ms_;
    AcSnapshot last_;
    Milliseconds last_good_read_ms_ = 0;
    std::optional<libs::air_conditioner::Mode> held_;
    Milliseconds last_mode_sent_ = 0;
};

}  // namespace business::services


