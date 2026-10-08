#include "libs/business/measurement/frame.hpp"

#include <utility>

namespace business::measurement {

const char* to_string(ChannelState state) noexcept {
    switch (state) {
        case ChannelState::present: return "present";
        case ChannelState::absent:  return "absent";
    }
    return "absent";
}

json::Value to_json(const ChannelFrame& channel) {
    json::Value v = json::object();
    v.set("serial", json::Value(channel.serial));
    // state 是三态里的"状态"那一个，与 counts 的数值**不是一回事**。
    // 缺失时写 "absent"，不是写一个空的 counts 数组——后者会被 UI
    // 当成"这一路是平的"。
    v.set("state", json::Value(to_string(channel.state)));
    if (channel.state == ChannelState::absent) {
        v.set("reason", json::Value(channel.reason));
        return v;
    }
    v.set("pixels", json::Value(static_cast<double>(channel.pixels)));

    // 波长与 counts 是两个等长数组。原样上送，**不做归一化/基线扣除/平均**。
    // UI 要归一化自己算：把显示口径写死在业务层，改口径就得改库。
    json::Array wavelengths;
    json::Array counts;
    wavelengths.reserve(channel.wavelength_nm.size());
    counts.reserve(channel.counts.size());
    for (const double w : channel.wavelength_nm) wavelengths.push_back(json::Value(w));
    for (const double c : channel.counts) counts.push_back(json::Value(c));
    v.set("wavelength_nm", json::Value(std::move(wavelengths)));
    v.set("counts", json::Value(std::move(counts)));
    return v;
}

json::Value to_json(const SpectrumFrame& frame) {
    json::Value v = json::object();
    v.set("frame_index", json::Value(static_cast<double>(frame.frame_index)));
    v.set("shot_id", json::Value(static_cast<double>(frame.shot_id)));
    v.set("wall_clock_ms", json::Value(static_cast<double>(frame.wall_clock_ms)));
    v.set("captured_monotonic_ms", json::Value(static_cast<double>(frame.captured_monotonic_ms)));
    // 同步事实原样上送，不合并成一个布尔。
    v.set("hardware_synchronized", json::Value(frame.hardware_synchronized));
    v.set("requested_sync", json::Value(frame.requested_sync));
    // 本帧是否由激光触发采集而来。UI 靠它区分"测量谱"与"背景谱"。
    v.set("laser_shot", json::Value(frame.laser_shot));
    v.set("period_ms", json::Value(frame.period_ms));
    v.set("timeout_ms", json::Value(static_cast<double>(frame.timeout_ms)));
    v.set("ok", json::Value(frame.ok()));
    if (!frame.ok()) {
        v.set("capture_error_code", json::Value(frame.capture_error_code));
        v.set("capture_error_message", json::Value(frame.capture_error_message));
    }
    json::Array channels;
    for (const auto& channel : frame.channels) channels.push_back(to_json(channel));
    v.set("channels", json::Value(std::move(channels)));
    return v;
}

}  // namespace business::measurement