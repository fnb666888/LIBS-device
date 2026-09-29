#pragma once

// 空调与检测的联锁阈值（能力 9 的 3.9.2，**C 类**）。
//
// 卡点 G1（范围报告 §3.9.2）：任务书 §6.1 明示"未确定空调状态与允许检测的联锁条件"。
// 前置条件：① 联锁触发温度/状态位 ② 触发后的动作（告警/暂停/停机）③ 恢复条件。
//
// **这一对子项与 3.4.2 出光门禁的区别是本文件存在的全部理由**：
// 3.4.2 六个联锁位已由驱动解码、规则已确定，是 A 类、已实现、已测；
// 本子项的阈值与动作都没定，是 C 类。
// 合并到同一能力（范围报告 §3.9）**不改变实施纪律**：
// 子项不得互相代偿——门禁里那条 air_conditioner_interlock 拦截点，
// 在本文件落地之前**必须保持"不拦截"**，不得由 3.4.2 顺手补上。
//
// 因此本文件**没有判定函数**，只有一个显式的空实现点。
// 门禁（gate.hpp）通过 `set_air_conditioner_interlock()` 接收本模块的结论，
// 而本模块的 `evaluate()` 永远返回 unsupported——除非阈值已标定且配置里
// `ac_interlock.enabled = true` 且 `calibrated = true`（site_config 校验已保证后两者绑定）。

#include <optional>
#include <string>

#include "libs/business/config/site_config.hpp"
#include "libs/error.hpp"
#include "libs/protocols/air_conditioner.hpp"

namespace business::services {

// 联锁阈值。**当前没有任何可用的阈值**——所以只有"未标定"这一种状态。
//
// 注意这个结构体里没有 default_value：没有阈值就是没有阈值，
// 不能用 0 代替（0 会被读成"阈值是 0 度"，进而永远或永不触发）。
struct AirConditionerInterlockPolicy {
    bool enabled = false;         // 是否参与门禁。未标定一律 false
    bool calibrated = false;      // 阈值是否已标定。当前永远为 false

    // 触发温度/状态位。未标定时为 nullopt，**不是 0**。
    std::optional<double> trigger_indoor_temp_c;
    std::optional<libs::air_conditioner::StatusBit> trigger_status_bit;

    // 触发后动作。范围报告 §3.9.2 前置条件②未确认，因此只有"未知"一态。
    enum class Action { unknown, alarm_only, pause_acquisition, stop };
    Action action = Action::unknown;

    // 恢复条件。未标定时为 nullopt。
    std::optional<double> resume_indoor_temp_c;

    static AirConditionerInterlockPolicy uncalibrated() {
        AirConditionerInterlockPolicy p;
        p.enabled = false;
        p.calibrated = false;
        p.action = Action::unknown;
        return p;
    }
};

// 空调联锁的判定入口。
//
// 返回 true/false 意味着"阈值已标定且结论明确"。
// 阈值未标定时返回 unsupported——**不返回 true（放行）也不返回 false（拦停）**。
// 这是"未知 ≠ 正常"（范围报告 §7 纪律 2）在本子项上的具体形态。
class AirConditionerInterlock {
public:
    explicit AirConditionerInterlock(const config::SiteConfig& site);

    const AirConditionerInterlockPolicy& policy() const noexcept { return policy_; }

    // 当前策略是否真的会参与门禁。
    bool participates_in_gate() const noexcept { return policy_.enabled && policy_.calibrated; }

    // 按一次状态读判定。communication_ok=false 时直接返回 unsupported：
    // 通信失败绝不能被表达成"温度正常"（任务书 §6.1）。
    libs::Result<bool> evaluate(const libs::air_conditioner::StatusReading& status,
                                const libs::air_conditioner::ParameterReading& params,
                                bool communication_ok) const;

    // 前置条件清单，供 `businessd status` 与日志引用。
    static std::string prerequisites();

private:
    AirConditionerInterlockPolicy policy_;
};

}  // namespace business::services
