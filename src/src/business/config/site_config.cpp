#include "libs/business/config/site_config.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace business::config {
namespace {

using libs::Error;
using libs::ErrorKind;
using libs::Result;
using libs::VoidResult;

Error invalid(const std::string& message) {
    return Error::make(ErrorKind::invalid_argument, "bad_site_config", message);
}

// 读取一个必需字符串字段。缺失或类型不对都算非法，不给默认值。
Result<std::string> required_string(const json::Value& parent, const std::string& key,
                                    const std::string& where) {
    const json::Value& v = parent[key];
    if (v.is_null()) return invalid(where + "." + key + " is required");
    if (!v.is_string()) return invalid(where + "." + key + " must be a string");
    if (v.as_string().empty()) return invalid(where + "." + key + " must not be empty");
    return v.as_string();
}

Result<unsigned> required_uint(const json::Value& parent, const std::string& key,
                               const std::string& where) {
    const json::Value& v = parent[key];
    if (v.is_null()) return invalid(where + "." + key + " is required");
    if (!v.is_number()) return invalid(where + "." + key + " must be a number");
    const double d = v.as_number();
    if (d < 0 || d != std::floor(d)) return invalid(where + "." + key + " must be a non-negative integer");
    if (d > 65535) return invalid(where + "." + key + " is out of range");
    return static_cast<unsigned>(d);
}

Result<unsigned> optional_uint(const json::Value& parent, const std::string& key,
                                const std::string& where, unsigned fallback) {
    const json::Value& v = parent[key];
    if (v.is_null()) return fallback;
    return required_uint(parent, key, where);
}

Result<bool> optional_bool(const json::Value& parent, const std::string& key, bool fallback) {
    const json::Value& v = parent[key];
    if (v.is_null()) return fallback;
    if (!v.is_bool()) return invalid(key + " must be a boolean");
    return v.as_bool();
}

Result<double> optional_double(const json::Value& parent, const std::string& key, double fallback) {
    const json::Value& v = parent[key];
    if (v.is_null()) return fallback;
    if (!v.is_number()) return invalid(key + " must be a number");
    if (std::isnan(v.as_number()) || std::isinf(v.as_number()))
        return invalid(key + " must be a finite number");
    return v.as_number();
}

Result<std::uint8_t> optional_u8(const json::Value& parent, const std::string& key,
                                 std::uint8_t fallback) {
    const Result<unsigned> r = optional_uint(parent, key, "", 0);
    if (!r) return r.error();
    if (parent[key].is_null()) return fallback;
    if (r.value() > 255) return invalid(key + " must fit in one byte");
    return static_cast<std::uint8_t>(r.value());
}

Result<libs::transport::Parity> parse_parity(const json::Value& parent, const std::string& where) {
    const json::Value& v = parent["parity"];
    if (v.is_null()) return libs::transport::Parity::none;
    if (!v.is_string()) return invalid(where + ".parity must be a string");
    const std::string& s = v.as_string();
    if (s == "none") return libs::transport::Parity::none;
    if (s == "even") return libs::transport::Parity::even;
    if (s == "odd")  return libs::transport::Parity::odd;
    return invalid(where + ".parity must be one of none/even/odd");
}

Result<SerialPortConfig> parse_serial(const json::Value& parent, const std::string& key,
                                      const SerialPortConfig& defaults) {
    const json::Value& node = parent[key];
    SerialPortConfig cfg = defaults;
    if (node.is_null()) return invalid(key + " is required");

    if (!node.is_object()) return invalid(key + " must be an object");

    // 端口名可以省略：enabled=false 的设备允许没有端口。
    const json::Value& port = node["port"];
    if (!port.is_null()) {
        if (!port.is_string()) return invalid(key + ".port must be a string");
        cfg.port = port.as_string();
    }

    const Result<unsigned> baud = optional_uint(node, "baud", key, cfg.baud);
    if (!baud) return baud.error();
    cfg.baud = baud.value();

    const Result<libs::transport::Parity> parity = parse_parity(node, key);
    if (!parity) return parity.error();
    cfg.parity = parity.value();

    const Result<unsigned> bits = optional_uint(node, "data_bits", key, cfg.data_bits);
    if (!bits) return bits.error();
    cfg.data_bits = bits.value();

    const Result<unsigned> tmo = optional_uint(node, "timeout_ms", key, cfg.timeout_ms);
    if (!tmo) return tmo.error();
    cfg.timeout_ms = tmo.value();

    const Result<unsigned> idle = optional_uint(node, "idle_ms", key, cfg.idle_ms);
    if (!idle) return idle.error();
    cfg.idle_ms = idle.value();

    const Result<bool> en = optional_bool(node, "enabled", cfg.enabled);
    if (!en) return en.error();
    cfg.enabled = en.value();
    return cfg;
}

Result<SpectrometerConfig> parse_spectrometer(const json::Value& parent) {
    const json::Value& node = parent["spectrometer"];
    if (node.is_null()) return invalid("spectrometer is required");
    if (!node.is_object()) return invalid("spectrometer must be an object");

    SpectrometerConfig cfg;
    // 先读 enabled：未启用的设备不要求 SDK 路径，
    // 否则一份「设备全未接」的骨架配置就无法表达，
    // 而那正是 --print-default-config 必须能给出的那份文本。
    const Result<bool> en = optional_bool(node, "enabled", true);
    if (!en) return en.error();
    cfg.enabled = en.value();

    const json::Value& dll_node = node["sdk_dll"];
    if (dll_node.is_null()) {
        if (cfg.enabled) return invalid("spectrometer.sdk_dll is required when spectrometer is enabled");
    } else {
        if (!dll_node.is_string()) return invalid("spectrometer.sdk_dll must be a string");
        cfg.sdk_dll = dll_node.as_string();
    }

    const json::Value& serials = node["serials"];
    if (!serials.is_null()) {
        if (!serials.is_array()) return invalid("spectrometer.serials must be an array");
        for (const json::Value& item : serials.as_array()) {
            if (!item.is_string() || item.as_string().empty())
                return invalid("spectrometer.serials entries must be non-empty strings");
            cfg.serials.push_back(item.as_string());
        }
    }

    const json::Value& master = node["master_serial"];
    if (!master.is_null()) {
        if (!master.is_string()) return invalid("spectrometer.master_serial must be a string");
        cfg.master_serial = master.as_string();
    }

    const Result<bool> sync = optional_bool(node, "synchronized", false);
    if (!sync) return sync.error();
    cfg.synchronized = sync.value();

    // 激光触发光谱仪：等 pin 6 的外部 TTL，而不是自由触发。
    const Result<bool> wait = optional_bool(node, "wait_for_laser_trigger", false);
    if (!wait) return wait.error();
    cfg.wait_for_laser_trigger = wait.value();

    // 积分延时（纳秒）。必须是非负有限数：负延时只在"光谱仪发 TTL 点火"
    // 的接法下成立，本项目是反过来的（激光器点火），负延时没有意义。
    const json::Value& delay = node["integration_delay_ns"];
    if (!delay.is_null()) {
        if (!delay.is_number())
            return invalid("spectrometer.integration_delay_ns must be a number");
        const double v = delay.as_number();
        if (!std::isfinite(v) || v < 0.0)
            return invalid("spectrometer.integration_delay_ns must be a finite non-negative number of nanoseconds");
        cfg.integration_delay_ns = v;
    }

    return cfg;
}

Result<GatewayConfig> parse_gateway(const json::Value& parent) {
    GatewayConfig cfg;   // enabled 默认 false：无网络层（范围报告 §3.5.2 G2）
    const json::Value& node = parent["gateway"];
    if (node.is_null()) return cfg;
    if (!node.is_object()) return invalid("gateway must be an object");

    const auto str = [&](const char* key, std::string& out) -> const char* {
        const json::Value& v = node[key];
        if (v.is_null()) return nullptr;
        if (!v.is_string()) return "must be a string";
        out = v.as_string();
        return nullptr;
    };
    for (const auto& field : {std::make_pair("candidate_ip", &cfg.candidate_ip),
                             std::make_pair("candidate_adapter", &cfg.candidate_adapter),
                             std::make_pair("observed_ip", &cfg.observed_ip)}) {
        if (const char* problem = str(field.first, *field.second))
            return invalid(std::string("gateway.") + field.first + " " + problem);
    }

    const Result<unsigned> port = optional_uint(node, "port", "gateway", 0);
    if (!port) return port.error();
    cfg.port = port.value();

    const Result<bool> en = optional_bool(node, "enabled", false);
    if (!en) return en.error();
    cfg.enabled = en.value();

    const auto optional_string = [&](const char* key, std::string& out) -> VoidResult {
        const json::Value& v = node[key];
        if (v.is_null()) return VoidResult();
        if (!v.is_string()) return invalid(std::string("gateway.") + key + " must be a string");
        out = v.as_string();
        return VoidResult();
    };
    for (auto field : {std::pair<const char*, std::string*>("base_url", &cfg.base_url),
                       std::pair<const char*, std::string*>("product_id", &cfg.product_id),
                       std::pair<const char*, std::string*>("device_id", &cfg.device_id),
                       std::pair<const char*, std::string*>("token", &cfg.authorization_token),
                       std::pair<const char*, std::string*>("equipment_id", &cfg.equipment_id),
                       std::pair<const char*, std::string*>("equipment_type", &cfg.equipment_type),
                       std::pair<const char*, std::string*>("customs", &cfg.customs)}) {
        const VoidResult r = optional_string(field.first, *field.second);
        if (!r) return r.error();
    }
    const Result<unsigned> timeout = optional_uint(node, "timeout_ms", "gateway", cfg.timeout_ms);
    if (!timeout) return timeout.error();
    cfg.timeout_ms = timeout.value();
    return cfg;
}

Result<ConveyorConfig> parse_conveyor(const json::Value& parent) {
    ConveyorConfig cfg;
    const json::Value& node = parent["conveyor"];
    if (node.is_null()) return cfg;
    if (!node.is_object()) return invalid("conveyor must be an object");
    const Result<bool> ctl = optional_bool(node, "software_controlled", false);
    if (!ctl) return ctl.error();
    cfg.software_controlled = ctl.value();
    const Result<bool> run = optional_bool(node, "running_assumed", true);
    if (!run) return run.error();
    cfg.running_assumed = run.value();
    return cfg;
}

Result<AirConditionerInterlockConfig> parse_interlock(const json::Value& parent) {
    AirConditionerInterlockConfig cfg;   // enabled=false, calibrated=false
    const json::Value& node = parent["ac_interlock"];
    if (node.is_null()) return cfg;
    if (!node.is_object()) return invalid("ac_interlock must be an object");
    const Result<bool> en = optional_bool(node, "enabled", false);
    if (!en) return en.error();
    cfg.enabled = en.value();
    const Result<double> t = optional_double(node, "trigger_indoor_temp_c", 0.0);
    if (!t) return t.error();
    cfg.trigger_indoor_temp_c = t.value();
    const Result<bool> cal = optional_bool(node, "calibrated", false);
    if (!cal) return cal.error();
    cfg.calibrated = cal.value();
    // 未标定却启用了联锁：这是配置自相矛盾，必须拒绝而不是拿 0 当阈值。
    if (cfg.enabled && !cfg.calibrated)
        return invalid("ac_interlock.enabled requires calibrated=true; an uncalibrated threshold is a guess");
    return cfg;
}

Result<MotorMotionConfig> parse_motor_motion(const json::Value& parent) {
    MotorMotionConfig cfg;
    const json::Value& node = parent["motor_motion"];
    if (node.is_null()) return cfg;  // 旧配置兼容：未出现即未标定
    if (!node.is_object()) return invalid("motor_motion must be an object");

    const Result<bool> commissioned = optional_bool(node, "commissioned", cfg.commissioned);
    if (!commissioned) return commissioned.error();
    cfg.commissioned = commissioned.value();

    const Result<unsigned> max_frequency =
        optional_uint(node, "max_frequency_percent", "motor_motion", cfg.max_frequency_percent);
    if (!max_frequency) return max_frequency.error();
    cfg.max_frequency_percent = max_frequency.value();

    const Result<unsigned> max_duration =
        optional_uint(node, "max_duration_ms", "motor_motion", cfg.max_duration_ms);
    if (!max_duration) return max_duration.error();
    cfg.max_duration_ms = max_duration.value();

    const Result<unsigned> stop_timeout =
        optional_uint(node, "stop_timeout_ms", "motor_motion", cfg.stop_timeout_ms);
    if (!stop_timeout) return stop_timeout.error();
    cfg.stop_timeout_ms = stop_timeout.value();

    const Result<unsigned> poll_interval =
        optional_uint(node, "poll_interval_ms", "motor_motion", cfg.poll_interval_ms);
    if (!poll_interval) return poll_interval.error();
    cfg.poll_interval_ms = poll_interval.value();

    const Result<bool> watchdog = optional_bool(
        node, "communication_watchdog_confirmed", cfg.communication_watchdog_confirmed);
    if (!watchdog) return watchdog.error();
    cfg.communication_watchdog_confirmed = watchdog.value();
    return cfg;
}

// 出光标定参数。**所有字段都必须显式写出**，不给任何"聪明"的默认值：
// 这里每个数的默认值都意味着"能出光"，回落就等于替现场做了一次未标定的决定。
Result<LaserFireConfig> parse_laser_fire(const json::Value& parent) {
    LaserFireConfig cfg;                 // commissioned=false；会话参数为 0，外部时钟候选为 1
    const json::Value& node = parent["laser_fire"];
    if (node.is_null()) return cfg;      // 整段缺失 = 未标定，合法且安全
    if (!node.is_object()) return invalid("laser_fire must be an object");

    const Result<bool> cal = optional_bool(node, "commissioned", false);
    if (!cal) return cal.error();
    cfg.commissioned = cal.value();

    struct Field { const char* key; unsigned* out; };
    const Field fields[] = {
        {"discharge_current_a", &cfg.discharge_current_a},
        {"pulse_width_us",      &cfg.pulse_width_us},
        {"frequency_hz",        &cfg.frequency_hz},
        {"discharge_delay_us",  &cfg.discharge_delay_us},
        {"q_delay_us",          &cfg.q_delay_us},
        {"q_warmup_s",          &cfg.q_warmup_s},
        {"laser_clock_mode",    &cfg.laser_clock_mode},
        {"settle_timeout_ms",   &cfg.settle_timeout_ms},
        {"emission_hold_ms",    &cfg.emission_hold_ms},
        {"min_discharges",      &cfg.min_discharges},
        {"scans",               &cfg.scans},
    };
    for (const Field& f : fields) {
        const unsigned fallback = *f.out;
        const Result<unsigned> v = optional_uint(node, f.key, "laser_fire", fallback);
        if (!v) return v.error();
        *f.out = v.value();
    }

    // 声明标定却没给全参数 = 自相矛盾。宁可在这里报错，也不要拿着 0 去点激光。
    if (cfg.commissioned) {
        if (cfg.discharge_current_a == 0) return invalid("laser_fire.discharge_current_a is required when commissioned=true");
        if (cfg.pulse_width_us == 0)      return invalid("laser_fire.pulse_width_us is required when commissioned=true");
        if (cfg.frequency_hz == 0)        return invalid("laser_fire.frequency_hz is required when commissioned=true");
        if (cfg.q_delay_us == 0)          return invalid("laser_fire.q_delay_us is required when commissioned=true");
        if (cfg.emission_hold_ms == 0)    return invalid("laser_fire.emission_hold_ms is required when commissioned=true");
        if (cfg.min_discharges == 0)      return invalid("laser_fire.min_discharges must be at least 1");
    }
    return cfg;
}

json::Value to_json_serial(const SerialPortConfig& c) {
    json::Value v = json::object();
    v.set("port", json::Value(c.port));
    v.set("baud", json::Value(static_cast<double>(c.baud)));
    v.set("parity", json::Value(c.parity == libs::transport::Parity::none   ? "none"
                                : c.parity == libs::transport::Parity::even ? "even"
                                                                             : "odd"));
    v.set("data_bits", json::Value(static_cast<double>(c.data_bits)));
    v.set("timeout_ms", json::Value(static_cast<double>(c.timeout_ms)));
    v.set("idle_ms", json::Value(static_cast<double>(c.idle_ms)));
    v.set("enabled", json::Value(c.enabled));
    return v;
}

}  // namespace

Result<SiteConfig> from_json(const json::Value& value) {
    if (!value.is_object()) return invalid("the top level of a site config must be an object");

    SiteConfig cfg;
    const Result<std::string> station = required_string(value, "station_id", "root");
    if (!station) return station.error();
    cfg.station_id = station.value();

    cfg.laser = SerialPortConfig{"", 9600, libs::transport::Parity::none, 8, 650, 80, true};
    const Result<SerialPortConfig> laser = parse_serial(value, "laser", cfg.laser);
    if (!laser) return laser.error();
    cfg.laser = laser.value();

    const Result<SerialPortConfig> rf = parse_serial(value, "rangefinder", SerialPortConfig{});
    if (!rf) return rf.error();
    cfg.rangefinder = rf.value();

    const Result<SerialPortConfig> ind = parse_serial(value, "indicator", SerialPortConfig{});
    if (!ind) return ind.error();
    cfg.indicator = ind.value();

    const Result<SerialPortConfig> ac = parse_serial(value, "air_conditioner", SerialPortConfig{});
    if (!ac) return ac.error();
    cfg.air_conditioner = ac.value();

    // SU300 是新增的可选设备；旧配置没有 motor 节点时保持关闭，避免升级后
    // 因缺少串口字段而阻断原有站点启动。
    SerialPortConfig motor_defaults;
    motor_defaults.enabled = false;
    if (value["motor"].is_null()) {
        cfg.motor = motor_defaults;
    } else {
        const Result<SerialPortConfig> motor = parse_serial(value, "motor", motor_defaults);
        if (!motor) return motor.error();
        cfg.motor = motor.value();
    }

    const Result<SpectrometerConfig> spec = parse_spectrometer(value);
    if (!spec) return spec.error();
    cfg.spectrometer = spec.value();

    const Result<LaserFireConfig> lf = parse_laser_fire(value);
    if (!lf) return lf.error();
    cfg.laser_fire = lf.value();

    const Result<GatewayConfig> gw = parse_gateway(value);
    if (!gw) return gw.error();
    cfg.gateway = gw.value();

    const Result<ConveyorConfig> cv = parse_conveyor(value);
    if (!cv) return cv.error();
    cfg.conveyor = cv.value();

    const Result<AirConditionerInterlockConfig> il = parse_interlock(value);
    if (!il) return il.error();
    cfg.ac_interlock = il.value();

    const Result<MotorMotionConfig> mm = parse_motor_motion(value);
    if (!mm) return mm.error();
    cfg.motor_motion = mm.value();

    const json::Value& model = value["indicator_model"];
    if (!model.is_null()) {
        if (!model.is_string()) return invalid("indicator_model must be a string");
        cfg.indicator_model = model.as_string();
    }

    const Result<std::uint8_t> ia = optional_u8(value, "indicator_address", 1);
    if (!ia) return ia.error();
    cfg.indicator_address = ia.value();

    const Result<std::uint8_t> la = optional_u8(value, "laser_address", 1);
    if (!la) return la.error();
    cfg.laser_address = la.value();

    const Result<std::uint8_t> ra = optional_u8(value, "rangefinder_address", 1);
    if (!ra) return ra.error();
    cfg.rangefinder_address = ra.value();

    const Result<std::uint8_t> aca = optional_u8(value, "air_conditioner_address", 1);
    if (!aca) return aca.error();
    cfg.air_conditioner_address = aca.value();

    const Result<std::uint8_t> ma = optional_u8(value, "motor_address", 1);
    if (!ma) return ma.error();
    cfg.motor_address = ma.value();

    const Result<unsigned> ct = optional_uint(value, "capture_timeout_ms", "", 3000);
    if (!ct) return ct.error();
    cfg.capture_timeout_ms = ct.value();

    const Result<double> im = optional_double(value, "integration_ms", 10.0);
    if (!im) return im.error();
    cfg.integration_ms = im.value();

    const VoidResult v = validate(cfg);
    if (!v) return v.error();
    return cfg;
}

json::Value to_json(const SiteConfig& config) {
    json::Value v = json::object();
    v.set("station_id", json::Value(config.station_id));
    v.set("laser", to_json_serial(config.laser));
    v.set("rangefinder", to_json_serial(config.rangefinder));
    v.set("indicator", to_json_serial(config.indicator));
    v.set("air_conditioner", to_json_serial(config.air_conditioner));
    v.set("motor", to_json_serial(config.motor));

    json::Value spec = json::object();
    spec.set("sdk_dll", json::Value(config.spectrometer.sdk_dll));
    json::Array serials;
    for (const std::string& s : config.spectrometer.serials) serials.emplace_back(s);
    spec.set("serials", json::Value(std::move(serials)));
    spec.set("master_serial", json::Value(config.spectrometer.master_serial));
    spec.set("synchronized", json::Value(config.spectrometer.synchronized));
    spec.set("enabled", json::Value(config.spectrometer.enabled));
    spec.set("wait_for_laser_trigger", json::Value(config.spectrometer.wait_for_laser_trigger));
    spec.set("integration_delay_ns", json::Value(config.spectrometer.integration_delay_ns));
    v.set("spectrometer", std::move(spec));

    json::Value lf = json::object();
    lf.set("commissioned", json::Value(config.laser_fire.commissioned));
    lf.set("discharge_current_a", json::Value(static_cast<double>(config.laser_fire.discharge_current_a)));
    lf.set("pulse_width_us", json::Value(static_cast<double>(config.laser_fire.pulse_width_us)));
    lf.set("frequency_hz", json::Value(static_cast<double>(config.laser_fire.frequency_hz)));
    lf.set("discharge_delay_us", json::Value(static_cast<double>(config.laser_fire.discharge_delay_us)));
    lf.set("q_delay_us", json::Value(static_cast<double>(config.laser_fire.q_delay_us)));
    lf.set("q_warmup_s", json::Value(static_cast<double>(config.laser_fire.q_warmup_s)));
    lf.set("laser_clock_mode", json::Value(static_cast<double>(config.laser_fire.laser_clock_mode)));
    lf.set("settle_timeout_ms", json::Value(static_cast<double>(config.laser_fire.settle_timeout_ms)));
    lf.set("emission_hold_ms", json::Value(static_cast<double>(config.laser_fire.emission_hold_ms)));
    lf.set("min_discharges", json::Value(static_cast<double>(config.laser_fire.min_discharges)));
    lf.set("scans", json::Value(static_cast<double>(config.laser_fire.scans)));
    v.set("laser_fire", std::move(lf));

    json::Value gw = json::object();
    gw.set("candidate_ip", json::Value(config.gateway.candidate_ip));
    gw.set("candidate_adapter", json::Value(config.gateway.candidate_adapter));
    gw.set("observed_ip", json::Value(config.gateway.observed_ip));
    gw.set("port", json::Value(static_cast<double>(config.gateway.port)));
    gw.set("enabled", json::Value(config.gateway.enabled));
    gw.set("base_url", json::Value(config.gateway.base_url));
    gw.set("product_id", json::Value(config.gateway.product_id));
    gw.set("device_id", json::Value(config.gateway.device_id));
    gw.set("token", json::Value(config.gateway.authorization_token));
    gw.set("equipment_id", json::Value(config.gateway.equipment_id));
    gw.set("equipment_type", json::Value(config.gateway.equipment_type));
    gw.set("customs", json::Value(config.gateway.customs));
    gw.set("timeout_ms", json::Value(static_cast<double>(config.gateway.timeout_ms)));
    v.set("gateway", std::move(gw));

    json::Value cv = json::object();
    cv.set("software_controlled", json::Value(config.conveyor.software_controlled));
    cv.set("running_assumed", json::Value(config.conveyor.running_assumed));
    v.set("conveyor", std::move(cv));

    json::Value il = json::object();
    il.set("enabled", json::Value(config.ac_interlock.enabled));
    il.set("trigger_indoor_temp_c", json::Value(config.ac_interlock.trigger_indoor_temp_c));
    il.set("calibrated", json::Value(config.ac_interlock.calibrated));
    v.set("ac_interlock", std::move(il));

    json::Value mm = json::object();
    mm.set("commissioned", json::Value(config.motor_motion.commissioned));
    mm.set("max_frequency_percent",
           json::Value(static_cast<double>(config.motor_motion.max_frequency_percent)));
    mm.set("max_duration_ms", json::Value(static_cast<double>(config.motor_motion.max_duration_ms)));
    mm.set("stop_timeout_ms", json::Value(static_cast<double>(config.motor_motion.stop_timeout_ms)));
    mm.set("poll_interval_ms", json::Value(static_cast<double>(config.motor_motion.poll_interval_ms)));
    mm.set("communication_watchdog_confirmed",
           json::Value(config.motor_motion.communication_watchdog_confirmed));
    v.set("motor_motion", std::move(mm));

    v.set("indicator_model", json::Value(config.indicator_model));
    v.set("indicator_address", json::Value(static_cast<double>(config.indicator_address)));
    v.set("laser_address", json::Value(static_cast<double>(config.laser_address)));
    v.set("rangefinder_address", json::Value(static_cast<double>(config.rangefinder_address)));
    v.set("air_conditioner_address", json::Value(static_cast<double>(config.air_conditioner_address)));
    v.set("motor_address", json::Value(static_cast<double>(config.motor_address)));
    v.set("capture_timeout_ms", json::Value(static_cast<double>(config.capture_timeout_ms)));
    v.set("integration_ms", json::Value(config.integration_ms));
    return v;
}

json::Value to_json_redacted(const SiteConfig& config) {
    json::Value v = to_json(config);
    auto it = v.as_object().find("gateway");
    if (it != v.as_object().end() && it->second.is_object()) {
        it->second.set("token", json::Value());
        it->second.set("token_configured", json::Value(!config.gateway.authorization_token.empty()));
    }
    return v;
}

Result<SiteConfig> load_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return invalid("cannot open the site config file: " + path);

    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) return invalid("cannot read the site config file: " + path);

    json::Value root;
    const json::ParseError err = json::parse(buffer.str(), root);
    if (!err.ok) {
        std::ostringstream msg;
        msg << "invalid JSON in " << path << " at offset " << err.offset << ": " << err.message;
        return invalid(msg.str());
    }
    return from_json(root);
}

Result<SiteConfig> default_template() {
    SiteConfig c;
    c.station_id = "libs-station-01";   // 唯一的「值」：站号一家机器一台。这不是推断，改它。
    c.laser.enabled = false;
    c.rangefinder.enabled = false;
    c.indicator.enabled = false;
    c.air_conditioner.enabled = false;
    c.motor.enabled = false;
    c.spectrometer.enabled = false;
    // 触发方向：**激光器触发光谱仪**（AvaSoft 手册 §4.9 接法二）。
    // 默认模板就按这个方向给，接法一（光谱仪 TTL 点火激光器）不在本项目范围内。
    c.spectrometer.wait_for_laser_trigger = true;
    // 收到激光 TTL 后延迟 5000 ns（5 us）开始积分。
    // 取值理由：安全默认，不猜现场最优。手册给的型号下限是 0.28 us（AvaSpec-3648-USB2）
    // 与 1.28 us（AvaSpec-2048-USB2），5 us 两者都满足；
    // 且明显大于激光脉冲宽度（50 us 的脉冲本身不需要回避，
    // 真正要等的是等离子体自发光上升沿），现场按样品再标定。
    c.spectrometer.integration_delay_ns = 5000.0;
    c.gateway.enabled = false;            // 3.5.2 G2：无网络传输层
    c.conveyor.software_controlled = false;   // 未确认前一律 false
    c.ac_interlock.enabled = false;       // 3.9.2 C 类：阈值未标定
    const VoidResult v = validate(c);
    if (!v) return v.error();
    return c;
}

Result<std::string> save_string(const SiteConfig& config) {
    const VoidResult v = validate(config);
    if (!v) return v.error();
    return json::stringify(to_json(config), 2);
}

VoidResult validate(const SiteConfig& config) {
    if (config.station_id.empty()) return invalid("station_id must not be empty");

    const auto check_serial = [](const char* name, const SerialPortConfig& c) -> VoidResult {
        if (!c.enabled) return libs::VoidResult();
        if (c.port.empty()) return invalid(std::string(name) + " is enabled but has no port");
        if (c.port.rfind("COM", 0) != 0 && c.port.rfind("\\\\.\\", 0) != 0)
            return invalid(std::string(name) + ".port must look like COM3");
        if (c.baud == 0) return invalid(std::string(name) + ".baud must be positive");
        if (c.data_bits != 7 && c.data_bits != 8)
            return invalid(std::string(name) + ".data_bits must be 7 or 8");
        return libs::VoidResult();
    };

    const VoidResult l = check_serial("laser", config.laser);
    if (!l) return l;
    const VoidResult r = check_serial("rangefinder", config.rangefinder);
    if (!r) return r;
    const VoidResult i = check_serial("indicator", config.indicator);
    if (!i) return i;
    const VoidResult a = check_serial("air_conditioner", config.air_conditioner);
    if (!a) return a;
    const VoidResult m = check_serial("motor", config.motor);
    if (!m) return m;

    if (config.spectrometer.enabled) {
        if (config.spectrometer.sdk_dll.empty())
            return invalid("spectrometer is enabled but sdk_dll is empty");
        if (config.spectrometer.sdk_dll.find(':') == std::string::npos)
            return invalid("spectrometer.sdk_dll must be an absolute path");
    }
    // 门禁纪律：同步标志只能由现场确认后打开，这里不替现场做决定，
    // 但 master + synchronized 的组合必须自洽。
    if (config.spectrometer.synchronized && config.spectrometer.serials.size() < 2)
        return invalid("spectrometer.synchronized needs at least two serials");
    if (!config.spectrometer.master_serial.empty() && config.spectrometer.serials.empty())
        return invalid("spectrometer.master_serial requires an explicit serials list");

    // 延时只有在等外部触发时才有意义（驱动也会再拒一次，这里提前给出可读错误）。
    if (config.spectrometer.integration_delay_ns > 0.0 && !config.spectrometer.wait_for_laser_trigger)
        return invalid("spectrometer.integration_delay_ns is set but wait_for_laser_trigger is false; "
                       "an integration delay only means something when the laser TTL starts the integration");
    if (config.capture_timeout_ms == 0) return invalid("capture_timeout_ms must be positive");
    if (!(config.integration_ms > 0.0)) return invalid("integration_ms must be positive");

    if (config.laser_address == 0) return invalid("laser_address must be 1..255");
    if (config.rangefinder_address == 0) return invalid("rangefinder_address must be 1..255");
    if (config.air_conditioner_address == 0) return invalid("air_conditioner_address must be 1..255");
    if (config.indicator_address == 0) return invalid("indicator_address must be 1..255");
    if (config.motor_address == 0 || config.motor_address > 247)
        return invalid("motor_address must be 1..247");

    if (config.motor.enabled) {
        if (config.motor.baud != 9600 && config.motor.baud != 19200 && config.motor.baud != 38400)
            return invalid("motor.baud must be one of 9600/19200/38400");
        if (config.motor.data_bits != 8)
            return invalid("motor serial format must be 8N1, 8E1, or 8O1");
        if (config.motor.timeout_ms == 0 || config.motor.timeout_ms > 10000)
            return invalid("motor.timeout_ms must be 1..10000 ms");
        if (config.motor.idle_ms > config.motor.timeout_ms)
            return invalid("motor.idle_ms must not exceed motor.timeout_ms");
    }

    if (config.motor_motion.max_frequency_percent > 100)
        return invalid("motor_motion.max_frequency_percent must be 0..100 percent");
    if (config.motor_motion.max_duration_ms > 60000)
        return invalid("motor_motion.max_duration_ms must be 0 or 1..60000 ms");
    if (config.motor_motion.stop_timeout_ms == 0)
        return invalid("motor_motion.stop_timeout_ms must be positive");
    if (config.motor_motion.poll_interval_ms == 0)
        return invalid("motor_motion.poll_interval_ms must be positive");
    if (config.motor_motion.commissioned) {
        if (!config.motor.enabled)
            return invalid("motor must be enabled when motor_motion.commissioned=true");
        if (config.motor_motion.max_frequency_percent == 0)
            return invalid("motor_motion.max_frequency_percent is required when commissioned=true");
        if (config.motor_motion.max_duration_ms == 0)
            return invalid("motor_motion.max_duration_ms is required when commissioned=true");
        if (!config.motor_motion.communication_watchdog_confirmed)
            return invalid("motor_motion.communication_watchdog_confirmed must be true when commissioned=true");
    }

    // 联锁纪律：未标定不得启用（范围报告 §3.9.2）。
    if (config.ac_interlock.enabled && !config.ac_interlock.calibrated)
        return invalid("ac_interlock is enabled without a calibrated threshold");

    if (config.gateway.timeout_ms == 0)
        return invalid("gateway.timeout_ms must be positive");
    if (config.gateway.enabled) {
        if (config.gateway.base_url.empty())
            return invalid("gateway.base_url is required when gateway.enabled=true");
        if (config.gateway.base_url.rfind("http://", 0) != 0 &&
            config.gateway.base_url.rfind("https://", 0) != 0)
            return invalid("gateway.base_url must start with http:// or https://");
        if (config.gateway.product_id.empty())
            return invalid("gateway.product_id is required when gateway.enabled=true");
        if (config.gateway.device_id.empty())
            return invalid("gateway.device_id is required when gateway.enabled=true");
        if (config.gateway.authorization_token.empty())
            return invalid("gateway.token is required when gateway.enabled=true");
        if (config.gateway.equipment_id.empty())
            return invalid("gateway.equipment_id is required when gateway.enabled=true");
        if (config.gateway.equipment_type.empty())
            return invalid("gateway.equipment_type is required when gateway.enabled=true");
    }

    const VoidResult lf = validate_laser_fire(config.laser_fire);
    if (!lf) return lf;

    return libs::VoidResult();
}

VoidResult validate_laser_fire(const LaserFireConfig& lf) {
    // 未标定时这些数一个都不会被下发，拿它们报错只会挡住
    // "还没标定"这个完全合法的中间状态。
    if (!lf.commissioned) return libs::VoidResult();

    const auto outside = [](unsigned v, unsigned lo, unsigned hi) { return v < lo || v > hi; };
    if (outside(lf.discharge_current_a, 15, 100))
        return invalid("laser_fire.discharge_current_a must be 15..100 A (protocol §3.1)");
    if (outside(lf.pulse_width_us, 50, 300))
        return invalid("laser_fire.pulse_width_us must be 50..300 us (protocol §3.1)");
    if (outside(lf.frequency_hz, 1, 10))
        return invalid("laser_fire.frequency_hz must be 1..10 Hz (protocol §3.1)");
    if (lf.discharge_delay_us > 500)
        return invalid("laser_fire.discharge_delay_us must be 0..500 us (protocol §3.1)");
    if (outside(lf.q_delay_us, 2, 500))
        return invalid("laser_fire.q_delay_us must be 2..500 us (protocol §3.1)");
    if (lf.settle_timeout_ms == 0)
        return invalid("laser_fire.settle_timeout_ms must be positive");
    if (lf.laser_clock_mode != 1 && lf.laser_clock_mode != 2)
        return invalid("laser_fire.laser_clock_mode must be 1 (external clock/internal Q) or 2 (external clock/external Q)");
    return libs::VoidResult();
}

}  // namespace business::config

