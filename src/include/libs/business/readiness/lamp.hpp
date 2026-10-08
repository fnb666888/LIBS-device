#pragma once

// 业务一：就绪判定（readiness）—— 灯显示层。
//
// ── 状态 -> 灯色的映射是**纯函数** ───────────────────────────────────────
//
//   | 状态        | 灯          | 现场含义                              |
//   |-------------|-------------|---------------------------------------|
//   | ready       | 绿常亮      | 预检通过，门禁已放行                  |
//   | acquiring   | 绿常亮      | 出光采集中 —— 这是**正常**工作状态     |
//   | standby     | 黄常亮      | 设备就绪但参数未齐备 / 未布防          |
//   | fault       | 红常亮      | 联锁开、急停、超温、读不到激光状态    |
//   | recovering  | 红常亮      | 恢复中                                |
//   | offline     | 全灭        | 设备没打开                            |
//
// 三条纪律：
//
//   1. **灯是纯函数输出，不得反向参与门禁判定**。判定唯一入口是 evaluate()。
//      灯灭只说明"现在不能出光"，绝不能反过来变成"因为灯灭所以放行"。
//
//   2. **灯写失败不改变 verdict，也不阻断返回值**。灯的串口坏了，
//      不该让系统拒绝出光 —— 灯是辅助指示，不是判据。
//
//   3. **失败路径也要刷灯**。预检不通过时现场必须立刻看到红/黄，
//      否则操作员只看到"按钮点了没反应"。所以下发灯在返回错误**之前**。
//
// 蜂鸣器恒为 off —— 现场噪声大，误鸣比不鸣更糟。需要报警声音改
// `lamp_states()` 一处。

#include <map>
#include <string>

#include "libs/business/readiness/facts.hpp"
#include "libs/devices/indicator.hpp"
#include "libs/error.hpp"

namespace business::readiness {

// 把门禁状态转成灯显示意图。**纯函数**，不碰设备，便于离线测试。
// 全库唯一的 状态->灯色 转换点。
std::map<libs::indicator::Channel, libs::indicator::State>
lamp_states(MachineState state);

// 灯显示意图的业务语义名，给日志与 status 用。
const char* lamp_intent_name(MachineState state) noexcept;

// 一次灯下发的原子动作。**全库唯一的一个下发点**。
//
// indicator 为空时返回 unsupported（灯没接上），**不静默成功** ——
// 调用方要能看见"灯没接上"，而不是收到一个假的成功。
libs::Result<libs::devices::IndicatorApplyReport> apply_lamp(
        MachineState state, libs::devices::Indicator* indicator);

}  // namespace business::readiness