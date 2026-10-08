#include "libs/business/readiness/precheck.hpp"

#include <sstream>
#include <utility>

namespace business::readiness {

PrecheckService::PrecheckService(ReadinessMonitor& monitor,
                                 libs::devices::Indicator* indicator)
    : monitor_(monitor), indicator_(indicator) {}

void PrecheckService::record_lamp(PrecheckReport& report, MachineState state) {
    report.state = state;
    report.lamp_intent = lamp_intent_name(state);
    const auto applied = apply_lamp(state, indicator_);
    report.lamp_applied = applied.ok();
    report.lamp_error = applied ? std::string() : applied.error().message;
}

libs::VoidResult PrecheckService::run() {
    last_report_ = PrecheckReport{};

    // 布防 -> 判定。**顺序不能换**：不布防则 allowed 恒假，
    // 而 arm() 内部已会跑一次 evaluate_now()，所以这里拿的是最新结论。
    const libs::VoidResult armed = monitor_.arm();
    if (!armed) {
        // 布防失败（激光在配置里未使能）：仍然刷灯，让现场看到"不能出光"。
        last_report_.state = monitor_.latest_state();
        last_report_.detail = "门禁布防失败：" + armed.error().message;
        record_lamp(last_report_, last_report_.state);
        return armed.error();
    }

    const GateVerdict verdict = monitor_.evaluate_now();
    const MachineState state = monitor_.latest_state();

    last_report_.state = state;
    last_report_.allowed = verdict.allowed;
    last_report_.detail = verdict.detail;
    for (GateReason r : verdict.blocked_by) {
        last_report_.blocked_by.emplace_back(to_string(r));
    }
    // 读激光状态是否成功。false = 读不到，按"不安全"处理，不当作"没事"。
    last_report_.laser_readable = monitor_.laser_readable();

    // 先刷灯再返回失败：预检不通过时现场必须立刻看到红/黄。
    record_lamp(last_report_, state);

    if (!verdict.allowed) {
        std::ostringstream os;
        os << "门禁未放行：" << verdict.detail;
        return libs::Error::make(libs::ErrorKind::device, "gate_blocked", os.str());
    }
    return libs::VoidResult();
}

libs::VoidResult PrecheckService::sync_lamp_only() {
    const MachineState state = monitor_.latest_state();
    last_report_.state = state;
    last_report_.lamp_intent = lamp_intent_name(state);
    const auto applied = apply_lamp(state, indicator_);
    last_report_.lamp_applied = applied.ok();
    last_report_.lamp_error = applied ? std::string() : applied.error().message;
    return applied ? libs::VoidResult() : libs::VoidResult(applied.error());
}

libs::VoidResult PrecheckService::force_offline() {
    last_report_.state = MachineState::offline;
    last_report_.lamp_intent = lamp_intent_name(MachineState::offline);
    const auto applied = apply_lamp(MachineState::offline, indicator_);
    last_report_.lamp_applied = applied.ok();
    last_report_.lamp_error = applied ? std::string() : applied.error().message;
    return applied ? libs::VoidResult() : libs::VoidResult(applied.error());
}

json::Value PrecheckService::to_json(const PrecheckReport& report) const {
    json::Value v = json::object();
    v.set("state", json::Value(to_string(report.state)));
    v.set("allowed", json::Value(report.allowed));
    v.set("detail", json::Value(report.detail));
    json::Array blocked;
    for (const std::string& s : report.blocked_by) blocked.emplace_back(s);
    v.set("blocked_by", json::Value(std::move(blocked)));
    v.set("laser_readable", json::Value(report.laser_readable));

    // 灯显示意图 + 下发结果。**lamp_applied=false 与 allowed 无关**。
    v.set("lamp_intent", json::Value(report.lamp_intent));
    v.set("lamp_applied", json::Value(report.lamp_applied));
    if (!report.lamp_error.empty()) v.set("lamp_error", json::Value(report.lamp_error));
    return v;
}

}  // namespace business::readiness