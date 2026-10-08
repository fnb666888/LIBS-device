#pragma once

// 业务一：就绪判定（readiness）—— 事实层。
//
// ── 这一层只承载"事实"，没有任何判断 ─────────────────────────────────────
//
// 读设备 -> LaserSafetyFacts / ReadinessFacts -> verdict() -> GateVerdict。
// 本文件是 `facts` 这一层：把设备的原始状态解码成**结构化的安全位**，
// 不回答"现在能不能出光"。
//
// 三条纪律：
//
//   1. **`parse_actual()` 是 B2 到安全位的唯一转换点**。轮询与出光前复核
//      都调它，不允许两处各写一遍。曾发生过两处都写成 `raw==1`，
//      把危险读成安全的缺陷 —— 故单独抽出来并加离线测试。
//
//   2. **语义统一为 "true = 安全"**。协议原文里 0/1 的极性是反的
//      （联锁 0=闭合=安全），因此必须逐位取反。**取反不是笔误。**
//
//   3. **读不到 != 安全**。`available=false` 时按不安全处理，
//      不当作"值为真"，也不当作"没事"。

#include <cstdint>

#include "libs/devices/laser.hpp"

namespace business::readiness {

// 业务状态。状态灯与门禁共用这一套枚举 —— 只有判定结果才产出它。
enum class MachineState {
    offline,      // 未上电 / 未配置
    standby,      // 待机，参数未齐备
    ready,        // 就绪，参数齐备且无联锁
    acquiring,    // 采集中
    fault,        // 异常
    recovering,   // 恢复中
};
const char* to_string(MachineState state) noexcept;

// 激光器已解码的状态位（依据 devices/laser.cpp 的 0xB2 解码）。
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
// **本函数是 B2 到安全位的唯一转换点**：runtime 的轮询与 MeasurementJob
// 的运行期重判都调它，避免两处各写一遍而其中一处写反。
// scaled=false 的字段（协议未确认取值）一律当不安全，不当作"值为真"。
LaserSafetyFacts parse_actual(const libs::devices::LaserActual& actual,
                              std::uint32_t* discharge_counter = nullptr);

// 一次判定的全部输入。**这是 verdict() 的唯一入参**。
//
// 之所以把它抽成一个结构体（而不是让 verdict() 一手收五个 bool）：
// 这样 verdict() 就是**可离线穷举的纯函数** —— 给定一份 facts，
// 就能离线算出结论，不需要设备、不需要串口、不需要线程。
struct ReadinessFacts {
    LaserSafetyFacts laser;            // 从激光器 B2 状态帧解码

    bool devices_opened = false;       // 设备是否打开
    bool spectrometer_ready = false;   // 光谱仪是否可用
    bool parameters_complete = false;  // 核心参数是否齐备（由参数模块驱动）

    // 设备是否在配置里使能。本机出光参数是否已标定。
    bool laser_enabled = true;
    bool laser_commissioned = false;   // 默认 false = 未标定 = 拒绝出光

    // 是否已布防。状态机：disarm() -> (参数齐备 && 联锁安全) -> arm()。
    bool armed = false;

    std::uint64_t sampled_at_ms = 0;   // 本次采样时刻（进程内单调）
};

}  // namespace business::readiness