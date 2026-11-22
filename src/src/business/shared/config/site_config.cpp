#include "libs/business/shared/config/site_config.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include "libs/measurement_limits.hpp"
#include "libs/protocols/indicator.hpp"

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

Result<ModbusDeviceConfig> parse_optional_modbus(const json::Value& parent, const std::string& key) {
    ModbusDeviceConfig cfg;
    if (!parent.has(key)) return cfg;
    const auto serial = parse_serial(parent, key, cfg.serial);
    if (!serial) return serial.error();
    const auto address = optional_u8(parent[key], "address", cfg.address);
    if (!address) return address.error();
    cfg.serial = serial.value();
    cfg.address = address.value();
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

    // integration_delay_ns 不再从站点配置读取 —— 它是测量参数，
    // 唯一来源是参数库（ParameterStore），默认值见 ParameterService。
    // 这里**不报错**：老的 site.json 里可能有这个字段，让它安静地
    // 被忽略即可，不能因为配置里多一个废弃键就拒绝开机。

    return cfg;
}

// 出光标定参数。**所有字段都必须显式写出**，不给任何"聪明"的默认值：
// 这里每个数的默认值都意味着"能出光"，回落就等于替现场做了一次未标定的决定。
Result<LaserFireConfig> parse_laser_fire(const json::Value& parent) {
    LaserFireConfig cfg;                 // commissioned=false, 全部 0
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
        {"settle_timeout_ms",   &cfg.settle_timeout_ms},
        {"min_discharges",      &cfg.min_discharges},
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

    const Result<SerialPortConfig> ind = parse_serial(value, "indicator", SerialPortConfig{});
    if (!ind) return ind.error();
    cfg.indicator = ind.value();

    const Result<SpectrometerConfig> spec = parse_spectrometer(value);
    if (!spec) return spec.error();
    cfg.spectrometer = spec.value();

    const Result<LaserFireConfig> lf = parse_laser_fire(value);
    if (!lf) return lf.error();
    cfg.laser_fire = lf.value();

    const json::Value& model = value["indicator_model"];
    if (!model.is_null()) {
        if (!model.is_string()) return invalid("indicator_model must be a string");
        cfg.indicator_model = model.as_string();
    }

    const Result<std::uint8_t> ia = optional_u8(value, "indicator_address", 1);
    if (!ia) return ia.error();
    cfg.indicator_address = ia.value();

    const auto rangefinder = parse_optional_modbus(value, "rangefinder");
    if (!rangefinder) return rangefinder.error();
    cfg.rangefinder = rangefinder.value();
    const auto air_conditioner = parse_optional_modbus(value, "air_conditioner");
    if (!air_conditioner) return air_conditioner.error();
    cfg.air_conditioner = air_conditioner.value();

    const auto motor = parse_optional_modbus(value, "motor");
    if (!motor) return motor.error();
    cfg.motor.connection = motor.value();
    if (value.has("motor")) {
        const auto& node = value["motor"];
        const auto jog = optional_uint(node, "jog_ms", "motor", 0);
        const auto forward = optional_uint(node, "forward_limit_mask", "motor", 0);
        const auto reverse = optional_uint(node, "reverse_limit_mask", "motor", 0);
        const auto active_low = optional_bool(node, "limits_active_low", false);
        if (!jog) return jog.error();
        if (!forward) return forward.error();
        if (!reverse) return reverse.error();
        if (!active_low) return active_low.error();
        cfg.motor.jog_ms = jog.value();
        cfg.motor.forward_limit_mask = forward.value();
        cfg.motor.reverse_limit_mask = reverse.value();
        cfg.motor.limits_active_low = active_low.value();
        if (cfg.motor.connection.serial.enabled && cfg.motor.jog_ms && (forward.value() || reverse.value()) &&
                !node.has("limits_active_low"))
            return invalid("motor.limits_active_low is required when limit inputs are configured");
    }

    const VoidResult v = validate(cfg);
    if (!v) return v.error();
    return cfg;
}

json::Value to_json(const SiteConfig& config) {
    json::Value v = json::object();
    v.set("station_id", json::Value(config.station_id));
    v.set("laser", to_json_serial(config.laser));
    v.set("indicator", to_json_serial(config.indicator));

    json::Value spec = json::object();
    spec.set("sdk_dll", json::Value(config.spectrometer.sdk_dll));
    json::Array serials;
    for (const std::string& s : config.spectrometer.serials) serials.emplace_back(s);
    spec.set("serials", json::Value(std::move(serials)));
    spec.set("master_serial", json::Value(config.spectrometer.master_serial));
    spec.set("synchronized", json::Value(config.spectrometer.synchronized));
    spec.set("enabled", json::Value(config.spectrometer.enabled));
    spec.set("wait_for_laser_trigger", json::Value(config.spectrometer.wait_for_laser_trigger));
    v.set("spectrometer", std::move(spec));

    json::Value lf = json::object();
    lf.set("commissioned", json::Value(config.laser_fire.commissioned));
    lf.set("discharge_current_a", json::Value(static_cast<double>(config.laser_fire.discharge_current_a)));
    lf.set("pulse_width_us", json::Value(static_cast<double>(config.laser_fire.pulse_width_us)));
    lf.set("frequency_hz", json::Value(static_cast<double>(config.laser_fire.frequency_hz)));
    lf.set("discharge_delay_us", json::Value(static_cast<double>(config.laser_fire.discharge_delay_us)));
    lf.set("q_delay_us", json::Value(static_cast<double>(config.laser_fire.q_delay_us)));
    lf.set("q_warmup_s", json::Value(static_cast<double>(config.laser_fire.q_warmup_s)));
    lf.set("settle_timeout_ms", json::Value(static_cast<double>(config.laser_fire.settle_timeout_ms)));
    lf.set("min_discharges", json::Value(static_cast<double>(config.laser_fire.min_discharges)));
    v.set("laser_fire", std::move(lf));

    v.set("indicator_model", json::Value(config.indicator_model));
    v.set("indicator_address", json::Value(static_cast<double>(config.indicator_address)));
    const auto modbus_json = [](const ModbusDeviceConfig& device) {
        auto node = to_json_serial(device.serial);
        node.set("address", json::Value(static_cast<double>(device.address)));
        return node;
    };
    v.set("rangefinder", modbus_json(config.rangefinder));
    v.set("air_conditioner", modbus_json(config.air_conditioner));
    auto motor = modbus_json(config.motor.connection);
    motor.set("jog_ms", json::Value(static_cast<double>(config.motor.jog_ms)));
    motor.set("forward_limit_mask", json::Value(static_cast<double>(config.motor.forward_limit_mask)));
    motor.set("reverse_limit_mask", json::Value(static_cast<double>(config.motor.reverse_limit_mask)));
    motor.set("limits_active_low", json::Value(config.motor.limits_active_low));
    v.set("motor", std::move(motor));
    return v;
}

namespace {
Result<json::Value> read_document(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
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
    return root;
}
}  // namespace

Result<SiteConfig> load_file(const std::string& path) {
    const auto root = read_document(path);
    return root ? from_json(root.value()) : Result<SiteConfig>(root.error());
}

Result<SiteConfig> save_device_connection(const std::string& path, const std::string& device,
                                          const json::Value& changes) {
    const bool lamp = device == "indicator";
    if ((!lamp && device != "rangefinder") || !changes.is_object())
        return invalid("连接设置只接受状态灯或测距仪的参数对象");
    auto loaded = read_document(path);
    if (!loaded) return loaded.error();
    auto& root = loaded.value();
    const auto original = from_json(root);
    if (!original) return original.error();
    auto serial = root[device].is_object() ? root[device] : json::object();
    for (const auto& field : changes.as_object()) {
        const auto& key = field.first;
        const auto& value = field.second;
        const bool number = key == "baud" || key == "data_bits" || key == "timeout_ms" || key == "address" || (lamp && key == "idle_ms");
        const bool string = key == "port" || key == "parity" || (lamp && key == "model");
        if (!(number && value.is_number()) && !(string && value.is_string()) && !(key == "enabled" && value.is_bool()))
            return invalid("无效的连接参数：" + key);
        if (lamp && key == "address") root.set("indicator_address", value);
        else if (lamp && key == "model") root.set("indicator_model", value);
        else serial.set(key, value);
    }
    root.set(device, std::move(serial));
    auto updated = from_json(root);
    if (!updated) return updated.error();
    if (lamp) {
        const auto& site = updated.value();
        if (site.indicator_address > 247 || site.indicator.timeout_ms < 80 || site.indicator.timeout_ms > 10000)
            return invalid("状态灯地址须为 1–247，通信超时须为 80–10000 ms");
        libs::indicator::ModelTraits model;
        if (site.indicator.enabled && !libs::indicator::lookup_model(site.indicator_model, model))
            return invalid("启用状态灯前，请填写设备层已支持的准确型号");
    }

    namespace fs = std::filesystem;
    const auto target = fs::u8path(path);
    auto temporary = target;
    temporary += ".connection.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << json::stringify(root, 2) << '\n';
    output.close();
    std::error_code ec;
    if (output) fs::rename(temporary, target, ec);
    if (!output || ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Error::make(ErrorKind::transport, "site_write_failed", "无法保存站点配置，请检查文件权限和占用状态");
    }
    return updated;
}

Result<SiteConfig> default_template() {
    SiteConfig c;
    c.station_id = "libs-station-01";   // 唯一的「值」：站号一家机器一台。这不是推断，改它。
    c.laser.enabled = false;
    c.indicator.enabled = false;
    c.spectrometer.enabled = false;
    // 触发方向：**激光器触发光谱仪**（AvaSoft 手册 §4.9 接法二）。
    // 默认模板就按这个方向给，接法一（光谱仪 TTL 点火激光器）不在本项目范围内。
    c.spectrometer.wait_for_laser_trigger = true;
    // 收到激光 TTL 后延迟多久开始积分是**测量参数**，不在这里设默认值。
    // 出厂值 5000 ns 由 ParameterService 在首次启动时种进参数库，之后
    // 操作员按样品标定，改这里不会生效（曾经两份来源，就是这么来的）。
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
    const VoidResult i = check_serial("indicator", config.indicator);
    if (!i) return i;
    const auto check_modbus = [&](const char* name, const ModbusDeviceConfig& device) -> VoidResult {
        if (device.address < 1 || device.address > 247)
            return invalid(std::string(name) + ".address must be in 1..247");
        // The serial Link uses an 80 ms end-of-frame idle interval by default.
        if (device.serial.enabled && (device.serial.timeout_ms < 80 || device.serial.timeout_ms > 10000))
            return invalid(std::string(name) + ".timeout_ms must be in 80..10000");
        return check_serial(name, device.serial);
    };
    const auto rangefinder = check_modbus("rangefinder", config.rangefinder);
    if (!rangefinder) return rangefinder;
    const auto air_conditioner = check_modbus("air_conditioner", config.air_conditioner);
    if (!air_conditioner) return air_conditioner;
    const auto motor = check_modbus("motor", config.motor.connection);
    if (!motor) return motor;
    if (config.motor.connection.serial.enabled && config.motor.jog_ms) {
        const auto single_bit = [](unsigned mask) { return mask && mask <= 0xffff && (mask & (mask - 1)) == 0; };
        if (config.motor.jog_ms < libs::measurement_limits::kMotorJogMinMs ||
                config.motor.jog_ms > libs::measurement_limits::kMotorJogMaxMs)
            return invalid("motor.jog_ms must be in 100..50000, or 0 for status only");
        if ((config.motor.forward_limit_mask || config.motor.reverse_limit_mask) &&
            (!single_bit(config.motor.forward_limit_mask) || !single_bit(config.motor.reverse_limit_mask) ||
             config.motor.forward_limit_mask == config.motor.reverse_limit_mask))
            return invalid("motor limit mappings must be omitted or use distinct one-bit masks");
    }
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

    if (config.indicator_address == 0) return invalid("indicator_address must be 1..255");

    const VoidResult lf = validate_laser_fire(config.laser_fire);
    if (!lf) return lf;

    return libs::VoidResult();
}

VoidResult validate_laser_fire(const LaserFireConfig& lf) {
    // 未标定时这些数一个都不会被下发，拿它们报错只会挡住
    // "还没标定"这个完全合法的中间状态。
    if (!lf.commissioned) return libs::VoidResult();

    const std::pair<const char*, unsigned> fields[] = {
        {"discharge_current_a", lf.discharge_current_a}, {"pulse_width_us", lf.pulse_width_us},
        {"frequency_hz", lf.frequency_hz}, {"discharge_delay_us", lf.discharge_delay_us},
        {"q_delay_us", lf.q_delay_us}, {"q_warmup_s", lf.q_warmup_s},
        {"settle_timeout_ms", lf.settle_timeout_ms},
        {"min_discharges", lf.min_discharges},
    };
    for (const auto& field : fields) {
        const auto* range = libs::measurement_limits::find("laser", field.first);
        if (!range->contains(field.second))
            return invalid(std::string("laser_fire.") + field.first + " must be " +
                std::to_string(range->lo) + ".." + std::to_string(range->hi) + " " +
                range->unit + " (" + range->source + ")");
    }
    return libs::VoidResult();
}

}  // namespace business::config

