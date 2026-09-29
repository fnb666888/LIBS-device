#include "libs/business/services/ore_presence.hpp"

namespace business::services {

namespace {
// 取一个寄存器**按地址**。
//
// 曾经这里按 name 匹配，而协议层给 0x0202/0x0207 起的名字是
// "advanced_distance_raw" / "sample_counter_raw"，与这里的期望名不一致，
// 结果永远匹配不上：distance_raw 恒为 0，而且因为哨兵寄存器也找不到，
// in_range 恒为 false。**地址才是稳定键**，name 只是给人看的标签。
const libs::rangefinder::Register* find(const libs::rangefinder::Reading& reading,
                                         std::uint16_t address) {
    for (const auto& reg : reading.registers) {
        if (reg.address == address) return &reg;
    }
    return nullptr;
}
}  // namespace

const char* to_string(DecisionBasis basis) noexcept {
    switch (basis) {
        case DecisionBasis::unconfigured: return "unconfigured";
        case DecisionBasis::absolute:     return "absolute";
        case DecisionBasis::delta:        return "delta";
    }
    return "unconfigured";
}

json::Value to_json(const DistanceObservation& obs) {
    json::Value v = json::object();
    v.set("wall_clock_ms", json::Value(static_cast<double>(obs.wall_clock_ms)));
    v.set("read_ok", json::Value(obs.read_ok));
    if (!obs.read_ok) v.set("read_error", json::Value(obs.read_error));
    v.set("distance_raw", json::Value(static_cast<double>(obs.distance_raw)));
    v.set("confidence_raw", json::Value(static_cast<double>(obs.confidence_raw)));
    v.set("sample_counter", json::Value(static_cast<double>(obs.sample_counter)));
    v.set("device_valid", json::Value(obs.device_valid));
    v.set("in_range", json::Value(obs.in_range));
    if (!obs.auxiliary_raw.empty()) {
        json::Value aux = json::object();
        for (const auto& kv : obs.auxiliary_raw) aux.set(kv.first, json::Value(static_cast<double>(kv.second)));
        v.set("auxiliary_raw", std::move(aux));
    }
    v.set("note", json::Value("distance_raw/confidence_raw 原样记录，未换算为工程量；"
                              "in_range=false 表示 raw 落在量程哨兵之外"));
    return v;
}

DistanceObservation observe(const libs::rangefinder::Reading& reading,
                            const libs::Error& error_or_none) {
    DistanceObservation obs;
    obs.wall_clock_ms = wall_clock_ms();
    if (error_or_none) {
        obs.read_ok = false;
        obs.read_error = error_or_none.message;
        return obs;
    }

    const auto* dist = find(reading, libs::rangefinder::reg::kDistanceRaw);
    const auto* conf = find(reading, libs::rangefinder::reg::kConfidence);
    const auto* counter = find(reading, libs::rangefinder::reg::kSampleCounter);
    obs.distance_raw = dist ? dist->raw : 0;
    obs.confidence_raw = conf ? conf->raw : 0;
    obs.sample_counter = counter ? counter->raw : 0;
    obs.device_valid = dist ? dist->valid : true;

    // 业务层自己判超量程：协议层 default valid=true 是"本层不判断"。
    const auto* zero = find(reading, libs::rangefinder::reg::kDistanceZeroOnOutOfRange);
    const auto* max = find(reading, libs::rangefinder::reg::kDistanceMaxOnOutOfRange);
    if (zero && max) {
        obs.in_range = (obs.distance_raw > zero->raw && obs.distance_raw < max->raw);
    } else {
        // 哨兵寄存器没读到就不能说"在量程内"。未知 ≠ 在量程内。
        obs.in_range = false;
    }

    // 其余寄存器原样带上，不换算。
    for (const auto& r : reading.registers) {
        if (r.address == libs::rangefinder::reg::kDistanceRaw || r.address == libs::rangefinder::reg::kConfidence ||
            r.address == libs::rangefinder::reg::kSampleCounter ||
            r.address == libs::rangefinder::reg::kDistanceZeroOnOutOfRange ||
            r.address == libs::rangefinder::reg::kDistanceMaxOnOutOfRange) {
            continue;
        }
        obs.auxiliary_raw[r.name] = r.raw;
    }
    return obs;
}

json::Value to_json(const OreDecision& decision) {
    json::Value v = json::object();
    if (decision.ore_present.has_value()) {
        v.set("ore_present", json::Value(*decision.ore_present));
    } else {
        v.set("ore_present", json::Value());   // null = 未知，不是 false
    }
    v.set("basis", json::Value(decision.basis));
    v.set("has_observation", json::Value(decision.has_observation));
    v.set("decided_at_ms", json::Value(static_cast<double>(decision.decided_at_ms)));

    json::Value p = json::object();
    p.set("basis", json::Value(to_string(decision.parameters_snapshot.basis)));
    p.set("calibrated", json::Value(decision.parameters_snapshot.calibrated));
    p.set("distance_unit", json::Value(decision.parameters_snapshot.distance_unit));
    p.set("consecutive_confirmations",
          json::Value(static_cast<double>(decision.parameters_snapshot.consecutive_confirmations)));
    if (decision.parameters_snapshot.threshold.has_value()) {
        p.set("threshold", json::Value(*decision.parameters_snapshot.threshold));
    } else {
        p.set("threshold", json::Value());
    }
    if (decision.parameters_snapshot.hysteresis.has_value()) {
        p.set("hysteresis", json::Value(*decision.parameters_snapshot.hysteresis));
    } else {
        p.set("hysteresis", json::Value());
    }
    v.set("parameters_snapshot", std::move(p));
    return v;
}

OrePresenceJudge::OrePresenceJudge(OrePresencePolicy policy) : policy_(std::move(policy)) {}

void OrePresenceJudge::record(const DistanceObservation& observation) {
    // 先记录、后判定：这里只保存，判定在 decide() 里另算。
    last_ = observation;
}

OreDecision OrePresenceJudge::decide() const {
    OreDecision decision;
    decision.decided_at_ms = wall_clock_ms();
    decision.parameters_snapshot = policy_;
    decision.has_observation = last_.has_value();

    // 情形 1：没有可用观测 —— 不知道。
    if (!last_.has_value() || !last_->read_ok) {
        decision.basis = last_.has_value() ? ("读失败: " + last_->read_error) : "尚无观测";
        return decision;   // ore_present 保持 nullopt
    }
    // 情形 2：超量程 —— 不知道。
    if (!last_->in_range) {
        decision.basis = "距离 raw 落在量程之外，无法判定";
        return decision;
    }
    // 情形 3：规则未标定 —— 不知道。
    if (!policy_.calibrated || policy_.basis == DecisionBasis::unconfigured) {
        decision.basis = "判定规则未标定：" + prerequisites();
        return decision;
    }
    // 情形 4：规则已标定（当前不可达，因为阈值来源未确认）。
    // 保留位置并注明原因，避免日后误以为漏写。
    decision.basis = "规则已标定但阈值来源仍未确认（绝对量还是变化量，任务书 R03）";
    return decision;
}

std::string OrePresenceJudge::prerequisites() {
    return "绝对距离还是距离变化尚未明确（任务书 R03）；"
           "3-5 cm 是暂定范围，不作为阈值填入（范围报告 §3.8.2）";
}

}  // namespace business::services
