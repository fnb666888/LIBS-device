#include "libs/business/services/ac_interlock.hpp"

namespace business::services {

AirConditionerInterlock::AirConditionerInterlock(const config::SiteConfig& site) {
    const config::AirConditionerInterlockConfig& c = site.ac_interlock;
    // site_config::validate 已经拒绝 enabled && !calibrated，
    // 但本类不假设调用方一定走过校验：这里再判一次，宁可保守。
    if (c.enabled && c.calibrated) {
        policy_.enabled = true;
        policy_.calibrated = true;
        policy_.trigger_indoor_temp_c = c.trigger_indoor_temp_c;
    }
    // action 与 resume 条件在配置里没有字段，因为它们本身是未确认的前置条件。
}

libs::Result<bool> AirConditionerInterlock::evaluate(const libs::air_conditioner::StatusReading& status,
                                                     const libs::air_conditioner::ParameterReading& params,
                                                     bool communication_ok) const {
    (void)status;
    (void)params;

    if (!communication_ok) {
        return libs::Error::make(
            libs::ErrorKind::unsupported, "ac_interlock_no_communication",
            "空调通信不可用，无法判定联锁。通信失败绝不能表达为“温度正常”"
            "（任务书 §6.1 / 范围报告 §7 纪律 2）");
    }
    if (!participates_in_gate()) {
        return libs::Error::make(
            libs::ErrorKind::unsupported, "ac_interlock_uncalibrated",
            "空调联锁阈值未标定，联锁不参与门禁。前置条件：" + prerequisites());
    }
    // 阈值已标定的分支当前不可达：标定方案本身就是未确认的前置条件之一。
    // 保留位置并写明为什么不可达，避免日后有人以为这里漏写了。
    return libs::Error::make(
        libs::ErrorKind::unsupported, "ac_interlock_action_unspecified",
        "联锁触发后的动作（告警/暂停/停机）与恢复条件未定义。前置条件：" + prerequisites());
}

std::string AirConditionerInterlock::prerequisites() {
    return "① 联锁触发温度/状态位 ② 触发后的动作（告警/暂停/停机）③ 恢复条件"
           "（任务书 §6.1 未确定空调状态与允许检测的联锁条件；范围报告 §3.9.2）";
}

}  // namespace business::services
