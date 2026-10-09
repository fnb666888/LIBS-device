#include "libs/business/app/parameter_service.hpp"

#include <cmath>

#include "libs/business/app/runtime.hpp"
#include "libs/business/app/measurement_defaults.hpp"
#include "libs/business/measurement/frame.hpp"
#include "libs/measurement_limits.hpp"

namespace business::app {
namespace {

// 参数库里的字段名 -> LaserFireConfig 成员。**名字必须与配置结构对齐**，
// 否则参数库里存的是 "discharge_current"、出光读的是 "discharge_current_a"，
// 又变成两份来源，界面调了不生效。
constexpr const char* kFieldDischargeCurrent = "discharge_current_a";
constexpr const char* kFieldPulseWidth      = "pulse_width_us";
constexpr const char* kFieldFrequency       = "frequency_hz";
constexpr const char* kFieldDischargeDelay  = "discharge_delay_us";
constexpr const char* kFieldQDelay          = "q_delay_us";
constexpr const char* kFieldQWarmup         = "q_warmup_s";
constexpr const char* kFieldSettleTimeout   = "settle_timeout_ms";
constexpr const char* kFieldEmissionHold    = "emission_hold_ms";
constexpr const char* kFieldMinDischarges   = "min_discharges";

constexpr const char* kFieldIntegrationMs     = "integration_ms";
constexpr const char* kFieldCaptureTimeoutMs = "capture_timeout_ms";
constexpr const char* kFieldIntegrationDelay = "integration_delay_ns";

// 本机默认表集中在 measurement_defaults.hpp；驱动和测量 DTO 不选择工作点。
libs::Error bad(const char* code, const std::string& message) {
    return libs::Error::make(libs::ErrorKind::invalid_argument, code, message);
}

// 从参数库取一个非负整数字段。字段缺失时**不改 out**，返回 false。
//
// 入参是 `map<string,double>`（ParameterStore::query 的出参），不是 ParameterGroup ——
// 拿 Group 当入参会让"字段是否存在"的判断和实际读到的 map 走两条路。
bool take_uint(const std::map<std::string, double>& values, const char* field, unsigned* out) {
    const auto it = values.find(field);
    if (it == values.end()) return false;
    if (!libs::measurement_limits::accepts("laser", field, it->second)) return false;
    *out = static_cast<unsigned>(it->second);
    return true;
}

bool take_double(const std::map<std::string, double>& values, const char* field, double* out) {
    const auto it = values.find(field);
    if (it == values.end()) return false;
    *out = it->second;
    return true;
}

}  // namespace

std::vector<std::string> ParameterService::required_groups() {
    return {"spectrometer", "laser"};
}

std::map<std::string, std::vector<std::string>> ParameterService::required_fields() {
    return {
        {"laser", {kFieldDischargeCurrent, kFieldPulseWidth, kFieldFrequency,
                   kFieldDischargeDelay, kFieldQDelay, kFieldQWarmup,
                   kFieldSettleTimeout, kFieldEmissionHold, kFieldMinDischarges}},
        {"spectrometer", {kFieldIntegrationMs, kFieldCaptureTimeoutMs, kFieldIntegrationDelay}},
    };
}

libs::VoidResult ParameterService::seed_defaults() {
    // 首次开机（或参数库里缺字段）时把**真机默认参数**补进参数库：
    //   * 激光侧 9 个：80 A / 200 us / 1 Hz / 0 us / 210 us / 0 s /
    //                  20000 ms / 3000 ms / 1 次
    //   * 光谱侧 3 个：1.05 ms / 10000 ms / 1280 ns
    // 全部取自 docs/real-device-parameters-20260929.md。
    //
    // 语义是"补缺失"，不是"覆盖"：操作员在参数管理页面标定过的值永远优先，
    // 重启不会把现场调好的参数打回默认。进程启动即可检测 —— 门禁的
    // parameters_incomplete / laser_not_commissioned 两条自然放行，
    // 不用等操作员先手工把 9 个框填一遍。
    //
    // 曾经叫 seed_from_site(site)，把 site.json 的 laser_fire 抄进参数库 ——
    // 不带 --config 启动就把一串 0 种了进去。现在默认值只来自真机记录，
    // 函数不再需要 site 入参。
    //
    // **曾经激光侧完全不播种**（"本机标定值，无通用默认"），结果是开箱状态下
    // 界面上 9 个框全是量程下限、门禁卡在未标定。既然已有 2026-09-29 真机
    // 记录作为本机默认，就一并播种；换机器时在参数页填新的真机值保存即可
    //（种子只补缺失字段，不会把新值打回）。
    //
    // 非 const：seed 是**写**操作（补缺失字段）。Runtime::parameters() 返回
    // 非 const 指针，这里拿非 const 指针才对。
    config::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) {
        return libs::Error::make(libs::ErrorKind::internal, "parameter_store_absent",
                                 "参数库尚未初始化（Runtime::start 未完成）");
    }

    // 先读现状：只补**缺失**的字段，绝不覆盖操作员已经调过的值。
    const std::map<std::string, config::ParameterGroup> current = store->all();

    // **immediately**：种子是开机时的默认值，不是"下次空闲生效"的暂存项。
    // 用 staged 会让刚出厂的机器缺参数、门禁卡在 parameters_incomplete。
    auto ensure = [&](const std::string& group, const std::map<std::string, double>& values) {
        std::map<std::string, double> missing;
        const auto it = current.find(group);
        for (const auto& kv : values) {
            const bool present = (it != current.end()) && it->second.has(kv.first);
            if (!present) missing.emplace(kv.first, kv.second);
        }
        if (missing.empty()) return libs::VoidResult();
        const config::ApplyResult r = store->apply(group, missing);
        if (!r.completed()) {
            return libs::VoidResult(libs::Error::make(
                libs::ErrorKind::internal, "parameter_seed_failed",
                "写入默认参数失败（" + group + "）：" + r.message));
        }
        return libs::VoidResult();
    };

    // 光谱侧先种：光谱仪能不能预览与激光器标定无关。
    const libs::VoidResult spec = ensure("spectrometer", {
        {kFieldIntegrationMs,     kRealDeviceDefaults.integration_ms},
        {kFieldCaptureTimeoutMs,  kRealDeviceDefaults.capture_timeout_ms},
        {kFieldIntegrationDelay,  kRealDeviceDefaults.integration_delay_ns},
    });
    if (!spec) return spec;

    // ── laser 组：9 个字段逐个补缺失 ─────────────────────────────────
    // 真机默认值来自 2026-09-29 记录。只补缺失字段，所以现场标定过的值不会被
    // 打回；"已标定"仍由 commissioned_from_store() 按参数库实际内容判定。
    const libs::VoidResult laser = ensure("laser", {
        {kFieldDischargeCurrent, kRealDeviceDefaults.discharge_current_a},
        {kFieldPulseWidth,       kRealDeviceDefaults.pulse_width_us},
        {kFieldFrequency,        kRealDeviceDefaults.frequency_hz},
        {kFieldDischargeDelay,   kRealDeviceDefaults.discharge_delay_us},
        {kFieldQDelay,           kRealDeviceDefaults.q_delay_us},
        {kFieldQWarmup,          kRealDeviceDefaults.q_warmup_s},
        {kFieldSettleTimeout,    kRealDeviceDefaults.settle_timeout_ms},
        {kFieldEmissionHold,     kRealDeviceDefaults.emission_hold_ms},
        {kFieldMinDischarges,    kRealDeviceDefaults.min_discharges},
    });
    if (!laser) return laser;

    return libs::VoidResult();
}

libs::VoidResult ParameterService::collect_laser(measurement::FireParameters& out) const {
    const config::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) {
        return libs::Error::make(libs::ErrorKind::internal, "parameter_store_absent",
                                 "参数库尚未初始化（Runtime::start 未完成）");
    }
    std::map<std::string, double> values;
    const config::ApplyResult q = store->query("laser", values);
    if (q.state != config::ApplyState::completed) {
        return bad("parameter_group_missing", "laser 参数分组不存在");
    }
    // **只有参数库一个来源**。曾经这里是"基准 = 站点配置、参数库覆盖它"，
    // 那就是同一件事两个来源：操作员在参数页把电流改成 50 A，出光还是
    // site.json 里的 80 A。现在连基准都没有了 —— 参数库缺字段就是缺，
    // 直接报错，不拿装机配置兜底。
    config::LaserFireConfig cfg;
    if (!take_uint(values, kFieldDischargeCurrent, &cfg.discharge_current_a)) {
        return bad("parameter_field_bad", "laser.discharge_current_a 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldPulseWidth, &cfg.pulse_width_us)) {
        return bad("parameter_field_bad", "laser.pulse_width_us 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldFrequency, &cfg.frequency_hz)) {
        return bad("parameter_field_bad", "laser.frequency_hz 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldDischargeDelay, &cfg.discharge_delay_us)) {
        return bad("parameter_field_bad", "laser.discharge_delay_us 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldQDelay, &cfg.q_delay_us)) {
        return bad("parameter_field_bad", "laser.q_delay_us 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldQWarmup, &cfg.q_warmup_s)) {
        return bad("parameter_field_bad", "laser.q_warmup_s 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldSettleTimeout, &cfg.settle_timeout_ms)) {
        return bad("parameter_field_bad", "laser.settle_timeout_ms 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldEmissionHold, &cfg.emission_hold_ms)) {
        return bad("parameter_field_bad", "laser.emission_hold_ms 缺失或不是非负整数");
    }
    if (!take_uint(values, kFieldMinDischarges, &cfg.min_discharges)) {
        return bad("parameter_field_bad", "laser.min_discharges 缺失或不是非负整数");
    }
    // commissioned 由 commissioned_from_store() 单独判定后写进来，
    // 不在这里凭空置 true —— 那是门禁的判据，不是搬运工的判断。
    cfg.commissioned = commissioned_from_store();
    out.laser = cfg;
    return libs::VoidResult();
}

libs::VoidResult ParameterService::collect_spectrometer(measurement::FireParameters& out) const {
    const config::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) {
        return libs::Error::make(libs::ErrorKind::internal, "parameter_store_absent",
                                 "参数库尚未初始化（Runtime::start 未完成）");
    }
    std::map<std::string, double> values;
    const config::ApplyResult q = store->query("spectrometer", values);
    if (q.state != config::ApplyState::completed) {
        return bad("parameter_group_missing", "spectrometer 参数分组不存在");
    }
    if (!take_double(values, kFieldIntegrationMs, &out.integration_ms)) {
        return bad("parameter_field_bad", "spectrometer.integration_ms 缺失");
    }
    double timeout = kRealDeviceDefaults.capture_timeout_ms;
    (void)take_double(values, kFieldCaptureTimeoutMs, &timeout);
    if (!libs::measurement_limits::accepts("spectrometer", kFieldCaptureTimeoutMs, timeout))
        return bad("parameter_field_bad", "spectrometer.capture_timeout_ms 超出允许范围或不是整数");
    out.capture_timeout_ms = static_cast<unsigned>(timeout);
    out.integration_delay_ns = kRealDeviceDefaults.integration_delay_ns;
    (void)take_double(values, kFieldIntegrationDelay, &out.integration_delay_ns);
    if (!libs::measurement_limits::accepts("spectrometer", kFieldIntegrationMs, out.integration_ms) ||
        !libs::measurement_limits::accepts("spectrometer", kFieldIntegrationDelay, out.integration_delay_ns))
        return bad("parameter_field_bad", "spectrometer 积分时间或积分延时超出允许范围");
    return libs::VoidResult();
}

bool ParameterService::commissioned_from_store() const {
    // "已标定"不再由配置文件里的一个 bool 决定，而是**按参数库实际内容算**：
    // laser 组 9 个字段齐备，且每一个都落在协议量程内。
    //
    // 这样"填齐了参数"和"允许出光"就是同一件事，界面上的参数管理页面
    // 成了唯一的标定入口：操作员把 9 个框填成合法值保存，系统即视为已标定。
    // 反过来，只要有一个框缺失或越界，就仍是未标定 —— 一条写指令都不发。
    const config::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) return false;
    std::map<std::string, double> values;
    const config::ApplyResult q = store->query("laser", values);
    if (q.state != config::ApplyState::completed) return false;

    config::LaserFireConfig cfg;
    if (!take_uint(values, kFieldDischargeCurrent, &cfg.discharge_current_a)) return false;
    if (!take_uint(values, kFieldPulseWidth, &cfg.pulse_width_us)) return false;
    if (!take_uint(values, kFieldFrequency, &cfg.frequency_hz)) return false;
    if (!take_uint(values, kFieldDischargeDelay, &cfg.discharge_delay_us)) return false;
    if (!take_uint(values, kFieldQDelay, &cfg.q_delay_us)) return false;
    if (!take_uint(values, kFieldQWarmup, &cfg.q_warmup_s)) return false;
    if (!take_uint(values, kFieldSettleTimeout, &cfg.settle_timeout_ms)) return false;
    if (!take_uint(values, kFieldEmissionHold, &cfg.emission_hold_ms)) return false;
    if (!take_uint(values, kFieldMinDischarges, &cfg.min_discharges)) return false;

    cfg.commissioned = true;
    // 量程判据只有一个来源：与出光前校验、出光时校验走同一个函数。
    // 曾经这里自己写一套范围判断，于是"门禁说已标定、真正出光时被拒"。
    return static_cast<bool>(config::validate_laser_fire(cfg));
}

libs::Result<measurement::FireParameters> ParameterService::fire_parameters(const json::Value& args) const {
    measurement::FireParameters out;
    const libs::VoidResult laser = collect_laser(out);
    if (!laser) return laser.error();
    const libs::VoidResult spec = collect_spectrometer(out);
    if (!spec) return spec.error();

    if (!spec) return spec.error();

    // ── 本次覆盖 ────────────────────────────────────────────────────────────
    // 覆盖只作用于这一次调用，不写回参数库。需要长期生效的改动走 param_set。
    if (args.has("laser")) {
        const json::Value& o = args["laser"];
        if (!o.is_object()) {
            return bad("laser_overrides_bad", "args.laser 必须是对象");
        }
        const auto read_u = [&](const char* key, unsigned* dst) {
            if (!o.has(key)) return true;
            if (!o[key].is_number()) return false;
            const double v = o[key].as_number();
            if (!libs::measurement_limits::accepts("laser", key, v)) return false;
            *dst = static_cast<unsigned>(v);
            return true;
        };
        const bool shape_ok =
            read_u(kFieldDischargeCurrent, &out.laser.discharge_current_a) &&
            read_u(kFieldPulseWidth,      &out.laser.pulse_width_us) &&
            read_u(kFieldFrequency,       &out.laser.frequency_hz) &&
            read_u(kFieldDischargeDelay,  &out.laser.discharge_delay_us) &&
            read_u(kFieldQDelay,          &out.laser.q_delay_us) &&
            read_u(kFieldQWarmup,         &out.laser.q_warmup_s) &&
            read_u(kFieldSettleTimeout,   &out.laser.settle_timeout_ms) &&
            read_u(kFieldEmissionHold,    &out.laser.emission_hold_ms) &&
            read_u(kFieldMinDischarges,   &out.laser.min_discharges);
        if (!shape_ok) {
            return bad("laser_overrides_bad", "args.laser 的字段必须是 0 或正整数");
        }
    }

    if (args.has("spectrometer")) {
        const json::Value& o = args["spectrometer"];
        if (!o.is_object()) {
            return bad("spectrometer_overrides_bad", "args.spectrometer 必须是对象");
        }
        if (o.has(kFieldIntegrationMs)) {
            if (!o[kFieldIntegrationMs].is_number()) {
                return bad("spectrometer_overrides_bad", "spectrometer.integration_ms 必须是数字");
            }
            // 与驱动硬校验一致（spectrometer.cpp: [0.002, 60000] ms）。
            const double v = o[kFieldIntegrationMs].as_number();
            if (!libs::measurement_limits::accepts("spectrometer", kFieldIntegrationMs, v)) {
                return bad("spectrometer_overrides_bad",
                           "spectrometer.integration_ms 必须在 0.002~60000 ms 之间");
            }
            out.integration_ms = v;
        }
        if (o.has(kFieldCaptureTimeoutMs)) {
            if (!o[kFieldCaptureTimeoutMs].is_number()) {
                return bad("spectrometer_overrides_bad", "spectrometer.capture_timeout_ms 必须是数字");
            }
            const double v = o[kFieldCaptureTimeoutMs].as_number();
            if (!libs::measurement_limits::accepts("spectrometer", kFieldCaptureTimeoutMs, v)) {
                return bad("spectrometer_overrides_bad",
                           "spectrometer.capture_timeout_ms 必须是 1~600000 的整数毫秒");
            }
            out.capture_timeout_ms = static_cast<unsigned>(v);
        }
        if (o.has(kFieldIntegrationDelay)) {
            if (!o[kFieldIntegrationDelay].is_number()) {
                return bad("spectrometer_overrides_bad", "spectrometer.integration_delay_ns 必须是数字");
            }
            const double v = o[kFieldIntegrationDelay].as_number();
            if (!libs::measurement_limits::accepts("spectrometer", kFieldIntegrationDelay, v)) {
                return bad("spectrometer_overrides_bad", "spectrometer.integration_delay_ns 必须在 0~1000000 ns 之间");
            }
            out.integration_delay_ns = v;
        }
    }

    // ── 量程校验：与站点配置共用同一个函数 ──────────────────────────────────
    //
    // 不接受第二套规则。"界面限 100 A、请求能传 500 A"这种破口，
    // 在两处各校验一次就等于没校验。
    if (!out.laser.commissioned) {
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_commissioned",
                                 "出光参数尚未标定（commissioned=false），不能出光");
    }
    const libs::VoidResult lf = config::validate_laser_fire(out.laser);
    if (!lf) return lf.error();

    if (args.has("spectrometer")) {
        const auto& spec = args["spectrometer"];
        out.override_integration = spec.has("integration_ms");
        out.override_timeout = spec.has("capture_timeout_ms");
        out.override_delay = spec.has("integration_delay_ns");
    }
    return libs::Result<measurement::FireParameters>::success(out);
}

double ParameterService::value_or(const std::string& group, const std::string& field,
                                double fallback) const {
    const libs::Result<double> r = get(group, field);
    return r ? r.value() : fallback;
}

measurement::SpectrumMeasurement ParameterService::spectrometer_measurement() const {
    measurement::SpectrumMeasurement m;
    m.integration_ms = value_or("spectrometer", kFieldIntegrationMs, kRealDeviceDefaults.integration_ms);
    const double timeout = value_or("spectrometer", kFieldCaptureTimeoutMs, kRealDeviceDefaults.capture_timeout_ms);
    // 历史参数库可能含坏值：转换为 unsigned 前检查，拒绝时保留未配置哨兵 0。
    m.timeout_ms = libs::measurement_limits::accepts("spectrometer", kFieldCaptureTimeoutMs, timeout)
        ? static_cast<unsigned>(timeout) : 0;
    m.integration_delay_ns =
        value_or("spectrometer", kFieldIntegrationDelay, kRealDeviceDefaults.integration_delay_ns);
    return m;
}

config::ReadinessReport ParameterService::readiness() const {
    const config::ParameterStore* store = runtime_.parameters();
    return store ? store->readiness() : config::ReadinessReport();
}

libs::Result<double> ParameterService::get(const std::string& group,
                                            const std::string& field) const {
    std::map<std::string, double> values;
    const config::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) return bad("parameter_store_absent", "参数库尚未初始化");
    const config::ApplyResult q = store->query(group, values);
    if (q.state != config::ApplyState::completed) {
        return bad("parameter_group_missing", "参数分组不存在：" + group);
    }
    const auto it = values.find(field);
    if (it == values.end()) {
        return bad("parameter_field_missing", "参数字段不存在：" + group + "." + field);
    }
    return libs::Result<double>::success(it->second);
}

}  // namespace business::services
