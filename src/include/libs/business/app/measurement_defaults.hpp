#pragma once
#include "libs/measurement_limits.hpp"

namespace business::app {

// 本机工作点的唯一默认表，来源：docs/real-device-parameters-20260929.md。
// 仅 ParameterService 使用它补缺失字段/读取回退；驱动不依赖本机工艺参数。
// 已保存的现场参数和本次请求的显式覆盖值优先。
struct MeasurementDefaults {
    double integration_ms;
    unsigned capture_timeout_ms;
    double integration_delay_ns;
    unsigned discharge_current_a;
    unsigned pulse_width_us;
    unsigned frequency_hz;
    unsigned discharge_delay_us;
    unsigned q_delay_us;
    unsigned q_warmup_s;
    unsigned settle_timeout_ms;
    unsigned emission_hold_ms;
    unsigned min_discharges;
};

inline constexpr MeasurementDefaults kRealDeviceDefaults{
    1.05, 10000, 1280.0,
    80, 200, 1, 0, 210, 0,
    20000, 3000, 1,
};

static_assert(
    libs::measurement_limits::accepts("spectrometer", "integration_ms", kRealDeviceDefaults.integration_ms) &&
    libs::measurement_limits::accepts("spectrometer", "capture_timeout_ms", kRealDeviceDefaults.capture_timeout_ms) &&
    libs::measurement_limits::accepts("spectrometer", "integration_delay_ns", kRealDeviceDefaults.integration_delay_ns) &&
    libs::measurement_limits::accepts("laser", "discharge_current_a", kRealDeviceDefaults.discharge_current_a) &&
    libs::measurement_limits::accepts("laser", "pulse_width_us", kRealDeviceDefaults.pulse_width_us) &&
    libs::measurement_limits::accepts("laser", "frequency_hz", kRealDeviceDefaults.frequency_hz) &&
    libs::measurement_limits::accepts("laser", "discharge_delay_us", kRealDeviceDefaults.discharge_delay_us) &&
    libs::measurement_limits::accepts("laser", "q_delay_us", kRealDeviceDefaults.q_delay_us) &&
    libs::measurement_limits::accepts("laser", "q_warmup_s", kRealDeviceDefaults.q_warmup_s) &&
    libs::measurement_limits::accepts("laser", "settle_timeout_ms", kRealDeviceDefaults.settle_timeout_ms) &&
    libs::measurement_limits::accepts("laser", "emission_hold_ms", kRealDeviceDefaults.emission_hold_ms) &&
    libs::measurement_limits::accepts("laser", "min_discharges", kRealDeviceDefaults.min_discharges),
    "real device defaults must stay within the declared parameter limits");

}  // namespace business::app
