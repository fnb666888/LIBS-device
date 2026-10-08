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

unsigned CaptureCoordinator::expected_discharges(const config::LaserFireConfig& fire) {
    // 光闸保持窗口里，激光按 frequency_hz 连续放电；窗口长度是 emission_hold_ms。
    // 所以窗口内实际会打 频率 x 保持秒数 发。
    const double discharges =
        (static_cast<double>(fire.frequency_hz) * static_cast<double>(fire.emission_hold_ms)) / 1000.0;
    // 至少 1 发：出光一旦真的发生，就不可能采 0 张谱。
    // 窗口短于一个周期时这里取 1，而不是 0 —— 0 会让"该采却没采"看起来像正常。
    if (discharges < 1.0) return 1u;
    return static_cast<unsigned>(discharges + 0.5);   // 四舍五入到最接近的发数
}

libs::VoidResult CaptureCoordinator::validate_scans(const config::LaserFireConfig& fire) {
    if (fire.scans == 0) return libs::VoidResult();   // 未配置：用派生值
    const unsigned expected = expected_discharges(fire);
    if (fire.scans != expected) {
        // 少配 = 多出来的放电 TTL 没有测量在等，被硬件无声丢弃（"N 发出一张谱"）；
        // 多配 = 后几发等不到触发，整次采集白等到超时。
        // 两种都必须当场拒绝，不能现场拿到一张"看起来正常"的少采谱。
        return libs::Error::make(
            libs::ErrorKind::invalid_argument, "laser_fire_scans_mismatch",
            "laser_fire.scans=" + std::to_string(fire.scans) +
                " 与出光窗口的放电发数 " + std::to_string(expected) +
                " 不一致（频率 " + std::to_string(fire.frequency_hz) + " Hz x 保持 " +
                std::to_string(fire.emission_hold_ms) + " ms）。" +
                "配少会静默少采，配多会等不到触发；请改成 0（按窗口自动）或与窗口一致的值");
    }
    return libs::VoidResult();
}

libs::VoidResult CaptureCoordinator::validate_rate_match(const config::LaserFireConfig& fire,
                                                        double integration_ms,
                                                        double integration_delay_ns) {
    if (fire.frequency_hz == 0) {
        return libs::Error::make(libs::ErrorKind::invalid_argument, "laser_frequency_unset",
                                 "激光频率为 0，无法校验与光谱仪的采集节奏是否同步");
    }
    const double period_ms = 1000.0 / static_cast<double>(fire.frequency_hz);
    const double occupied_ms = integration_ms + integration_delay_ns / 1.0e6;
    if (occupied_ms >= period_ms) {
        return libs::Error::make(
            libs::ErrorKind::invalid_argument, "laser_spectrometer_rate_mismatch",
            "光谱仪跟不上激光频率：一个周期只有 " + std::to_string(period_ms) +
                " ms，而「积分延时 + 积分」就要占 " + std::to_string(occupied_ms) +
                " ms。两者必须同步，否则会丢发/错发，采到的谱与放电发次对不上");
    }
    return libs::VoidResult();
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
                                                                      double integration_delay_ns,
                                                                      bool spectrometer_triggers_laser,
                                                                      std::uint32_t laser_delay_cycles,
                                                                      std::uint32_t laser_width_cycles) const {
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
    request.value().spectrometer_triggers_laser = spectrometer_triggers_laser;
    request.value().laser_delay_cycles = laser_delay_cycles;
    request.value().laser_width_cycles = laser_width_cycles;
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
    // 这条链路来自激光诱导击穿光谱仪。明确标记来源，防止下游把原始
    // wavelength/counts 误当成附件里的近红外十项成分或模型相似度结果。
    root.set("spectrum_source", json::Value("LIBS"));
    root.set("spectrum_format", json::Value("wavelength_nm+counts"));
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
