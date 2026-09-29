#include "libs/business/services/motion.hpp"

namespace business::services {

json::Value to_json(const MotionTiming& timing) {
    json::Value v = json::object();
    v.set("calibrated", json::Value(timing.calibrated));
    if (timing.forward_time_s.has_value()) v.set("forward_time_s", json::Value(*timing.forward_time_s));
    else v.set("forward_time_s", json::Value());
    if (timing.reverse_time_s.has_value()) v.set("reverse_time_s", json::Value(*timing.reverse_time_s));
    else v.set("reverse_time_s", json::Value());
    v.set("note", json::Value("calibrated=false 表示 1 s 是占位值（任务书 R01 未标定）；"
                              "计时完成不等于已证实到位（任务书 §3）"));
    return v;
}

libs::VoidResult UnavailablePositionSwitch::switch_to(int position) {
    return libs::Error::make(
        libs::ErrorKind::unsupported, "position_switch_unavailable",
        "位置切换未实现：驱动层无电机/继电器/限位入口，且不复用旧 MCC4 步进电机绑定。"
        "前置条件：" + prerequisites() + "（本次请求位置 " + std::to_string(position) + " 未执行）");
}

libs::Result<int> UnavailablePositionSwitch::current_position() {
    return libs::Error::make(
        libs::ErrorKind::unsupported, "position_feedback_unavailable",
        "当前位置不可知：限位反馈方式（干接点或总线）未确认，驱动层无入口。"
        "前置条件：" + prerequisites());
}

std::string UnavailablePositionSwitch::prerequisites() {
    return "① 电机控制方式（继电器/接触器/变频器）② 限位开关是干接点还是总线 "
           "③ 接线图核实 ④ 正反转行程时间标定（任务书 §2.2；范围报告 §3.12.2 G1/G2）";
}

}  // namespace business::services
