#include "libs/business/services/capture.hpp"

namespace business::services {

CaptureCoordinator::CaptureCoordinator(const config::SiteConfig& site) : site_(site) {}

CapturePace CaptureCoordinator::pace_from_frequency(const WorkFrequency& frequency,
                                                     const config::SiteConfig& site) {
    CapturePace pace;
    pace.scans = 1;
    // 频率缺失或未标定 -> 未配置。**不猜、不回落、不用默认 Hz。**
    if (!frequency.configured() || !frequency.calibrated) {
        pace.configured = false;
        pace.source = "unconfigured";
        pace.note = "工作频率缺失或未标定，采集节奏不推导（范围报告 §3.10.2：不猜）";
        return pace;
    }
    pace.configured = true;
    pace.source = "work_frequency";
    // 单位未确认前不做换算：把频率原值当 ms 的换算等于猜一个系数。
    if (!frequency.unit_confirmed) {
        pace.period_ms = *frequency.frequency_hz;
        pace.timeout_ms = site.capture_timeout_ms;
        pace.note = "频率单位未确认，period_ms 暂以频率原值占位（unit_confirmed=false）";
        return pace;
    }
    if (*frequency.frequency_hz <= 0.0) {
        pace.configured = false;
        pace.source = "unconfigured";
        pace.note = "频率非正数，视为未配置";
        return pace;
    }
    pace.period_ms = 1000.0 / *frequency.frequency_hz;
    pace.timeout_ms = site.capture_timeout_ms;
    pace.note = "由工作频率映射得到周期";
    return pace;
}

CapturePace CaptureCoordinator::pace_from_config(const config::SiteConfig& site) {
    CapturePace pace;
    pace.configured = true;
    pace.scans = 1;
    pace.period_ms = static_cast<double>(site.capture_timeout_ms);
    pace.timeout_ms = site.capture_timeout_ms;
    pace.source = "config";
    pace.note = "由站点配置的采集超时与积分时间直接给出（不经工作频率）";
    return pace;
}

libs::Result<libs::devices::CaptureRequest> CaptureCoordinator::build_request(const CapturePace& pace) const {
    if (!pace.configured) {
        return libs::Error::make(libs::ErrorKind::invalid_argument, "pace_unconfigured",
                                 "采集节奏未配置，拒绝构造采集请求（范围报告 §3.10.2：频率缺失时不猜）");
    }
    libs::devices::CaptureRequest request;
    request.serials = site_.spectrometer.serials;
    request.master_serial = site_.spectrometer.master_serial;
    request.synchronized = site_.spectrometer.synchronized;
    request.integration_ms = pace.integration_ms.has_value()
                                 ? *pace.integration_ms
                                 : static_cast<double>(site_.integration_ms);
    request.timeout_ms = pace.timeout_ms.has_value() ? *pace.timeout_ms : site_.capture_timeout_ms;
    request.scans = pace.scans;
    // ── 激光触发光谱仪（AvaSoft 手册 §4.9 接法二）──────────────────────────
    // true = 光谱仪进外触发模式，从 pin 6 等激光器 TTL，收到后再等
    // integration_delay_ns 才开始积分。纳秒由驱动按 SDK 公式换算成 FPGA 计数。
    // 这两个值来自站点配置，业务层不猜、不设兜底默认值。
    request.wait_for_laser_trigger = site_.spectrometer.wait_for_laser_trigger;
    request.integration_delay_ns = site_.spectrometer.integration_delay_ns;
    return request;
}

libs::Result<libs::devices::CaptureResult> CaptureCoordinator::capture(libs::devices::Spectrometer* device,
                                                                       const CapturePace& pace) const {
    if (device == nullptr) {
        return libs::Error::make(libs::ErrorKind::unsupported, "spectrometer_not_open",
                                 "光谱仪未打开或未启用，无法采集");
    }
    const libs::Result<libs::devices::CaptureRequest> request = build_request(pace);
    if (!request) return request.error();
    // 驱动自己处理有界多次采集；hardware_synchronized 由 SDK 判定，业务层不干预。
    return device->capture(request.value());
}

libs::Result<libs::devices::CaptureResult> CaptureCoordinator::capture(libs::devices::Spectrometer* device,
                                                                      const CapturePace& pace,
                                                                      bool wait_for_laser_trigger,
                                                                      double integration_delay_ns) const {
    if (device == nullptr) {
        return libs::Error::make(libs::ErrorKind::unsupported, "spectrometer_not_open",
                                 "光谱仪未打开或未启用，无法采集");
    }
    libs::Result<libs::devices::CaptureRequest> request = build_request(pace);
    if (!request) return request.error();
    // 只覆盖"等不等激光触发"与"延时多少"这两个触发相关字段，
    // 其余（通道、积分时间、扫描次数）仍由配置给出。
    request.value().wait_for_laser_trigger = wait_for_laser_trigger;
    request.value().integration_delay_ns = integration_delay_ns;
    return device->capture(request.value());
}

CaptureCoordinator::SyncView CaptureCoordinator::sync_view(const config::SiteConfig& site,
                                                           const libs::devices::ScanGroup& group) {
    SyncView view;
    view.hardware_synchronized = group.hardware_synchronized;   // 原样上送
    view.requested_sync = site.spectrometer.synchronized;
    view.arm_order = group.arm_order;
    view.complete = group.complete;
    return view;
}

json::Value CaptureCoordinator::to_payload(const std::vector<libs::devices::ScanGroup>& groups) {
    json::Value root = json::object();
    json::Value scan_groups = json::array();
    for (const libs::devices::ScanGroup& group : groups) {
        json::Value g = json::object();
        g.set("shot_id", json::Value(static_cast<double>(group.shot_id)));
        g.set("complete", json::Value(group.complete));
        // 原样上送，不推断：SDK 报什么就是什么。
        g.set("hardware_synchronized", json::Value(group.hardware_synchronized));
        json::Value order = json::array();
        for (const std::string& serial : group.arm_order) order.push_back(json::Value(serial));
        g.set("arm_order", std::move(order));

        json::Value channels = json::array();
        for (const libs::devices::Spectrum& spectrum : group.spectra) {
            json::Value c = json::object();
            c.set("serial", json::Value(spectrum.serial));
            c.set("pixels", json::Value(static_cast<double>(spectrum.pixels)));
            c.set("device_time_ticks", json::Value(static_cast<double>(spectrum.device_time_ticks)));
            json::Value wavelength = json::array();
            for (double nm : spectrum.wavelength_nm) wavelength.push_back(json::Value(nm));
            c.set("wavelength_nm", std::move(wavelength));
            json::Value counts = json::array();
            for (double v : spectrum.counts) counts.push_back(json::Value(v));
            c.set("counts", std::move(counts));
            channels.push_back(std::move(c));
        }
        // 缺失通道不占位：channels 里只有真的采到的通道。
        g.set("channels", std::move(channels));
        scan_groups.push_back(std::move(g));
    }
    root.set("scan_groups", std::move(scan_groups));
    return root;
}

}  // namespace business::services
