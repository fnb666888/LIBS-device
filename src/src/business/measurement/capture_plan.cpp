#include "libs/business/measurement/capture_plan.hpp"
#include <cmath>
#include "libs/measurement_limits.hpp"

namespace business::measurement {
libs::VoidResult validate_capture_rate(unsigned hz, const SpectrumMeasurement& m) {
    if (!libs::measurement_limits::accepts("laser", "frequency_hz", hz) ||
        !libs::measurement_limits::accepts("spectrometer", "integration_ms", m.integration_ms) ||
        !libs::measurement_limits::accepts("spectrometer", "integration_delay_ns", m.integration_delay_ns) ||
        !libs::measurement_limits::accepts("spectrometer", "capture_timeout_ms", m.timeout_ms))
        return libs::Error::make(libs::ErrorKind::invalid_argument, "capture_parameters_invalid", "采集参数无效");
    if (m.integration_ms + m.integration_delay_ns / 1.0e6 >= 1000.0 / hz)
        return libs::Error::make(libs::ErrorKind::invalid_argument,
            "laser_spectrometer_rate_mismatch", "积分延时与积分时间超过激光周期");
    return libs::VoidResult();
}


libs::devices::CaptureRequest build_capture_request(const config::SiteConfig& site,
                                                     const SpectrumMeasurement& measurement) {
    libs::devices::CaptureRequest request{measurement.integration_ms, measurement.timeout_ms,
                                         measurement.integration_delay_ns, true};
    // 装机配置给**接线与通道**：接了哪几台、主从、SYNC。
    request.serials = site.spectrometer.serials;
    request.master_serial = site.spectrometer.master_serial;
    request.synchronized = site.spectrometer.synchronized;
    // 测量参数给**测量本身**：积分时间、超时、扫描次数、积分延时。
    request.scans = measurement.scans;
    // **出光流程下 wait_for_laser_trigger 必然为 true**：触发源就是激光器，
    // 光谱仪只能从 pin 6 等它的 TTL（AvaSoft 手册 §4.9 接法二）。
    // 这是接法决定的物理事实，不是可调项 —— 配置没填也必须等。
    return request;
}

}  // namespace business::measurement
