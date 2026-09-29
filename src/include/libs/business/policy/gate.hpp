#pragma once

// 门禁、联锁与状态指示（能力 4）。
//
// 判定依据：范围报告 §3.4 = 3.4.1 待机就绪门禁 [A] + 3.4.2 出光门禁与联锁判读 [A]
//         + 3.4.3 状态灯业务状态映射 [A]。三项全部 A 类。
//
// **这是全部 25 个子项里安全价值最高的一项，越早落地越安全。**
//
// 关键纪律（范围报告 §3.4.2 逐字保留）：
//   驱动层"照常解码上报但绝不参与门禁"，判断权全部在业务层。
//   README 明确把"不判断现在允不允许激光出光"留给上层，本类就是那个上层。
//
// 合并后新增的纪律（范围报告 §3.4.3）：
//   **指示层是门禁状态之上的纯函数，不得反向参与门禁判定。**
//   判定的唯一入口是 Gate::evaluate()。
//
// **空调联锁（3.9.2，C 类）不在本文件**：阈值未定，见 interlock.hpp。

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "libs/business/json/json.hpp"
#include "libs/error.hpp"
#include "libs/protocols/indicator.hpp"
#include "libs/protocols/laser.hpp"

namespace business::policy {

// 业务状态。状态灯与门禁共用这一套枚举——但只有 Gate 产出它。
enum class MachineState {
    offline,      // 未上电 / 未配置
    standby,      // 待机，参数未齐备
    ready,        // 就绪，参数齐备且无联锁
    acquiring,    // 采集中
    fault,        // 异常
    recovering,   // 恢复中
};
const char* to_string(MachineState state) noexcept;

// 激光器已解码的状态位（范围报告 §3.4.2 依据 laser.cpp:165-175）。
// 这里只承载**事实**，不含任何判断。
struct LaserSafetyFacts {
    bool available = false;          // 是否成功读到 B2

    bool work_state = false;         // 0=不工作 1=工作
    bool interlock_1 = true;         // true = 联锁闭合（安全）
    bool interlock_2 = true;
    bool emergency_stop = true;      // true = 未急停
    bool q_gate = true;              // true = 光闸关（安全）
    bool work_enabled = false;       // 已使能工作
    bool over_temperature = true;    // true = 未超温

    // fault_bits 是位图，非 0 表示激光器自身报了故障。
    std::uint16_t fault_bits = 0;
    bool has_fault() const noexcept { return fault_bits != 0; }
};

// 解析一次 0xB2 实际状态帧。**极性按协议原文逐位取反，不是笔误**：
//   data[3]  联锁1   0=短接(闭合,安全)  1=断开(危险)
//   data[4]  联锁2   0=短接(闭合,安全)  1=断开(危险)
//   data[5]  急停     0=正常(安全)      1=急停(危险)
//   data[6]  过温     0=正常(安全)      1=过温(危险)
//   data[19] Q闸设置 0=关(安全)        1=开(有光路)
// LaserSafetyFacts 的语义统一为 "true = 安全"，因此必须取反。
//
// **本函数是 B2 到安全位的唯一转换点**：runtime.cpp 的轮询与
// LaserFireService 的出光前复核都调它，避免两处各写一遍而其中一处写反。
// 曾发生过两处都写成 raw==1（把危险读成安全）的缺陷，故单独抽出来并加测试。
// scaled=false 的字段（协议未确认取值）一律当不安全，不当作"值为真"。
LaserSafetyFacts parse_actual_frame(const libs::laser::Frame& frame,
                                            std::uint32_t* discharge_counter = nullptr);

// 门禁判定的结果。
//
// blocked_by 逐条列出原因——**不要只给一个 bool**。
// 现场排障需要知道"到底哪几条不满足"，而不是"不能出光"。
enum class GateReason {
    ready,                    // 允许
    parameters_incomplete,    // 核心参数未齐备
    laser_unavailable,        // 读不到激光器状态
    interlock_open,           // 联锁断开
    emergency_stopped,        // 急停
    q_gate_open,              // 光闸开
    laser_fault,              // 激光器故障位
    over_temperature,         // 超温
    not_enabled,              // 设备未使能
    laser_not_commissioned,   // 本机出光参数未标定（配置 laser_fire.commissioned=false）
    air_conditioner_interlock,// 空调联锁未通过（仅在 3.9.2 启用后可能出现）
};
const char* to_string(GateReason reason) noexcept;

struct GateVerdict {
    bool allowed = false;                 // 是否允许进入检测/出光
    std::vector<GateReason> blocked_by;   // 允许时为空

    // 判据来源。任务书 §2.2 反复区分"开始检测业务信号"与"物理触发"：
    // 这里的 allowed 只表示"业务层允许"，**不代表物理触发已发生**。
    std::string detail;

    bool ok() const noexcept { return allowed; }
    const char* primary_reason() const noexcept;
};

// 待机就绪门禁 + 出光门禁（3.4.1 + 3.4.2）。
//
// 状态机：disarm() -> (参数齐备 && 联锁安全) -> arm() -> 可出光。
// 任何时刻条件不再满足，evaluate() 自动降回 blocked——不需要显式 disarm()。
class Gate {
public:
    // 设置核心参数是否齐备。由参数模块（能力 3）驱动，不自己查参数。
    void set_parameters_ready(bool ready) noexcept { parameters_ready_ = ready; }

    // 更新激光器事实。每次读回 B2 调一次。
    void update_laser_facts(const LaserSafetyFacts& facts) noexcept { facts_ = facts; }

    // 空调联锁是否通过。**默认 true = 不拦截**：
    // 空调联锁阈值未定（3.9.2 C 类），未标定前不允许它拦停出光。
    // 启用后由 interlock.hpp 显式设置。
    void set_air_conditioner_interlock(bool ok) noexcept { ac_interlock_ok_ = ok; }

    // 设备是否使能（配置层面）。
    void set_laser_enabled(bool enabled) noexcept { laser_enabled_ = enabled; }

    // 本机出光参数是否已标定（配置层面）。
    //
    // **默认 false = 未标定 = 拒绝出光**。与 ac_interlock 同一套纪律：
    // 未标定不得启用。理由见 config::LaserFireConfig 的注释——过去用
    // "协议量程下限"当出光参数，量程下限对限制类参数意味着禁止工作，
    // 现场后果是整台设备不出光、并把机器配置写坏。
    void set_laser_commissioned(bool commissioned) noexcept { laser_commissioned_ = commissioned; }

    // 显式布防/撤防。撤防是运维/调试用，正常流程由 evaluate() 自动判定。
    libs::VoidResult arm();
    libs::VoidResult disarm();
    bool armed() const noexcept { return armed_; }
    MachineState state() const noexcept { return state_; }

    // 判定。这是**唯一的判定入口**——状态灯映射、UI、CLI 都读这里的结果，
    // 不得各自实现一套。
    // 副作用：同步刷新 state_。判定本身就是状态机的推进，因此不是 const。
    GateVerdict evaluate();

    // 只读判定，不刷新 state_。给"只想看一眼"的调用方（灯映射、UI）用。
    GateVerdict peek() const;

    // 供 JSON 回显。
    json::Value to_json() const;

private:
    bool parameters_ready_ = false;
    bool laser_enabled_ = true;
    bool laser_commissioned_ = false;
    bool ac_interlock_ok_ = true;
    bool armed_ = false;
    mutable MachineState state_ = MachineState::standby;
    LaserSafetyFacts facts_;
};

// ── 状态灯业务状态映射（3.4.3）──────────────────────────────────────────────
//
// **纯函数：门禁状态 -> 灯显示意图。不得反向参与门禁判定。**
// 任务书 §7.1：IndicatorUnit 的职责是"将业务状态转换为灯显示意图"。

enum class LampChannel { red, yellow, green, blue, white, buzzer };

enum class LampState { off, on, blink_1hz, blink_2hz };

// 灯色对应**尚未固定**（任务书 §7.1 明示），因此本表是**可配置**的，
// 默认值留空——空表示"这一项没有默认映射"，由配置补。
struct IndicatorMapping {
    // 每个业务状态 -> 每个通道的灯态。缺项即该通道在此状态下不动作。
    std::map<MachineState, std::map<LampChannel, LampState>> table;

    // 把业务状态翻译成灯显示意图。未知状态返回全灭。
    std::map<LampChannel, LampState> map_state(MachineState state) const;
};

// 协议层的通道/状态枚举与本层解耦：本层只产出"意图"，
// 由调用方翻译成 libs::indicator::Channel / State 后下发。
LampChannel to_lamp_channel(libs::indicator::Channel channel) noexcept;
LampState   to_lamp_state(libs::indicator::State state) noexcept;
libs::indicator::Channel from_lamp_channel(LampChannel channel);
libs::indicator::State   from_lamp_state(LampState state);

}  // namespace business::policy
