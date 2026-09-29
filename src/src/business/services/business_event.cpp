#include "libs/business/services/business_event.hpp"

namespace business::events {

const char* to_string(DetectionStage stage) noexcept {
    switch (stage) {
        case DetectionStage::idle:         return "idle";
        case DetectionStage::selected_belt: return "selected_belt";
        case DetectionStage::approaching:  return "approaching";
        case DetectionStage::measuring:    return "measuring";
        case DetectionStage::evaluating:   return "evaluating";
        case DetectionStage::reporting:    return "reporting";
        case DetectionStage::done:         return "done";
    }
    return "unknown";
}

const char* to_string(OrePresenceState state) noexcept {
    switch (state) {
        case OrePresenceState::absent:  return "absent";
        case OrePresenceState::present: return "present";
        case OrePresenceState::unknown: return "unknown";
    }
    return "unknown";
}

json::Value to_json(const DetectionStartRequested& event) {
    json::Value v = json::object();
    v.set("target_belt", json::Value(event.target_belt));
    v.set("stage", json::Value(to_string(event.stage)));
    v.set("requested_by", json::Value(event.requested_by));
    v.set("wall_clock_ms", json::Value(static_cast<double>(event.wall_clock_ms)));
    if (!event.attributes.empty()) {
        json::Value attrs = json::object();
        for (const auto& kv : event.attributes) attrs.set(kv.first, json::Value(kv.second));
        v.set("attributes", std::move(attrs));
    }
    return v;
}

json::Value to_json(const TargetBeltChanged& event) {
    json::Value v = json::object();
    v.set("belt", json::Value(event.belt));
    if (event.previous_belt.empty()) {
        v.set("previous_belt", json::Value());
    } else {
        v.set("previous_belt", json::Value(event.previous_belt));
    }
    v.set("wall_clock_ms", json::Value(static_cast<double>(event.wall_clock_ms)));
    v.set("raw_message", json::Value(event.raw_message));
    return v;
}

json::Value to_json(const OrePresent& event) {
    json::Value v = json::object();
    v.set("state", json::Value(to_string(event.state)));
    v.set("source", json::Value(event.source));
    v.set("basis", json::Value(event.basis));
    v.set("wall_clock_ms", json::Value(static_cast<double>(event.wall_clock_ms)));
    return v;
}

json::Value to_json(const EventEnvelope& envelope) {
    json::Value v = json::object();
    v.set("name", json::Value(envelope.name));
    v.set("wall_clock_ms", json::Value(static_cast<double>(envelope.wall_clock_ms)));
    v.set("sequence", json::Value(static_cast<double>(envelope.sequence)));
    v.set("payload", envelope.payload);
    return v;
}

libs::VoidResult UnavailableGatewayAdapter::poll(std::vector<EventEnvelope>& out) {
    out.clear();
    return libs::Error::make(
        libs::ErrorKind::unsupported, "gateway_transport_unavailable",
        "网关接入尚未实现：src 内无网络层，网关协议、报文格式与接收地址均未确认"
        "（范围报告 §3.5.2 卡点 G1/G2）。需要：① 上传协议与载荷格式 "
        "② 接收地址与端口 ③ 交付确认层级 ④ IPC->网关 与 网关->海关 的确认区分");
}

libs::Result<bool> UnspecifiedFeedSegmentRules::in_feed_segment() const {
    return libs::Error::make(
        libs::ErrorKind::unsupported, "feed_segment_rules_unspecified",
        "料流段划分规则未定义（范围报告 §3.5.3，C 类）。需要："
        "① 料流段如何划分（按时间？按带号？按总量？）② 换带触发条件 "
        "③ 料流中断时暂停还是继续。不得用固定时长切段顶替");
}

libs::Result<bool> UnspecifiedFeedSegmentRules::should_change_belt() const {
    return libs::Error::make(
        libs::ErrorKind::unsupported, "belt_change_rule_unspecified",
        "换带触发条件未定义（范围报告 §3.5.3，C 类）。需要："
        "① 料流段如何划分 ② 换带触发条件 ③ 料流中断时暂停还是继续");
}

}  // namespace business::events
