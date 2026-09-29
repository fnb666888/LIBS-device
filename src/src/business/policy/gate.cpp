#include "libs/business/policy/gate.hpp"

#include <sstream>

namespace business::policy {

LaserSafetyFacts parse_actual_frame(const libs::laser::Frame& frame,
                                    std::uint32_t* discharge_counter) {
    LaserSafetyFacts facts;
    for (const auto& field : frame.fields) {
        if (field.name == "work_state") {
            facts.work_state = field.raw == 1;
        } else if (field.name == "interlock_1") {
            facts.interlock_1 = field.scaled && field.raw == 0;   // 1=断开=不安全
        } else if (field.name == "interlock_2") {
            facts.interlock_2 = field.scaled && field.raw == 0;
        } else if (field.name == "emergency_stop") {
            facts.emergency_stop = field.scaled && field.raw == 0; // 1=急停=不安全
        } else if (field.name == "q_gate") {
            facts.q_gate = field.scaled && field.raw == 0;         // 1=开=不安全
        } else if (field.name == "work_enabled") {
            facts.work_enabled = field.raw == 1;
        } else if (field.name == "overtemperature") {
            facts.over_temperature = field.scaled && field.raw == 0; // 1=过温=不安全
        } else if (field.name == "fault_bits") {
            facts.fault_bits = field.raw;
        } else if (field.name == "discharge_counter" && discharge_counter != nullptr) {
            // 必须取 raw32：协议里这是 4 字节字段，取 raw 会被截断。
            *discharge_counter = field.wide ? field.raw32 : field.raw;
        }
    }
    facts.available = true;
    return facts;
}

const char* to_string(MachineState state) noexcept {
    switch (state) {
        case MachineState::offline:    return "offline";
        case MachineState::standby:    return "standby";
        case MachineState::ready:      return "ready";
        case MachineState::acquiring:  return "acquiring";
        case MachineState::fault:      return "fault";
        case MachineState::recovering: return "recovering";
    }
    return "unknown";
}

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
        case GateReason::air_conditioner_interlock: return "air_conditioner_interlock";
    }
    return "unknown";
}

const char* GateVerdict::primary_reason() const noexcept {
    return blocked_by.empty() ? "ready" : to_string(blocked_by.front());
}

libs::VoidResult Gate::arm() {
    if (!laser_enabled_) {
        return libs::Error::make(libs::ErrorKind::device, "gate_laser_disabled",
                                 "激光器在配置中未启用，无法布防");
    }
    armed_ = true;
    // 布防后立刻反映当前条件：条件不满足时 state 仍是 standby/fault。
    evaluate();
    return libs::VoidResult();
}

libs::VoidResult Gate::disarm() {
    armed_ = false;
    state_ = MachineState::standby;
    return libs::VoidResult();
}

GateVerdict Gate::peek() const {
    GateVerdict v;

    if (!laser_enabled_) {
        v.blocked_by.push_back(GateReason::not_enabled);
    }
    if (!parameters_ready_) {
        v.blocked_by.push_back(GateReason::parameters_incomplete);
    }
    // 未标定 = 不知道该发什么参数 = 不能出光。这一条与"参数齐备"是两回事：
    // 参数齐备说的是业务参数表填齐了，本机标定说的是**这台设备**的
    // 电流/脉宽/Q 延时到底该取多少。后者过去被硬编码成量程下限，是事故根因。
    if (!laser_commissioned_) {
        v.blocked_by.push_back(GateReason::laser_not_commissioned);
    }

    // 读不到激光器状态时**不能假定安全**。这是最关键的一条：
    // "不知道"不等于"正常"，也不等于"安全"。
    if (!facts_.available) {
        v.blocked_by.push_back(GateReason::laser_unavailable);
    } else {
        if (!facts_.interlock_1 || !facts_.interlock_2) {
            v.blocked_by.push_back(GateReason::interlock_open);
        }
        if (!facts_.emergency_stop) {
            v.blocked_by.push_back(GateReason::emergency_stopped);
        }
        if (!facts_.q_gate) {
            v.blocked_by.push_back(GateReason::q_gate_open);
        }
        if (facts_.has_fault()) {
            v.blocked_by.push_back(GateReason::laser_fault);
        }
        if (!facts_.over_temperature) {
            v.blocked_by.push_back(GateReason::over_temperature);
        }
    }

    if (!ac_interlock_ok_) {
        v.blocked_by.push_back(GateReason::air_conditioner_interlock);
    }

    v.allowed = v.blocked_by.empty() && armed_;

    if (!armed_) {
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

GateVerdict Gate::evaluate() {
    const GateVerdict v = peek();
    // 状态机只由判定结果推进，避免出现"state 与 verdict 不一致"的中间态。
    if (!armed_) {
        state_ = MachineState::standby;
    } else if (v.allowed) {
        state_ = MachineState::ready;
    } else if (v.blocked_by.size() == 1 && v.blocked_by.front() == GateReason::parameters_incomplete) {
        // 只有参数不齐 -> 还是待机；有联锁/急停/故障 -> 异常。
        state_ = MachineState::standby;
    } else {
        state_ = MachineState::fault;
    }
    return v;
}
json::Value Gate::to_json() const {
    const GateVerdict v = peek();
    json::Value root = json::object();
    root.set("state", json::Value(to_string(state_)));
    root.set("armed", json::Value(armed_));
    root.set("allowed", json::Value(v.allowed));
    root.set("detail", json::Value(v.detail));

    json::Array reasons;
    for (GateReason r : v.blocked_by) reasons.emplace_back(to_string(r));
    root.set("blocked_by", json::Value(std::move(reasons)));

    json::Value laser = json::object();
    laser.set("available", json::Value(facts_.available));
    laser.set("interlock_1", json::Value(facts_.interlock_1));
    laser.set("interlock_2", json::Value(facts_.interlock_2));
    laser.set("emergency_stop", json::Value(facts_.emergency_stop));
    laser.set("q_gate", json::Value(facts_.q_gate));
    laser.set("work_state", json::Value(facts_.work_state));
    laser.set("work_enabled", json::Value(facts_.work_enabled));
    laser.set("over_temperature", json::Value(facts_.over_temperature));
    laser.set("fault_bits", json::Value(static_cast<double>(facts_.fault_bits)));
    root.set("laser_facts", std::move(laser));

    root.set("parameters_ready", json::Value(parameters_ready_));
    root.set("laser_enabled", json::Value(laser_enabled_));
    root.set("laser_commissioned", json::Value(laser_commissioned_));
    root.set("ac_interlock_ok", json::Value(ac_interlock_ok_));
    return root;
}

std::map<LampChannel, LampState> IndicatorMapping::map_state(MachineState state) const {
    const auto it = table.find(state);
    if (it == table.end()) {
        // 未配置的映射一律全灭。**不猜**灯色——任务书 §7.1 明示灯色对应尚未固定。
        return {};
    }
    return it->second;
}

LampChannel to_lamp_channel(libs::indicator::Channel channel) noexcept {
    switch (channel) {
        case libs::indicator::Channel::red:    return LampChannel::red;
        case libs::indicator::Channel::yellow: return LampChannel::yellow;
        case libs::indicator::Channel::green:  return LampChannel::green;
        case libs::indicator::Channel::blue:   return LampChannel::blue;
        case libs::indicator::Channel::white:  return LampChannel::white;
        case libs::indicator::Channel::buzzer: return LampChannel::buzzer;
    }
    return LampChannel::red;
}

LampState to_lamp_state(libs::indicator::State state) noexcept {
    switch (state) {
        case libs::indicator::State::off:       return LampState::off;
        case libs::indicator::State::on:        return LampState::on;
        case libs::indicator::State::blink_1hz: return LampState::blink_1hz;
        case libs::indicator::State::blink_2hz: return LampState::blink_2hz;
    }
    return LampState::off;
}

libs::indicator::Channel from_lamp_channel(LampChannel channel) {
    switch (channel) {
        case LampChannel::red:    return libs::indicator::Channel::red;
        case LampChannel::yellow: return libs::indicator::Channel::yellow;
        case LampChannel::green:  return libs::indicator::Channel::green;
        case LampChannel::blue:   return libs::indicator::Channel::blue;
        case LampChannel::white:  return libs::indicator::Channel::white;
        case LampChannel::buzzer: return libs::indicator::Channel::buzzer;
    }
    return libs::indicator::Channel::red;
}

libs::indicator::State from_lamp_state(LampState state) {
    switch (state) {
        case LampState::off:       return libs::indicator::State::off;
        case LampState::on:        return libs::indicator::State::on;
        case LampState::blink_1hz: return libs::indicator::State::blink_1hz;
        case LampState::blink_2hz: return libs::indicator::State::blink_2hz;
    }
    return libs::indicator::State::off;
}

}  // namespace business::policy
