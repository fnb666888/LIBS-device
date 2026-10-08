#pragma once

// 业务一：就绪判定（readiness）—— 判定层。
//
// ── `evaluate()` 是**纯函数** ────────────────────────────────────────────
//
//   读 facts -> 返回 GateVerdict{allowed, blocked_by, detail}
//
// 它不写激光器、不写灯、不读串口、不起线程。
//
// **为什么必须是纯函数**（三条理由，缺一不可）：
//
//   1. 它才能被离线测试**穷举** —— 门禁是全库安全价值最高的一段逻辑，
//      "每个安全位单独坏掉时是否都拦得住"必须能一条条断言。
//   2. 它才能被**三方共用同一份逻辑**：业务一自己、UI 的 status 面板、
//      业务二在运行期的周期性重判。三处若各判一次，迟早有一处写歪。
//   3. 它才能在未来被搬到常驻进程里而不必重写 —— 纯函数没有状态可搬丢。
//
// 状态机：`disarm() -> (参数齐备 && 联锁安全) -> arm() -> 可出光`。
// 条件不再满足时 evaluate() 自动降回 blocked，**不需要**显式 disarm()。

#include <string>
#include <vector>

#include "libs/business/readiness/facts.hpp"

namespace business::readiness {

// 门禁不放行的原因。**逐条列出，不要只给 bool** ——
// 现场排障要知道"到底哪几条不满足"，而不是"不能出光"。
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
    laser_not_commissioned,   // 本机出光参数未标定
};
const char* to_string(GateReason reason) noexcept;

// 判定结果。
struct GateVerdict {
    bool allowed = false;                 // 是否允许进入检测/出光
    std::vector<GateReason> blocked_by;   // 允许时为空

    // 判据来源。allowed 只表示"业务层允许"，**不代表物理触发已发生**。
    std::string detail;

    const char* primary_reason() const noexcept;
};

// **唯一的判定入口。纯函数**：输入一份 facts，输出一份结论。
//
// 不放行时 blocked_by **逐条列出全部原因**。
GateVerdict evaluate(const ReadinessFacts& facts);

// 判定结果 -> 机器状态。**纯函数**，供灯映射与 UI 复用同一份映射逻辑。
//
// 与 evaluate() 分开是为了让"判定"与"展示"各归一处：判定只回答允不允许，
// 状态只回答现场该看到什么。
MachineState state_of(const ReadinessFacts& facts, const GateVerdict& verdict);

}  // namespace business::readiness