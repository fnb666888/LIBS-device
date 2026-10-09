#include "libs/business/readiness/lamp.hpp"

#include "libs/business/readiness/facts.hpp"

namespace business::readiness {

std::map<libs::indicator::Channel, libs::indicator::State>
lamp_states(MachineState state) {
    using Ch = libs::indicator::Channel;
    using St = libs::indicator::State;

    // 完整业务意图。驱动按型号先全灭，再点亮使用的颜色，避免残留旧灯色；
    // 不向三色灯发送蓝/白指令，也不要求 K50 支持逐路关闭。
    std::map<Ch, St> table;
    switch (state) {
        case MachineState::ready:
        case MachineState::acquiring:
            // **出光采集中 = 绿灯**。正在测量是正常状态，不是异常。
            table[Ch::red] = St::off;
            table[Ch::yellow] = St::off;
            table[Ch::green] = St::on;
            break;
        case MachineState::standby:
            table[Ch::red] = St::off;
            table[Ch::yellow] = St::on;
            table[Ch::green] = St::off;
            break;
        case MachineState::fault:
        case MachineState::recovering:
            table[Ch::red] = St::on;
            table[Ch::yellow] = St::off;
            table[Ch::green] = St::off;
            break;
        case MachineState::offline:
        default:
            // 全部熄灭：设备没打开，灯也不该显示任何业务意图。
            table[Ch::red] = St::off;
            table[Ch::yellow] = St::off;
            table[Ch::green] = St::off;
            break;
    }
    // 蓝 / 白 / 蜂鸣器：当前业务流程不使用，恒为 off。
    table[Ch::blue] = St::off;
    table[Ch::white] = St::off;
    table[Ch::buzzer] = St::off;
    return table;
}

const char* lamp_intent_name(MachineState state) noexcept {
    // 意图名带亮色，而不是状态名。原因是两个状态共享一种灯色：
    // ready 和 acquiring 都是绿灯；fault 和 recovering 都是红灯。
    // 若只返回状态名，status 上就无法告诉操作员“灯应该是什么色”，
    // 而这正是现场排障时唯一能看的东西。
    switch (state) {
        case MachineState::ready:
        case MachineState::acquiring:  return "ready_green";     // 准备 / 正在采集：绿灯
        case MachineState::standby:    return "standby_yellow";   // 待机：黄灯
        case MachineState::fault:
        case MachineState::recovering: return "fault_red";      // 异常 / 恢复中：红灯
        case MachineState::offline:
        default:                      return "offline_all_off";  // 未上电：全灭
    }
}

libs::Result<libs::devices::IndicatorApplyReport> apply_lamp(
        MachineState state, libs::devices::Indicator* indicator) {
    if (indicator == nullptr) {
        // indicator 为空 = 设备没打开或配置里没启用。这是**如实报告**，
        // 不是静默成功：调用方要能看见"灯没接上"。
        return libs::Error::make(libs::ErrorKind::unsupported, "indicator_not_open",
                                 "状态灯未打开或未启用，本次灯色未下发");
    }
    return indicator->applyAll(lamp_states(state), /*verify=*/true);
}

}  // namespace business::readiness
