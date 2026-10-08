#include "libs/business/readiness/verdict.hpp"

#include <sstream>

namespace business::readiness {

const char* to_string(GateReason reason) noexcept {
    switch (reason) {
        case GateReason::ready:                 return "ready";
        case GateReason::parameters_incomplete: return "parameters_incomplete";
        case GateReason::laser_unavailable:     return "laser_unavailable";
        case GateReason::interlock_open:        return "interlock_open";
        case GateReason::emergency_stopped:     return "emergency_stopped";
        case GateReason::q_gate_open:           return "q_gate_open";
        case GateReason::laser_fault:           return "laser_fault";
        case GateReason::over_temperature:      return "over_temperature";
        case GateReason::not_enabled:           return "not_enabled";
        case GateReason::laser_not_commissioned: return "laser_not_commissioned";
    }
    return "unknown";
}

const char* GateVerdict::primary_reason() const noexcept {
    return blocked_by.empty() ? "ready" : to_string(blocked_by.front());
}

GateVerdict evaluate(const ReadinessFacts& facts) {
    GateVerdict v;

    // “能不能出光”不只看激光器自己。设备没打开、或者光谱仪不可用时也不能出光：
    // 前者发出去的光没人接，后者发出去了也没人采到——
    // 两者都是“光白发”，而不是“持续测量”。
    //
    // 这里必须与 state_of() 保持一致：state_of() 在设备未打开时返回 offline，
    // 而如果 verdict 这边放行了，就会出现“状态机说离线、门禁却说可以”的矛盾。
    if (!facts.devices_opened || !facts.laser_enabled || !facts.spectrometer_ready) {
        v.blocked_by.push_back(GateReason::not_enabled);
    }
    if (!facts.parameters_complete) {
        v.blocked_by.push_back(GateReason::parameters_incomplete);
    }
    // 未标定 = 不知道该发什么参数 = 不能出光。这一条与"参数齐备"是两回事：
    // 参数齐备说的是业务参数表填齐了，本机标定说的是**这台设备**的
    // 电流/脉宽/Q 延时到底该取多少。后者过去被硬编码成量程下限，是事故根因。
    if (!facts.laser_commissioned) {
        v.blocked_by.push_back(GateReason::laser_not_commissioned);
    }

    // 读不到激光器状态时**不能假定安全**。这是最关键的一条：
    // "不知道"不等于"正常"，也不等于"安全"。
    if (!facts.laser.available) {
        v.blocked_by.push_back(GateReason::laser_unavailable);
    } else {
        if (!facts.laser.interlock_1 || !facts.laser.interlock_2) {
            v.blocked_by.push_back(GateReason::interlock_open);
        }
        if (!facts.laser.emergency_stop) {
            v.blocked_by.push_back(GateReason::emergency_stopped);
        }
        if (!facts.laser.q_gate) {
            v.blocked_by.push_back(GateReason::q_gate_open);
        }
        if (facts.laser.has_fault()) {
            v.blocked_by.push_back(GateReason::laser_fault);
        }
        if (!facts.laser.over_temperature) {
            v.blocked_by.push_back(GateReason::over_temperature);
        }
    }

    v.allowed = v.blocked_by.empty() && facts.armed;

    if (!facts.armed) {
        v.detail = "未布防";
    } else if (v.allowed) {
        v.detail = "允许：参数齐备且联锁全部安全";
    } else {
        std::ostringstream os;
        os << "禁止出光：";
        bool first = true;
        for (GateReason r : v.blocked_by) {
            if (!first) os << "、";
            first = false;
            os << to_string(r);
        }
        v.detail = os.str();
    }
    return v;
}

MachineState state_of(const ReadinessFacts& facts, const GateVerdict& verdict) {
    if (!facts.devices_opened) return MachineState::offline;
    if (!facts.armed) return MachineState::standby;
    if (verdict.allowed) return MachineState::ready;
    // 只有参数不齐 -> 还是待机；有联锁/急停/故障 -> 异常。
    if (verdict.blocked_by.size() == 1 &&
        verdict.blocked_by.front() == GateReason::parameters_incomplete) {
        return MachineState::standby;
    }
    return MachineState::fault;
}

}  // namespace business::readiness