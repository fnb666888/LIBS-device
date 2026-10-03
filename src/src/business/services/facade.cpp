#include "libs/business/services/facade.hpp"

#include "libs/business/services/laser_fire.hpp"

#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

#include "libs/business/integration/heike_events.hpp"
#include "libs/business/services/composition.hpp"
#include "libs/business/services/capture.hpp"
#include "libs/business/services/data_delivery.hpp"
#include "libs/business/services/motion.hpp"
#include "libs/business/services/ore_presence.hpp"
#include "libs/business/services/spectrum_stream.hpp"
#include "libs/business/time_util.hpp"

namespace business::app {

Response Response::success(json::Value payload) {
    Response r;
    r.ok = true;
    r.payload = std::move(payload);
    return r;
}

Response Response::failure(libs::Error error) {
    Response r;
    r.ok = false;
    r.error = std::move(error);
    json::Value e = json::object();
    e.set("kind", json::Value(libs::to_string(r.error.kind)));
    e.set("code", json::Value(r.error.code));
    e.set("message", json::Value(r.error.message));
    r.payload.set("error", std::move(e));
    return r;
}

namespace {

// 交付状态 -> JSON。两个状态位彼此正交，**不合并成一个枚举**
// （任务书 §6.3：本地存了 != 海关收到了）。
json::Value delivery_to_json(const data_delivery::DeliveryStatus& s) {
    json::Value d = json::object();
    d.set("local_saved", json::Value(s.local_saved));
    d.set("local_path", json::Value(s.local_path));
    d.set("local_error", json::Value(s.local_error));
    d.set("gateway_delivered", json::Value(s.gateway_delivered));
    d.set("gateway_error", json::Value(s.gateway_error));
    return d;
}

}  // namespace

libs::VoidResult BusinessFacade::deliver_capture(
        const std::vector<libs::devices::ScanGroup>& groups,
        data_delivery::DeliveryStatus& status_out) {
    data_delivery::CaptureReference ref;
    ref.station_id = runtime_.config().station_id;
    ref.belt_sequence = runtime_.belt_sequence();
    ref.target_belt = runtime_.target_belt();
    ref.captured_at_ms = wall_clock_ms();
    if (!groups.empty()) {
        ref.shot_id = groups.front().shot_id;
        ref.hardware_synchronized = groups.front().hardware_synchronized;   // 原样上送，不推断
        for (const auto& group : groups)
            for (const auto& spectrum : group.spectra) ref.serials.push_back(spectrum.serial);
    }

    // 载荷含完整 wavelength_nm + counts：本地交付存的是**原始光谱**。
    const std::string blob =
        json::stringify(services::CaptureCoordinator::to_payload(groups));
    const libs::Result<data_delivery::DeliveryStatus> delivered =
        runtime_.local_sink().deliver(ref, blob);

    if (delivered) {
        status_out = delivered.value();
    } else {
        status_out = data_delivery::DeliveryStatus{};
        status_out.local_error = "本地交付出口调用失败：" + delivered.error().message;
    }
    // C 类（协议/地址/端口/确认层级全未定）：绝不返回"已交付海关"的假象，
    // 但把前置条件如实带上，让上游知道差什么。
    status_out.gateway_delivered = false;
    status_out.gateway_error = data_delivery::UnsupportedGatewaySink::prerequisites();
    runtime_.set_last_delivery(status_out);   // 同时写进运行日志

    if (!status_out.local_saved) {
        return libs::Error::make(libs::ErrorKind::device, "delivery_local_failed",
                                 "本地交付失败，本次采集的光谱没有落盘：" +
                                     status_out.local_error);
    }
    return libs::VoidResult();
}

bool BusinessFacade::parse_fire_parameters(const json::Value& args,
                                           services::FireParameters& out,
                                           libs::Error& error) {
    // 本次参数 = 站点配置 + 请求覆盖。
    //
    // 界面上的值必须真的生效，否则那又是一处"看着能调、实际不生效" ——
    // 这个项目已经为此栽过一次（界面的激光器参数写进业务参数库，
    // 出光读的却是另一份站点配置）。
    out.laser = runtime_.config().laser_fire;
    out.integration_ms = static_cast<double>(runtime_.config().integration_ms);
    out.capture_timeout_ms = runtime_.config().capture_timeout_ms;
    out.laser_clock_mode = static_cast<std::uint8_t>(runtime_.config().laser_fire.laser_clock_mode);

    // 触发方向是物理接线语义，必须由后端白名单解析；不能把任意字符串
    // 透传成设备动作。缺省保持现场已验证的激光器 -> 光谱仪方向。
    if (args.has("trigger_direction")) {
        if (!args["trigger_direction"].is_string()) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                      "trigger_direction_bad",
                                      "trigger_direction 必须是 laser_to_spectrometer 或 spectrometer_to_laser");
            return false;
        }
        const std::string direction = args["trigger_direction"].as_string();
        if (direction == "spectrometer_to_laser") {
            out.spectrometer_triggers_laser = true;
        } else if (direction != "laser_to_spectrometer") {
            error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                      "trigger_direction_bad",
                                      "trigger_direction 必须是 laser_to_spectrometer 或 spectrometer_to_laser");
            return false;
        }
    }

    if (args.has("laser_clock_mode")) {
        if (!args["laser_clock_mode"].is_number()) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                      "laser_clock_mode_bad",
                                      "laser_clock_mode 必须是 1 或 2");
            return false;
        }
        const double v = args["laser_clock_mode"].as_number();
        if (!(v == 1.0 || v == 2.0)) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                      "laser_clock_mode_bad",
                                      "laser_clock_mode 必须是 1（外时钟/内Q）或 2（外时钟/外Q）");
            return false;
        }
        out.laser_clock_mode = static_cast<std::uint8_t>(v);
    }

    if (args.has("laser")) {
        const json::Value& o = args["laser"];
        if (!o.is_object()) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument, "laser_overrides_bad",
                                      "args.laser 必须是对象");
            return false;
        }
        const auto read_u = [&](const char* key, unsigned* dst) {
            if (!o.has(key)) return true;
            if (!o[key].is_number()) return false;
            const double v = o[key].as_number();
            if (v < 0.0 || v != std::floor(v)) return false;
            *dst = static_cast<unsigned>(v);
            return true;
        };
        const bool shape_ok =
            read_u("discharge_current_a", &out.laser.discharge_current_a) &&
            read_u("pulse_width_us", &out.laser.pulse_width_us) &&
            read_u("frequency_hz", &out.laser.frequency_hz) &&
            read_u("discharge_delay_us", &out.laser.discharge_delay_us) &&
            read_u("q_delay_us", &out.laser.q_delay_us) &&
            read_u("q_warmup_s", &out.laser.q_warmup_s);
        if (!shape_ok) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument, "laser_overrides_bad",
                                      "args.laser 的字段必须是 0 或正整数");
            return false;
        }
        // 量程校验走**与站点配置同一套**函数：不接受第二套规则，
        // 否则"界面限 100 A、请求能传 500 A"这种破口会在两处各开一次。
        const libs::VoidResult lf = config::validate_laser_fire(out.laser);
        if (!lf) {
            error = lf.error();
            return false;
        }
    }

    if (args.has("spectrometer")) {
        const json::Value& o = args["spectrometer"];
        if (!o.is_object()) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                      "spectrometer_overrides_bad",
                                      "args.spectrometer 必须是对象");
            return false;
        }
        if (o.has("integration_ms")) {
            if (!o["integration_ms"].is_number()) {
                error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                          "spectrometer_overrides_bad",
                                          "spectrometer.integration_ms 必须是数字");
                return false;
            }
            const double v = o["integration_ms"].as_number();
            // 与驱动硬校验一致（spectrometer.cpp: [0.002, 60000] ms）。
            if (!(v >= 0.002 && v <= 60000.0)) {
                error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                          "spectrometer_overrides_bad",
                                          "spectrometer.integration_ms 必须在 0.002~60000 ms 之间");
                return false;
            }
            out.integration_ms = v;
        }
        if (o.has("capture_timeout_ms")) {
            if (!o["capture_timeout_ms"].is_number()) {
                error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                          "spectrometer_overrides_bad",
                                          "spectrometer.capture_timeout_ms 必须是数字");
                return false;
            }
            const double v = o["capture_timeout_ms"].as_number();
            if (!(v >= 1.0 && v <= 600000.0 && v == std::floor(v))) {
                error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                          "spectrometer_overrides_bad",
                                          "spectrometer.capture_timeout_ms 必须是 1~600000 的整数毫秒");
                return false;
            }
            out.capture_timeout_ms = static_cast<unsigned>(v);
        }
        const auto read_delay = [&](const char* key, double* dst) -> bool {
            if (!o.has(key)) return true;
            if (!o[key].is_number()) return false;
            const double v = o[key].as_number();
            if (!std::isfinite(v) || v < 0.0) return false;
            *dst = v;
            return true;
        };
        if (!read_delay("laser_delay_ns", &out.laser_trigger_delay_ns) ||
            !read_delay("laser_width_ns", &out.laser_trigger_width_ns)) {
            error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                      "spectrometer_overrides_bad",
                                      "spectrometer.laser_delay_ns/laser_width_ns 必须是非负有限数字");
            return false;
        }
    }
    return true;
}

std::vector<std::string> BusinessFacade::commands() {
    return {
        "ping",
        "config_show",
        "status",
        "gate",
        "param_set",
        "param_get",
        "detection_start",
        "capture",
        // 出光是唯一有物理后果的动作，必须出现在命令面上。
        // 此前它只在 dispatch() 里可派发，commands() 与 CLI 都看不见它，
        // 等于把核心功能从公开接口上摘掉了。
        "laser_fire",
        // 连续出光：开始 / 停止。与 laser_fire（有界测量）分开 —— 那个会
        // 出光几秒后自动收光，这个一直打，直到 laser_stop。
        "laser_run",
        "laser_stop",
        "composition",
        "recovery_list",
        "recovery_write",
        "health",
        "spectrum_stream",
        // 海关报文只提供离线构造/校验；未配置正式边端时绝不联网。
        "heike_report_preview",
    };
}

Response BusinessFacade::dispatch(const std::string& request_json) {
    json::Value request;
    const json::ParseError err = json::parse(request_json, request);
    if (!err.ok) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "request_bad_json",
            "请求不是合法 JSON，偏移 " + std::to_string(err.offset) + ": " + err.message));
    }
    if (!request.is_object() || !request.has("command")) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "request_missing_command",
            "请求必须是一个含 command 字段的 JSON 对象"));
    }
    const json::Value& command = request["command"];
    if (!command.is_string()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "request_command_not_string",
            "command 必须是字符串"));
    }
    const json::Value args = request.has("args") ? request["args"] : json::object();
    if (!args.is_object()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "request_args_not_object",
            "args 必须是 JSON 对象"));
    }

    try {
        return handle(command.as_string(), args);
    } catch (const std::exception& e) {
        // 门面绝不把异常抛给 IPC 层：那是"进程崩掉"的另一种形式。
        return Response::failure(libs::Error::make(
            libs::ErrorKind::internal, "request_unhandled_exception",
            std::string("命令执行抛出异常: ") + e.what()));
    }
}

Response BusinessFacade::handle(const std::string& command, const json::Value& args) {
    if (command == "ping")            return cmd_ping(args);
    if (command == "config_show")     return cmd_config_show(args);
    if (command == "status")          return cmd_status(args);
    if (command == "gate")            return cmd_gate(args);
    if (command == "param_set")       return cmd_param_set(args);
    if (command == "param_get")       return cmd_param_get(args);
    if (command == "detection_start") return cmd_detection_start(args);
    if (command == "capture")         return cmd_capture(args);
    if (command == "laser_fire")      return cmd_laser_fire(args);
    if (command == "laser_run")       return cmd_laser_run(args);
    if (command == "laser_stop")      return cmd_laser_stop(args);
    if (command == "spectrum_stream") return cmd_spectrum_stream(args);
    if (command == "composition")     return cmd_composition(args);
    if (command == "recovery_list")   return cmd_recovery_list(args);
    if (command == "recovery_write")  return cmd_recovery_write(args);
    if (command == "health")          return cmd_health(args);
    if (command == "heike_report_preview") return cmd_heike_report_preview(args);
    return Response::failure(libs::Error::make(
        libs::ErrorKind::unsupported, "unknown_command",
        "未知命令: " + command + "；可用命令见 commands 字段"));
}

Response BusinessFacade::cmd_ping(const json::Value& args) {
    (void)args;
    json::Value v = json::object();
    v.set("pong", json::Value(true));
    v.set("wall_clock_ms", json::Value(static_cast<double>(wall_clock_ms())));
    v.set("running", json::Value(runtime_.running()));
    json::Value cmds = json::array();
    for (const auto& c : commands()) cmds.push_back(json::Value(c));
    v.set("commands", std::move(cmds));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_config_show(const json::Value& args) {
    (void)args;
    // 回显当前生效配置。注意这是**内存里正在用的**配置，不是磁盘上那份文件。
    // 网关 Bearer token 属于凭证，不能通过命名管道、日志或 UI 回显。
    return Response::success(config::to_json_redacted(runtime_.config()));
}

Response BusinessFacade::cmd_status(const json::Value& args) {
    (void)args;
    // 下面要读两个设备（测距仪、空调），必须走 device_mutex：串口是独占资源，
    // 与出光/采集并发轮询会串帧。
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());

    json::Value v = json::object();
    v.set("running", json::Value(runtime_.running()));
    v.set("gate", runtime_.gate().to_json());
    v.set("stage", json::Value(events::to_string(runtime_.stage())));
    // 连续出光是否在跑。界面靠它决定「出光测量 / 连续出光 / 停止出光」哪个可点。
    v.set("laser_running", json::Value(runtime_.laser_running()));
    v.set("belt_sequence", json::Value(static_cast<double>(runtime_.belt_sequence())));
    v.set("target_belt", json::Value(runtime_.target_belt()));
    v.set("health", runtime_.health_json());
    v.set("log_records", json::Value(static_cast<double>(runtime_.log().count())));

    // 辅助监测：**这里是真的去读一次设备**。
    //
    // 以前读的是 ac_monitor().last_known() 这份缓存，而全进程没有任何一处
    // 刷新它；测距那边同理，没人喂观测给 ore_judge。两个服务都写好了、
    // 就是没接线，所以界面上"室内温度""测距"永远空着。
    //
    // 读失败/超时照样如实上报三态（available / stale / unavailable），
    // 绝不拿上一次的读数冒充本次。
    // **节流**：辅助监测每 AUX_READ_INTERVAL_MS 才真的去读一次设备。
    // status 被 UI 每 800ms 调一次，每次都读三个串口事务会把它从 8ms 拖到 2 秒，
    // 常年占着 device_mutex，把采集饿死。多数时候回缓存，新鲜度如实上报。
    constexpr Milliseconds kAuxReadIntervalMs = 3000;
    const Milliseconds now_ms = monotonic_ms();
    const bool aux_due = (now_ms - runtime_.last_aux_read_ms()) >= kAuxReadIntervalMs;

    services::AcSnapshot ac = runtime_.ac_monitor().last_known();
    if (aux_due) {
        ac = runtime_.ac_monitor().poll(runtime_.devices().air_conditioner());
    }
    v.set("air_conditioner", services::to_json(ac));
    v.set("ac_interlock_participates", json::Value(runtime_.ac_interlock().participates_in_gate()));

    // 测距仪：读一次，整理成观测喂给判定器。
    // 从"距离出界下限"起读 9 个寄存器，正好覆盖协议登记的那几个字段。
    if (aux_due) if (libs::devices::Rangefinder* rf = runtime_.devices().rangefinder()) {
        const libs::Result<libs::devices::RangefinderRead> read =
            rf->read(runtime_.config().rangefinder_address,
                     libs::rangefinder::reg::kDistanceZeroOnOutOfRange, 9);
        if (read) {
            runtime_.ore_judge().record(
                services::observe(read.value().reading, libs::Error{}));
        } else {
            // 读失败**不是**"距离为 0"：原样记成失败观测，UI 会显示"读数失败"。
            runtime_.ore_judge().record(
                services::observe(libs::rangefinder::Reading{}, read.error()));
        }
    }
    // 两个设备都读过了（或确认没设备）才算这一轮辅助读取完成。
    if (aux_due) runtime_.mark_aux_read(now_ms);

    // 有矿判定：未标定时 ore_present 为 null（未知），不是 false。
    json::Value ore = services::to_json(runtime_.ore_judge().decide());
    // 把**最近一次观测**的字段并进去：判定结果里没有距离/可信度
    // （判定可以是"未知"，但距离本身照样该显示给现场看）。
    if (const auto& obs = runtime_.ore_judge().last_observation()) {
        const json::Value oj = services::to_json(*obs);
        for (const auto& kv : oj.as_object()) ore.set(kv.first, kv.second);
    }
    // 读数有多旧，如实报出来 —— 节流之后"这条读数是不是当下的"必须看得见。
    ore.set("read_age_ms", json::Value(static_cast<double>(
                               (now_ms - runtime_.last_aux_read_ms() < 0)
                                   ? -1
                                   : (now_ms - runtime_.last_aux_read_ms()))));
    v.set("ore", std::move(ore));
    v.set("motion", services::to_json(runtime_.motion_timing()));

    if (runtime_.last_delivery().has_value()) {
        const auto& d = *runtime_.last_delivery();
        json::Value dv = json::object();
        dv.set("local_saved", json::Value(d.local_saved));
        dv.set("local_path", json::Value(d.local_path));
        dv.set("gateway_delivered", json::Value(d.gateway_delivered));
        v.set("last_delivery", std::move(dv));
    } else {
        v.set("last_delivery", json::Value());   // 还没采过
    }

    // 未实现能力清单：让 status 自己回答"哪些还差前置条件"。
    json::Value blocked = json::array();
    blocked.push_back(json::Value("compose_algorithm: " + services::CompositionAnalyzer::prerequisites()));
    blocked.push_back(json::Value("position_switch: " + services::UnavailablePositionSwitch::prerequisites()));
    blocked.push_back(json::Value("gateway_delivery: " + data_delivery::UnsupportedGatewaySink::prerequisites()));
    blocked.push_back(json::Value("gateway_signals: 192.168.101.100 是候选 IPC 地址；无网络层"));
    blocked.push_back(json::Value("ac_interlock: " + services::AirConditionerInterlock::prerequisites()));
    v.set("unavailable_capabilities", std::move(blocked));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_gate(const json::Value& args) {
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());
    if (args.has("action")) {
        const std::string action = args["action"].as_string();
        if (action == "arm") {
            const libs::VoidResult r = runtime_.gate().arm();
            if (!r) return Response::failure(r.error());
        } else if (action == "disarm") {
            runtime_.gate().disarm();
        } else if (action == "refresh") {
            const libs::VoidResult r = runtime_.poll_laser_facts();
            if (!r) return Response::failure(r.error());
        } else {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "gate_bad_action",
                "action 只能是 arm / disarm / refresh，收到: " + action));
        }
    }
    if (args.has("parameters_ready") && args["parameters_ready"].is_bool()) {
        runtime_.gate().set_parameters_ready(args["parameters_ready"].as_bool());
    }
    return Response::success(runtime_.gate().to_json());
}

Response BusinessFacade::cmd_param_set(const json::Value& args) {
    policy::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::internal, "parameters_absent", "参数存储未初始化"));
    }
    if (!args.has("group") || !args["group"].is_string()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "param_missing_group", "缺少 group 字段"));
    }
    if (!args.has("values") || !args["values"].is_object()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "param_missing_values", "values 必须是 JSON 对象"));
    }
    policy::ApplyTiming timing = policy::ApplyTiming::at_next_idle;
    if (args.has("timing") && args["timing"].is_string() &&
        args["timing"].as_string() == "immediately") {
        timing = policy::ApplyTiming::immediately;
    }
    std::map<std::string, double> values;
    for (const auto& kv : args["values"].as_object()) {
        if (!kv.second.is_number()) {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "param_value_not_number",
                "字段 " + kv.first + " 的值必须是数字（缺失/unknown 用不上参数请省略该字段）"));
        }
        values[kv.first] = kv.second.as_number();
    }

    const policy::ApplyResult result = store->apply(args["group"].as_string(), values, timing);
    json::Value v = json::object();
    v.set("state", json::Value(policy::to_string(result.state)));
    v.set("completed", json::Value(result.completed()));
    v.set("group", json::Value(result.group));
    v.set("message", json::Value(result.message));
    json::Array failed;
    for (const auto& f : result.failed_fields) failed.emplace_back(f);
    v.set("failed_fields", json::Value(std::move(failed)));
    v.set("note", json::Value("completed=false 表示“收到请求但未完成”；"
                              "调用方必须检查它，不得把非 completed 当成成功（任务书 §4.2）"));

    // 参数齐备与否驱动门禁（3.4.1）。这是参数模块与门禁之间唯一的耦合点。
    const policy::ReadinessReport readiness = store->readiness();
    json::Value r = json::object();
    r.set("ready", json::Value(readiness.ready));
    r.set("summary", json::Value(readiness.summary));
    v.set("readiness", std::move(r));
    runtime_.gate().set_parameters_ready(readiness.ready);
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_param_get(const json::Value& args) {
    policy::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::internal, "parameters_absent", "参数存储未初始化"));
    }
    json::Value v = json::object();
    if (args.has("group") && args["group"].is_string()) {
        std::map<std::string, double> out;
        const policy::ApplyResult r = store->query(args["group"].as_string(), out);
        if (!r.completed()) {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "param_query_rejected", r.message));
        }
        json::Value values = json::object();
        for (const auto& kv : out) values.set(kv.first, json::Value(kv.second));
        v.set("group", json::Value(args["group"].as_string()));
        v.set("values", std::move(values));
    } else {
        const auto all = store->all();
        json::Value groups = json::object();
        for (const auto& kv : all) {
            json::Value values = json::object();
            for (const auto& f : kv.second.values) values.set(f.first, json::Value(f.second));
            json::Value g = json::object();
            g.set("values", std::move(values));
            g.set("complete", json::Value(kv.second.complete));
            groups.set(kv.first, std::move(g));
        }
        v.set("groups", std::move(groups));
    }
    const policy::ReadinessReport readiness = store->readiness();
    json::Value r = json::object();
    r.set("ready", json::Value(readiness.ready));
    r.set("summary", json::Value(readiness.summary));
    v.set("readiness", std::move(r));
    // param_get 与 param_set 读的是同一份存储，两处的"参数齐备"必须一致。
    // 缺这一行会出现：参数已落盘、重启后 param_get 报"核心参数齐备"，
    // 门禁却仍是 parameters_incomplete（因为只有 param_set 才同步过门禁）。
    // 两个视图互相矛盾正是本项目 §6.4 记过的最难排障的一类缺陷。
    runtime_.gate().set_parameters_ready(readiness.ready);
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_detection_start(const json::Value& args) {
    // 3.5.1：这是**业务信号**，不是物理触发。它只是"请求开始检测"。
    // 门禁可能拒绝它，采集协调也可能拒绝执行它。
    events::DetectionStartRequested request;
    request.target_belt = args.has("target_belt") && args["target_belt"].is_string()
                              ? args["target_belt"].as_string()
                              : runtime_.target_belt();
    request.requested_by = args.has("requested_by") && args["requested_by"].is_string()
                               ? args["requested_by"].as_string()
                               : "cli";
    request.stage = events::DetectionStage::selected_belt;
    request.wall_clock_ms = wall_clock_ms();
    if (args.has("attributes") && args["attributes"].is_object()) {
        for (const auto& kv : args["attributes"].as_object()) {
            request.attributes[kv.first] = kv.second.as_string();
        }
    }

    json::Value v = json::object();
    v.set("accepted", json::Value(true));
    v.set("request", events::to_json(request));
    v.set("note", json::Value("这是业务信号；不代表出光许可，也不代表物理触发已发生"
                              "（任务书 §2.1、§3 步骤 6）"));

    if (!request.target_belt.empty()) runtime_.set_target_belt(request.target_belt);
    runtime_.set_stage(events::DetectionStage::selected_belt);
    runtime_.log().write(logging::RecordKind::general, "收到开始检测业务信号（尚非物理触发）",
                         [&] {
                             json::Value f = json::object();
                             f.set("target_belt", json::Value(request.target_belt));
                             f.set("requested_by", json::Value(request.requested_by));
                             return f;
                         }());
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_capture(const json::Value& args) {
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());
    services::CapturePace pace = services::CaptureCoordinator::pace_from_frequency(
        runtime_.work_frequency(), runtime_.config());
    if (args.has("from_config") && args["from_config"].as_bool()) {
        pace = services::CaptureCoordinator::pace_from_config(runtime_.config());
    }
    if (!pace.configured) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "pace_unconfigured",
            "工作频率未配置，采集节奏无从推导：" + pace.note));
    }

    // 采集租约：与实时流互斥。理由见 spectrum_stream.hpp ——
    // 外触发模式下任何一次 capture 都会去吃 pin 6 的 TTL，
    // 两条采集路径同时在等就是抢触发，测量那次会永远等不到。
    // 租约是 RAII，本函数返回（含所有失败路径）时自动释放。
    services::AcquisitionLease lease(runtime_.spectrum_stream());

    const libs::Result<libs::devices::CaptureResult> result =
        runtime_.capture().capture(runtime_.devices().spectrometer(), pace);
    if (!result) {
        return Response::failure(result.error());
    }
    json::Value v = json::object();
    v.set("pace_configured", json::Value(pace.configured));
    v.set("groups", json::Value(static_cast<double>(result.value().groups.size())));
    json::Array syncs;
    for (const auto& group : result.value().groups) {
        const services::CaptureCoordinator::SyncView view =
            services::CaptureCoordinator::sync_view(runtime_.config(), group);
        json::Value g = json::object();
        g.set("shot_id", json::Value(static_cast<double>(group.shot_id)));
        g.set("complete", json::Value(group.complete));
        // 原样上送，不推断。
        g.set("hardware_synchronized", json::Value(view.hardware_synchronized));
        g.set("requested_sync", json::Value(view.requested_sync));
        json::Value order = json::array();
        for (const auto& s : view.arm_order) order.push_back(json::Value(s));
        g.set("arm_order", std::move(order));
        syncs.push_back(std::move(g));
    }
    v.set("scan_groups", std::move(syncs));
    v.set("note", json::Value("hardware_synchronized 原样上送 SDK 事实；"
                              "业务层不宣称物理同步（范围报告 §3.10.1）"));

    // ── 交付出口（能力 6.3.1）───────────────────────────────────────────────
    // 与 laser_fire 共用 deliver_capture：采集到光谱的命令**每一条都要落盘**，
    // 不能一条接了、另一条漏了 —— 漏的那条就是"数据只活在管道响应里"。
    data_delivery::DeliveryStatus delivery;
    const libs::VoidResult saved = deliver_capture(result.value().groups, delivery);
    if (!saved) {
        Response resp = Response::failure(saved.error());
        resp.payload.set("captured", json::Value(true));   // 设备确实采到了，丢的是落盘
        resp.payload.set("delivery", delivery_to_json(delivery));
        return resp;
    }
    v.set("delivery", delivery_to_json(delivery));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_laser_fire(const json::Value& args) {
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());

    services::FireParameters params;
    libs::Error param_error{};
    if (!parse_fire_parameters(args, params, param_error)) {
        return Response::failure(param_error);
    }

    // 门禁预检：allow_when_blocked 是显式旁路，默认绝不允许。
    // 允许旁路时也只跳"门禁这一条"，其他失败（设备未开、串口错）照样拒绝。
    if (args.has("precheck_only") && args["precheck_only"].as_bool()) {
        if (params.spectrometer_triggers_laser) {
            if (runtime_.devices().spectrometer() == nullptr) {
                return Response::failure(libs::Error::make(
                    libs::ErrorKind::unsupported, "spectrometer_not_open",
                    "反向触发预检需要已打开的光谱仪（Pin 23 输出端）"));
            }
            if (runtime_.config().spectrometer.master_serial.empty()) {
                return Response::failure(libs::Error::make(
                    libs::ErrorKind::invalid_argument, "laser_output_master_required",
                    "反向触发预检需要配置 spectrometer.master_serial 作为 Pin 23 主通道"));
            }
        }
        const libs::VoidResult r = runtime_.laser_fire().precheck();
        if (!r) return Response::failure(r.error());
        json::Value v = json::object();
        v.set("precheck", json::Value(true));
        v.set("gate", runtime_.gate().to_json());
        v.set("trigger_direction", json::Value(
            params.spectrometer_triggers_laser ? "spectrometer_to_laser"
                                                : "laser_to_spectrometer"));
        v.set("trigger_path", json::Value(
            params.spectrometer_triggers_laser
                ? "Avantes Master HD-26 Pin 23 LASER OUT -> laser CLK IN"
                : "laser Q OUT -> Avantes Master HD-26 Pin 6 Trig In"));
        v.set("trigger_output_programmed", json::Value(false));
        v.set("note", json::Value("预检通过，门禁允许出光；本次未发出任何写指令"));
        return Response::success(std::move(v));
    }

    json::Value plan = json::object();
    {
        const config::LaserFireConfig& lf = runtime_.config().laser_fire;
        plan.set("commissioned", json::Value(lf.commissioned));
        plan.set("session_parameters", json::Value(
            "来自配置 laser_fire：电流 " + std::to_string(lf.discharge_current_a) + "A / 脉宽 " +
            std::to_string(lf.pulse_width_us) + "us / 频率 " + std::to_string(lf.frequency_hz) +
            "Hz / 放电延时 " + std::to_string(lf.discharge_delay_us) + "us / Q 延时 " +
            std::to_string(lf.q_delay_us) + "us / Q 预热 " + std::to_string(lf.q_warmup_s) + "s"));
    }
    plan.set("untouched_machine_config", json::Value(
        "0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护是本机整机标定值，"
        "存在设备 EEPROM 里被所有工具共享，本流程**只读不写**"));
    plan.set("sequence", json::Value("0xB2 读基线 -> 写会话参数 -> 0x21=1 -> 轮询 0xB2 等"
                                    "「正在工作且放电」 -> 0x24=1 连续出光 -> 0x24=0 收光 -> "
                                    "0x21=0 停机 -> 0xB2 读回确认（计数器增量/光闸关/已停机）"));
    plan.set("trigger_mode", json::Value(
        "连续出光；**不使用 0x34 单次触发**。协议文本把 0x34 写成单次触发，"
        "但厂家工具实测从不使用它，本项目照协议文本实现的版本真机上一次都没出过光"));
    plan.set("trigger_direction", json::Value(
        params.spectrometer_triggers_laser ? "spectrometer_to_laser" : "laser_to_spectrometer"));
    plan.set("trigger_path", json::Value(
        params.spectrometer_triggers_laser
            ? "Avantes Master HD-26 Pin 23 LASER OUT -> laser CLK IN"
            : "laser Q OUT -> Avantes Master HD-26 Pin 6 Trig In"));
    plan.set("laser_clock_mode", json::Value(static_cast<double>(params.laser_clock_mode)));
    plan.set("cleanup", json::Value("0x24=0 与 0x21=0 无条件执行，出光失败也会收光停机"));
    plan.set("eeprom", json::Value(
        "本流程不写 0x31 保存 EEPROM：每打一发就固化一次参数会把任何写列表错误永久写进设备"));

    const libs::Result<services::FireRecord> result =
        runtime_.laser_fire().fire_and_capture(params);
    if (!result) {
        json::Value v = std::move(plan);
        v.set("fired", json::Value(false));
        v.set("error", json::Value(result.error().message));
        v.set("error_code", json::Value(result.error().code));
        v.set("warning", json::Value("失败时已尽力执行收光(0x24=0)与停机(0x21=0)，"
                                    "但请现场确认光闸确实已关闭"));
        runtime_.log().write(logging::RecordKind::exception_recovery, "出光流程失败", [&] {
            json::Value f = json::object();
            f.set("error_code", json::Value(result.error().code));
            f.set("error", json::Value(result.error().message));
            return f;
        }());
        return Response::failure(result.error());
    }

    const services::FireRecord& r = result.value();
    json::Value v = std::move(plan);
    v.set("fired", json::Value(r.ok));
    v.set("note", json::Value(r.note));
    v.set("capture_attempted", json::Value(r.capture_attempted));
    v.set("spectrum_groups", json::Value(static_cast<double>(r.spectrum_groups)));
    // 把本次采到的**原始光谱**一并回给界面。
    // 只回一个组数的话，操作员刚打完一发却看不到那条谱 —— 图上看不到，
    // 结果就只剩一个文件名。载荷含完整 wavelength_nm + counts，与落盘同一份。
    v.set("spectra", services::CaptureCoordinator::to_payload(r.spectra));
    v.set("confirmed_work_state", json::Value(r.confirmed_work_state));
    v.set("q_gate_closed_after", json::Value(!r.confirmed_q_gate_open));
    v.set("discharges", json::Value(static_cast<double>(r.discharges)));
    v.set("settle_ms", json::Value(static_cast<double>(r.settle_ms)));
    v.set("discharge_counter_before", json::Value(static_cast<double>(r.discharge_counter_before)));
    v.set("discharge_counter_after", json::Value(static_cast<double>(r.discharge_counter_after)));
    v.set("trigger_direction", json::Value(r.trigger_direction));
    v.set("trigger_output_programmed", json::Value(r.trigger_output_programmed));
    v.set("trigger_path", json::Value(
        r.trigger_direction == "spectrometer_to_laser"
            ? "Avantes Master HD-26 Pin 23 LASER OUT -> laser CLK IN"
            : "laser Q OUT -> Avantes Master HD-26 Pin 6 Trig In"));
    v.set("parameters_saved", json::Value(false));

    // ── 交付出口（能力 6.3.1）───────────────────────────────────────────────
    // 采到的原始光谱必须**真的落盘**：只把组数写进响应等于把数据丢在函数里，
    // 客户端不自己存，这一发的光谱就不存在了。
    {
        data_delivery::DeliveryStatus delivery;
        const libs::VoidResult saved = deliver_capture(r.spectra, delivery);
        // 落盘失败 = 这一发的数据丢了，属于失败，不能报成功。
        // 但要如实带上"激光确实打了"的事实，别让调用方以为出光没成。
        if (!saved) {
            Response resp = Response::failure(saved.error());
            resp.payload.set("fired", json::Value(true));
            resp.payload.set("note", json::Value(r.note));
            resp.payload.set("delivery", delivery_to_json(delivery));
            return resp;
        }
        v.set("delivery", delivery_to_json(delivery));
    }

    json::Value frames = json::array();
    for (const std::string& f : r.sent_frames) frames.as_array().push_back(json::Value(f));
    v.set("sent_frames", std::move(frames));

    runtime_.log().write(logging::RecordKind::state_change, "出光流程完成", [&] {
        json::Value f = json::object();
        f.set("spectrum_groups", json::Value(static_cast<double>(r.spectrum_groups)));
        f.set("discharges", json::Value(static_cast<double>(r.discharges)));
        f.set("settle_ms", json::Value(static_cast<double>(r.settle_ms)));
        f.set("counter_before", json::Value(static_cast<double>(r.discharge_counter_before)));
        f.set("counter_after", json::Value(static_cast<double>(r.discharge_counter_after)));
        f.set("parameters_saved", json::Value(false));
        return f;
    }());
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_spectrum_stream(const json::Value& args) {
    services::SpectrumStream& stream = runtime_.spectrum_stream();
    const std::string action = args.has("action") ? args["action"].as_string() : "status";

    // ── start ────────────────────────────────────────────────────────────
    if (action == "start") {
        // 限速与触发模式可由调用方覆盖，但**下限保护不可覆盖**：
        // 允许调用方把 period 设得比保护值大，不能设得更小。
        services::SpectrumStream::Options options;
        options.follow_site_trigger_mode =
            args.has("follow_site_trigger_mode") && args["follow_site_trigger_mode"].as_bool();
        // 预览节奏可由调用方放宽（不能收紧）：一帧是四通道 2048 点，
        // 序列化 + 过管道实测要 ~800ms，按 200ms 产帧的话客户端永远追不上，
        // 队列一路丢帧、界面一直空。允许调用方把它调慢到追得上的节奏。
        if (args.has("min_period_ms") && args["min_period_ms"].is_number()) {
            const double want = args["min_period_ms"].as_number();
            if (want > static_cast<double>(options.min_period_ms)) {
                options.min_period_ms = static_cast<unsigned>(want);
            }
        }
        stream.set_options(options);

        // 启停要碰设备（线程会立刻去采集），与 gate/capture 同一条串行化路径。
        std::lock_guard<std::mutex> lock(runtime_.device_mutex());
        const libs::VoidResult started = stream.start();
        if (!started) return Response::failure(started.error());

        json::Value v = stream.status();
        v.set("started", json::Value(true));
        v.set("warning", json::Value("预览默认自由触发：拍到的是背景谱/杂散光，不是 LIBS 测量谱；"
                                    "本命令不发任何激光写指令"));
        return Response::success(std::move(v));
    }

    // ── stop ─────────────────────────────────────────────────────────────
    if (action == "stop") {
        // stop 会 join 采集线程；不取 device_mutex 是有意的：
        // 采集线程自己会去取那把锁，这里再持着就是自己等自己（死锁）。
        stream.stop();
        json::Value v = stream.status();
        v.set("stopped", json::Value(true));
        return Response::success(std::move(v));
    }

    // ── latest ───────────────────────────────────────────────────────────
    // 非阻塞取一帧。**不取 device_mutex**：帧已经离开设备了，
    // 取帧不该和设备访问互相排队。
    if (action == "latest") {
        const double want = args.has("subscriber") ? args["subscriber"].as_number(-1.0) : -1.0;
        if (want < 0) {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "stream_subscriber_required",
                "latest 需要 subscriber 句柄；先用 action=subscribe 订阅"));
        }
        const std::uint64_t id = static_cast<std::uint64_t>(want);
        services::SpectrumFrame frame;
        if (!stream.try_pop(id, frame)) {
            // 没数据**不是错误**：这是"暂时还没采到"，UI 应当继续等。
            // 返回 ok=true + null 帧，语义与"采集失败"分开。
            json::Value v = json::object();
            v.set("has_frame", json::Value(false));
            v.set("pending", json::Value(static_cast<double>(stream.pending(id))));
            v.set("note", json::Value("订阅队列为空：本次没有新帧，不是错误"));
            return Response::success(std::move(v));
        }
        json::Value v = services::to_json(frame);
        v.set("has_frame", json::Value(true));
        v.set("pending", json::Value(static_cast<double>(stream.pending(id))));
        return Response::success(std::move(v));
    }

    // ── subscribe / unsubscribe ──────────────────────────────────────────
    if (action == "subscribe") {
        const std::uint64_t id = stream.subscribe();
        json::Value v = stream.status();
        v.set("subscriber", json::Value(static_cast<double>(id)));
        v.set("subscribed", json::Value(true));
        return Response::success(std::move(v));
    }
    if (action == "unsubscribe") {
        if (!args.has("subscriber") || !args["subscriber"].is_number()) {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "stream_subscriber_required",
                "unsubscribe 需要 subscriber 句柄"));
        }
        stream.unsubscribe(static_cast<std::uint64_t>(args["subscriber"].as_number()));
        json::Value v = stream.status();
        v.set("unsubscribed", json::Value(true));
        return Response::success(std::move(v));
    }

    // ── status（默认）─────────────────────────────────────────────────────
    if (action == "status") {
        return Response::success(stream.status());
    }

    return Response::failure(libs::Error::make(
        libs::ErrorKind::invalid_argument, "stream_bad_action",
        "action 只能是 start / stop / status / subscribe / unsubscribe / latest，收到: " + action));
}

// ── 连续出光 ─────────────────────────────────────────────────────────────────

Response BusinessFacade::cmd_laser_run(const json::Value& args) {
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());

    // 已经在连续出光就别重复开：重复发 0x21=1/0x24=1 本身没害处，
    // 但会让操作员以为"点了两次是两次不同的动作"，状态反而更乱。
    if (runtime_.laser_running()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::device, "laser_already_running",
            "连续出光已在运行；要停止请用 laser_stop"));
    }

    services::FireParameters params;
    libs::Error param_error{};
    if (!parse_fire_parameters(args, params, param_error)) {
        return Response::failure(param_error);
    }
    if (params.spectrometer_triggers_laser) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "reverse_trigger_continuous_unsupported",
            "laser_run 只支持激光器连续出光；光谱仪触发激光器必须使用有界 laser_fire 流程"));
    }

    const libs::Result<services::RunRecord> result =
        runtime_.laser_fire().run_continuous(params);
    if (!result) {
        runtime_.log().write(logging::RecordKind::exception_recovery, "连续出光启动失败", [&] {
            json::Value f = json::object();
            f.set("error_code", json::Value(result.error().code));
            f.set("error", json::Value(result.error().message));
            return f;
        }());
        return Response::failure(result.error());
    }

    runtime_.set_laser_running(true);
    const services::RunRecord& r = result.value();

    json::Value v = json::object();
    v.set("running", json::Value(true));
    v.set("note", json::Value(r.note));
    v.set("settle_ms", json::Value(static_cast<double>(r.settle_ms)));
    v.set("discharge_counter_at_start",
          json::Value(static_cast<double>(r.discharge_counter_at_start)));
    v.set("session_parameters", json::Value(
        "来自配置 laser_fire + 本次覆盖：电流 " + std::to_string(params.laser.discharge_current_a) +
        "A / 脉宽 " + std::to_string(params.laser.pulse_width_us) + "us / 频率 " +
        std::to_string(params.laser.frequency_hz) + "Hz / Q 延时 " +
        std::to_string(params.laser.q_delay_us) + "us"));
    v.set("warning", json::Value(
        "激光正在**持续出光**。停止请调用 laser_stop；守护进程退出时也会自动收光，"
        "但操作员离开前必须确认光闸已关"));
    json::Value frames = json::array();
    for (const std::string& f : r.sent_frames) frames.push_back(json::Value(f));
    v.set("sent_frames", std::move(frames));

    runtime_.log().write(logging::RecordKind::state_change, "连续出光开始", [&] {
        json::Value f = json::object();
        f.set("settle_ms", json::Value(static_cast<double>(r.settle_ms)));
        f.set("discharge_counter_at_start",
              json::Value(static_cast<double>(r.discharge_counter_at_start)));
        return f;
    }());
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_laser_stop(const json::Value& args) {
    (void)args;
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());

    // **无条件收光**：不管 Runtime 记的是不是"正在连续出光"。
    // 记的那个标志只是我们自己的账，设备当下是什么状态以读回为准；
    // 这一点上宁可多发一次 0x24=0/0x21=0，也不能因为"我以为没在跑"就不发。
    const libs::VoidResult stopped = runtime_.laser_fire().stop_continuous();
    const bool was_running = runtime_.laser_running();
    runtime_.set_laser_running(false);

    json::Value v = json::object();
    v.set("running", json::Value(false));
    if (!stopped) {
        v.set("warning", json::Value(
            "已下发收光与停机，但读回确认没通过 —— **请现场人工确认光闸确实已关闭**"));
        runtime_.log().write(logging::RecordKind::exception_recovery, "连续出光停止未确认", [&] {
            json::Value f = json::object();
            f.set("error", json::Value(stopped.error().message));
            return f;
        }());
        return Response::failure(stopped.error());
    }
    v.set("note", json::Value(was_running ? "连续出光已停止：光闸关、已停机，读回确认通过"
                                          : "已下发收光与停机并读回确认（此前未记录为运行中）"));
    runtime_.log().write(logging::RecordKind::state_change, "连续出光停止", [&] {
        json::Value f = json::object();
        f.set("was_running", json::Value(was_running));
        return f;
    }());
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_composition(const json::Value& args) {
    // 11.2 是 C 类：算法未实现。这里返回 algorithm_missing，不是全 0。
    const services::CompositionResult result = runtime_.composition().analyze(
        args.has("source") ? args["source"].as_string() : "unspecified");
    json::Value v = services::to_json(result);
    v.set("prerequisites", json::Value(services::CompositionAnalyzer::prerequisites()));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_recovery_list(const json::Value& args) {
    (void)args;
    const libs::Result<std::vector<std::string>> paths = runtime_.recovery().list();
    if (!paths) return Response::failure(paths.error());
    json::Value v = json::object();
    json::Array arr;
    for (const auto& p : paths.value()) arr.push_back(json::Value(p));
    v.set("snapshots", std::move(arr));
    v.set("note", json::Value("快照是观测记录，不是命令队列；不得据此自动重发运动或触发"
                              "（任务书 §6.2）"));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_recovery_write(const json::Value& args) {
    logging::RecoveryContext context;
    context.snapshot_at_ms = wall_clock_ms();
    context.machine_state = policy::to_string(runtime_.gate().state());
    const policy::GateVerdict verdict = runtime_.gate().peek();
    for (policy::GateReason r : verdict.blocked_by) context.blocked_by.push_back(policy::to_string(r));
    context.gate_allowed = verdict.allowed;
    context.target_belt = runtime_.target_belt();
    context.detection_stage = events::to_string(runtime_.stage());
    context.belt_sequence = runtime_.belt_sequence();
    context.reason = args.has("reason") && args["reason"].is_string() ? args["reason"].as_string()
                                                                    : "手工快照";
    if (runtime_.parameters() != nullptr) {
        json::Value params = json::object();
        for (const auto& group : runtime_.parameters()->all()) {
            json::Value values = json::object();
            for (const auto& f : group.second.values) values.set(f.first, json::Value(f.second));
            params.set(group.first, std::move(values));
        }
        context.last_parameters = std::move(params);
    }
    if (runtime_.last_delivery().has_value() && runtime_.last_delivery()->local_saved) {
        context.last_saved_capture_path = runtime_.last_delivery()->local_path;
    }
    // 注意这里**不做任何恢复动作**：只落一份快照。
    const libs::Result<std::string> path = runtime_.recovery().write(context);
    if (!path) return Response::failure(path.error());
    json::Value v = json::object();
    v.set("path", json::Value(path.value()));
    v.set("written", json::Value(true));
    v.set("note", json::Value("只写快照，不执行任何恢复动作（任务书 §6.2）"));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_heike_report_preview(const json::Value& args) {
    // 这是报文构造/契约检查入口，不是网络发送入口。附件里的边端地址和
    // Bearer token 仍需配置。LIBS 结果复用近红外反馈结构，数值必须来自真实分析。
    const std::string kind = args.has("kind") ? args["kind"].as_string() : "";
    if (kind.empty()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "heike_report_kind_required",
            "heike_report_preview 需要 kind=heartbeat/event/openinit/alarm/composition/model/libs_raw"));
    }

    libs::Result<json::Value> payload = libs::Error::make(
        libs::ErrorKind::invalid_argument, "heike_report_not_built", "未构造报文");
    std::string event_name;

    // JSON numbers are stored as double in the project JSON layer.  Do not
    // cast an unchecked value to int64_t: NaN/Inf, fractions, or values above
    // the exact-integer range can otherwise produce undefined conversions.
    const auto read_json_i64 = [&](const json::Value& value, const char* field,
                                   bool non_negative, std::int64_t& out) -> bool {
        constexpr double kMaxExactJsonInteger = 9007199254740991.0;  // 2^53 - 1
        const double number = value.as_number(std::numeric_limits<double>::quiet_NaN());
        if (!value.is_number() || !std::isfinite(number) || number != std::floor(number) ||
            number < (non_negative ? 0.0 : -kMaxExactJsonInteger) ||
            number > kMaxExactJsonInteger) {
            payload = libs::Error::make(
                libs::ErrorKind::invalid_argument, "heike_integer_bad",
                std::string(field) + " must be a finite " +
                    (non_negative ? "non-negative " : "") + "integer");
            return false;
        }
        out = static_cast<std::int64_t>(number);
        return true;
    };

    if (kind == "heartbeat" || kind == "event" || kind == "openinit" || kind == "alarm") {
        integration::heike::DeviceIdentity identity;
        identity.equipment_id = args["equipment_id"].as_string();
        identity.equipment_type = args["equipment_type"].as_string();
        identity.customs = args["customs"].as_string();
        identity.timeseries = args["timeseries"].as_string();

        if (kind == "heartbeat") {
            event_name = "HEARTBEAT";
            payload = integration::heike::make_heartbeat(identity);
        } else if (kind == "event") {
            event_name = "EVENT";
            payload = integration::heike::make_runtime_event(
                identity, args["code"].as_string(), args["value"].as_string());
        } else if (kind == "openinit") {
            event_name = "OPENINIT";
            if (!args.has("stats") || !args["stats"].is_array()) {
                payload = libs::Error::make(libs::ErrorKind::invalid_argument,
                                             "heike_stats_required",
                                             "openinit 需要 stats 数组");
            } else {
                std::vector<integration::heike::DailyStat> stats;
                bool stats_valid = true;
                for (const json::Value& item : args["stats"].as_array()) {
                    if (!item.is_object()) {
                        payload = libs::Error::make(libs::ErrorKind::invalid_argument,
                                                    "heike_stat_bad_item",
                                                    "stats 每项必须是对象");
                        stats_valid = false;
                        break;
                    }
                    integration::heike::DailyStat stat;
                    stat.date = item["date"].as_string();
                    if (!read_json_i64(item["count"], "stats.count", true, stat.count)) {
                        stats_valid = false;
                        break;
                    }
                    // The reference protocol calls this field onlinePeriod. Keep
                    // the old snake_case spelling as a compatibility alias for
                    // existing UI callers, but emit the protocol spelling.
                    const json::Value& period = item.has("onlinePeriod")
                                                    ? item["onlinePeriod"]
                                                    : item["online_period_s"];
                    if (!read_json_i64(period, "stats.onlinePeriod", true,
                                       stat.online_period_s)) {
                        stats_valid = false;
                        break;
                    }
                    stats.push_back(std::move(stat));
                }
                if (stats_valid)
                    payload = integration::heike::make_openinit(identity, stats);
            }
        } else {
            event_name = "ALARM";
            if (!args.has("alarms") || !args["alarms"].is_array()) {
                payload = libs::Error::make(libs::ErrorKind::invalid_argument,
                                             "heike_alarms_required",
                                             "alarm 需要 alarms 数组");
            } else {
                std::vector<integration::heike::Alarm> alarms;
                bool alarms_valid = true;
                for (const json::Value& item : args["alarms"].as_array()) {
                    if (!item.is_object()) {
                        payload = libs::Error::make(libs::ErrorKind::invalid_argument,
                                                    "heike_alarm_bad_item",
                                                    "alarms 每项必须是对象");
                        alarms_valid = false;
                        break;
                    }
                    integration::heike::Alarm alarm;
                    alarm.name = item["name"].as_string();
                    alarm.type = item["type"].as_string();
                    alarm.color = item["color"].as_string();
                    alarm.firsttime = item["firsttime"].as_string();
                    alarm.time = item["time"].as_string();
                    alarm.value = item["value"].as_string();
                    alarm.remake = item["remake"].as_string();
                    alarms.push_back(std::move(alarm));
                }
                if (alarms_valid)
                    payload = integration::heike::make_alarm(identity, alarms);
            }
        }
    } else if (kind == "libs_raw") {
        event_name = args["event_name"].as_string();
        integration::heike::LibsCaptureContext context;
        context.biz_id = args["biz_id"].as_string();
        context.customs_code = args["customs_code"].as_string();
        context.belt_code = args["belt_code"].as_string();
        context.edge_code = args["edge_code"].as_string();
        context.collect_time = args["collect_time"].as_string();
        bool seq_valid = true;
        if (args.has("seq_no") &&
            !read_json_i64(args["seq_no"], "seq_no", true, context.seq_no))
            seq_valid = false;
        context.event_source = args.has("event_source")
                                   ? args["event_source"].as_string()
                                   : "LIBS-device";
        context.message_id = args["message_id"].as_string();
        context.event_code = args["event_code"].as_string();
        if (seq_valid)
            payload = integration::heike::make_libs_raw_event(
                context, args["raw_capture"], args["event_content_as_string"].as_bool(false));
    } else if (kind == "model" || kind == "composition") {
        integration::heike::EnvelopeInput input;
        input.biz_id = args["biz_id"].as_string();
        input.customs_code = args["customs_code"].as_string();
        input.event_code = kind == "composition" && !args.has("event_code")
                               ? "PISYS001" : args["event_code"].as_string();
        input.event_source = args["event_source"].as_string();
        input.message_id = args["message_id"].as_string();
        input.event_content = args["event_content"];
        input.event_content_as_string = args["event_content_as_string"].as_bool(false);
        input.notes = args["notes"].as_string();
        input.version = args["version"].as_string();
        if (kind == "composition") {
            // PISYS001 belongs to the inspection application's result interface.
            // Reusing its schema does not create a device HTTP upload route.
            event_name = "PISYS001";
            payload = integration::heike::make_composition_event(input);
        } else {
            event_name = integration::heike::model_event_name(input.event_code);
            payload = integration::heike::make_model_event(input);
        }
    } else {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::invalid_argument, "heike_report_bad_kind",
            "kind 只能是 heartbeat/event/openinit/alarm/composition/model/libs_raw"));
    }

    if (!payload) return Response::failure(payload.error());
    json::Value v = json::object();
    v.set("transport", json::Value("dry_run"));
    v.set("sent", json::Value(false));
    v.set("event_name", json::Value(event_name));
    if (kind == "composition") {
        // Completeness and acquisition source are local diagnostics, not new
        // fields in the shared upstream message. Unknown results remain null.
        json::Value content;
        const auto parsed = json::parse(payload.value()["eventContent"].as_string(), content);
        if (!parsed.ok) return Response::failure(libs::Error::make(
            libs::ErrorKind::internal, "heike_composition_invalid",
            "成分报文构造器返回了不可解析的 eventContent"));
        json::Value missing = json::array();
        for (const char* key : {"COMPO_AL2O3", "COMPO_CAO", "COMPO_FEO", "COMPO_MGO",
                                "COMPO_MNO", "COMPO_P2O5", "COMPO_S", "COMPO_SIO2",
                                "COMPO_TFE", "COMPO_TIO2"}) {
            if (content[key].is_null()) missing.push_back(json::Value(key));
        }
        v.set("measurement_method", json::Value("LIBS"));
        v.set("composition_complete", json::Value(missing.as_array().empty()));
        v.set("missing_components", std::move(missing));
        v.set("data_source_mapped", json::Value(!content["DATA_SOURCE"].as_string().empty()));
    }
    v.set("payload", std::move(payload.value()));
    v.set("gateway_enabled", json::Value(runtime_.config().gateway.enabled));
    v.set("warning", json::Value(
        "仅构造预览，未访问外部边端；LIBS 分析结果复用近红外反馈结构，缺失值不补零"));
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_health(const json::Value& args) {
    (void)args;
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());
    return Response::success(runtime_.health_json());
}

}  // namespace business::app
