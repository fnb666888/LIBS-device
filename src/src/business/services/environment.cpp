#include "libs/business/services/environment.hpp"

#include "libs/devices/air_conditioner.hpp"

namespace business::services {

const char* to_string(AcAvailability availability) noexcept {
    switch (availability) {
        case AcAvailability::available:   return "available";
        case AcAvailability::stale:       return "stale";
        case AcAvailability::unavailable: return "unavailable";
    }
    return "unavailable";
}

json::Value to_json(const AcSnapshot& snapshot) {
    json::Value v = json::object();
    v.set("availability", json::Value(to_string(snapshot.availability)));
    v.set("wall_clock_ms", json::Value(static_cast<double>(snapshot.wall_clock_ms)));
    if (!snapshot.read_error.empty()) v.set("read_error", json::Value(snapshot.read_error));

    // 工程量用 null 表示"本次没有"，不是 0。
    const auto set_opt = [&v](const char* key, const std::optional<double>& value) {
        if (value.has_value()) v.set(key, json::Value(*value));
        else v.set(key, json::Value());
    };
    set_opt("indoor_temp_c", snapshot.indoor_temp_c);
    set_opt("evaporator_temp_c", snapshot.evaporator_temp_c);
    set_opt("condenser_temp_c", snapshot.condenser_temp_c);
    set_opt("humidity_rh", snapshot.humidity_rh);

    if (snapshot.mode.has_value()) v.set("mode", json::Value(libs::air_conditioner::to_string(*snapshot.mode)));
    else v.set("mode", json::Value());

    json::Array bits;
    for (const auto& bit : snapshot.bits) {
        json::Value b = json::object();
        b.set("address", json::Value(static_cast<double>(bit.address)));
        b.set("name", json::Value(bit.name));
        b.set("active", json::Value(bit.active));
        b.set("is_alarm", json::Value(bit.is_alarm));
        bits.push_back(std::move(b));
    }
    v.set("bits", json::Value(std::move(bits)));

    json::Array alarms;
    for (const auto& bit : snapshot.active_alarms) {
        json::Value b = json::object();
        b.set("address", json::Value(static_cast<double>(bit.address)));
        b.set("name", json::Value(bit.name));
        alarms.push_back(std::move(b));
    }
    v.set("active_alarms", json::Value(std::move(alarms)));
    v.set("params_read", json::Value(snapshot.params_read));
    v.set("note", json::Value("availability != available 时各工程量为 null；"
                              "通信失败绝不表达为“正常”（任务书 §6.1）"));
    return v;
}

AirConditionerMonitor::AirConditionerMonitor(std::uint8_t address, unsigned stale_after_ms,
                                             unsigned keepalive_ms)
    : address_(address), stale_after_ms_(stale_after_ms), keepalive_ms_(keepalive_ms) {}

void AirConditionerMonitor::hold_mode(libs::air_conditioner::Mode mode) {
    held_ = mode;
    last_mode_sent_ = 0;   // 还没发过，keepalive 会立刻补一次
}

AcSnapshot AirConditionerMonitor::poll(libs::devices::AirConditioner* device) {
    AcSnapshot snapshot;
    snapshot.wall_clock_ms = wall_clock_ms();

    if (device == nullptr) {
        snapshot.availability = AcAvailability::unavailable;
        snapshot.read_error = "空调未打开或未启用；这不是“正常”";
        if (last_.wall_clock_ms != 0 &&
            snapshot.wall_clock_ms - last_good_read_ms_ <= static_cast<Milliseconds>(stale_after_ms_)) {
            snapshot.availability = AcAvailability::stale;
            snapshot.bits = last_.bits;
            snapshot.active_alarms = last_.active_alarms;
        }
        return snapshot;
    }

    const libs::Result<libs::devices::StatusRead> status = device->read_status(address_);
    if (!status) {
        snapshot.read_error = status.error().message;
        const bool have_recent = last_.wall_clock_ms != 0 &&
            snapshot.wall_clock_ms - last_good_read_ms_ <= static_cast<Milliseconds>(stale_after_ms_);
        if (have_recent) {
            // 有过期但可参考的读数：如实标 stale，并明确说明数据不是本次的。
            snapshot.availability = AcAvailability::stale;
            snapshot.bits = last_.bits;
            snapshot.active_alarms = last_.active_alarms;
        } else {
            snapshot.availability = AcAvailability::unavailable;
        }
        return snapshot;
    }

    snapshot.availability = AcAvailability::available;
    snapshot.bits = status.value().reading.bits;
    for (const auto& bit : snapshot.bits) {
        if (bit.is_alarm && bit.active) snapshot.active_alarms.push_back(bit);
    }

    const libs::Result<libs::devices::ParamRead> params = device->read_params(address_);
    if (params) {
        snapshot.params_read = true;
        for (const auto& reg : params.value().reading.registers) {
            if (reg.address == libs::air_conditioner::reg::kIndoorTemp) snapshot.indoor_temp_c = reg.value;
            else if (reg.address == libs::air_conditioner::reg::kEvaporatorTemp) snapshot.evaporator_temp_c = reg.value;
            else if (reg.address == libs::air_conditioner::reg::kCondenserTemp) snapshot.condenser_temp_c = reg.value;
            else if (reg.address == libs::air_conditioner::reg::kHumidity) snapshot.humidity_rh = reg.value;
            else if (reg.address == libs::air_conditioner::reg::kMode) {
                snapshot.mode = static_cast<libs::air_conditioner::Mode>(reg.raw);
            }
        }
    } else {
        snapshot.read_error = "状态可读但参数读取失败: " + params.error().message;
    }

    last_ = snapshot;
    last_good_read_ms_ = snapshot.wall_clock_ms;
    return snapshot;
}

libs::Result<bool> AirConditionerMonitor::keepalive(libs::devices::AirConditioner* device) {
    if (!held_.has_value()) return false;   // 没声明保持，不需要循环
    if (device == nullptr) {
        return libs::Error::make(libs::ErrorKind::unsupported, "ac_keepalive_no_device",
                                 "未声明保持模式或空调未打开，无法执行模式保持循环");
    }
    const Milliseconds now = monotonic_ms();
    if (last_mode_sent_ != 0 && now - last_mode_sent_ < static_cast<Milliseconds>(keepalive_ms_)) {
        return false;   // 还没到重发时刻
    }
    if (keepalive_ms_ >= static_cast<unsigned>(kDeviceKeepaliveLimitMs)) {
        // 自检：保持间隔若不小于设备上限，设备会自己回落到自动运行。
        return libs::Error::make(libs::ErrorKind::invalid_argument, "ac_keepalive_too_slow",
                                 "模式保持间隔必须显著小于设备上限 100 s（手册 §6.2）");
    }
    const libs::Result<libs::devices::ParamWrite> w = device->set_mode(address_, *held_);
    if (!w) return w.error();
    last_mode_sent_ = now;
    return true;
}

}  // namespace business::services


