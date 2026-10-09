#include "libs/business/app/facade.hpp"

#include <cmath>
#include <sstream>
#include <utility>

#include "libs/business/measurement/frame.hpp"
#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/job.hpp"
#include "libs/business/readiness/monitor.hpp"
#include "libs/business/readiness/precheck.hpp"
#include "libs/business/readiness/verdict.hpp"
#include "libs/business/shared/data_delivery.hpp"
#include "libs/business/shared/time_util.hpp"

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

Response Response::failure_with_payload(libs::Error error, json::Value report) {
    Response r = failure(std::move(error));
    r.payload.set("report", std::move(report));
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

json::Value gate_snapshot_json(const readiness::ReadinessMonitor& monitor) {
    // **只读缓存，绝不重新判定。**
    //
    // status / gate 是给界面看的两条查询命令。它们如果在这里再跑一次
    // evaluate_now()，就会变成判定的第二个执行点 —— 而正确形态是
    // "measurement_start 成功即已开光，失败看返回码"。UI 不参与决策。
    //
    // 读不到 B2 的事实也照实报：readiness 已经用它判成 laser_unavailable，
    // 这里只是让面板能区分"是没读到"还是"读到了但不安全"。
    const readiness::GateVerdict& verdict = monitor.latest();
    json::Value blocked = json::array();
    for (readiness::GateReason reason : verdict.blocked_by) {
        blocked.push_back(json::Value(readiness::to_string(reason)));
    }
    json::Value v = json::object();
    v.set("allowed", json::Value(verdict.allowed));
    v.set("blocked_by", std::move(blocked));
    v.set("detail", json::Value(verdict.detail));
    v.set("state", json::Value(readiness::to_string(monitor.latest_state())));
    v.set("armed", json::Value(monitor.armed()));
    v.set("laser_readable", json::Value(monitor.laser_readable()));
    return v;
}

libs::VoidResult BusinessFacade::deliver_capture(
        const std::vector<libs::devices::ScanGroup>& groups,
        data_delivery::DeliveryStatus& status_out) {
    data_delivery::CaptureReference ref;
    ref.station_id = runtime_.config().station_id;
    ref.captured_at_ms = wall_clock_ms();
    if (!groups.empty()) {
        ref.shot_id = groups.front().shot_id;
        ref.hardware_synchronized = groups.front().hardware_synchronized;   // 原样上送，不推断
        for (const auto& group : groups)
            for (const auto& spectrum : group.spectra) ref.serials.push_back(spectrum.serial);
    }

    // 载荷含完整 wavelength_nm + counts：本地交付存的是**原始光谱**。
    const std::string blob =
        json::stringify(data_delivery::to_capture_payload(groups));
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
                                           measurement::FireParameters& out,
                                           libs::Error& error) {
    // 薄封装：**参数怎么合并、怎么校验，全部由 ParameterService 负责**。
    //
    // 这一块以前是门面自己实现的，于是有了两份规则：面面一套量程校验、
    // 出光服务又一套。而出光服务那条无参入口根本不校验，直接读配置 ——
    // 于是"界面限 100 A、请求能传 500 A"这种破口能绕过一半。
    //
    // 现在只有一套：门面只负责把 JSON 递进去，把结果取出来。
    const libs::Result<measurement::FireParameters> r =
        runtime_.parameter_service().fire_parameters(args);
    if (!r) {
        error = r.error();
        return false;
    }
    out = r.value();
    return true;
}

std::vector<std::string> BusinessFacade::commands() {
    return {
        "ping",
        "config_show",
        "status",
        "gate",
        // 业务一：显式跑一次完整预检（布防 -> 判定 -> 刷灯）。
        // 与 gate 的分工：gate 只读缓存 / 单独布防撤防，precheck 才是
        // "按钮按下去立刻判一次"的那条路径。
        "precheck",
        "param_set",
        "param_get",
        "capture",
        "recovery_list",
        "recovery_write",
        "health",
        "shutdown",
        "measurement_start",
        "measurement_stop",
        "measurement_status",
        // 只剩"显示通道"了。start/stop 动作**故意不再提供** ——
        // 采集只有一个来源，就是 measurement_start。以前这个命令同时管
        // 采集与显示，于是 UI 以为"点开始 = 开工"，实际可能拍出一堆
        // 背景谱而操作员以为是测量值。
        "spectrum_stream",
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
    if (command == "precheck")        return cmd_precheck(args);
    if (command == "param_set")       return cmd_param_set(args);
    if (command == "param_get")       return cmd_param_get(args);
    if (command == "shutdown") {
        runtime_.request_shutdown();
        json::Value v = json::object(); v.set("shutdown_requested", json::Value(true));
        return Response::success(std::move(v));
    }
    if (command == "capture")         return cmd_capture(args);

    if (command == "spectrum_stream") return cmd_spectrum_stream(args);
    if (command == "measurement_start")  return cmd_measurement_start(args);
    if (command == "measurement_stop")   return cmd_measurement_stop(args);
    if (command == "measurement_status") return cmd_measurement_status(args);
    if (command == "recovery_list")   return cmd_recovery_list(args);
    if (command == "recovery_write")  return cmd_recovery_write(args);
    if (command == "health")          return cmd_health(args);
    return Response::failure(libs::Error::make(
        libs::ErrorKind::unsupported, "unknown_command",
        "未知命令: " + command + "；可用命令见 commands 字段。" +
         "注意：出光只有 measurement_start 一条路径（原 laser_fire / laser_run / laser_stop 已删除）"));
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
    return Response::success(config::to_json(runtime_.config()));
}

Response BusinessFacade::cmd_status(const json::Value& args) {
    (void)args;
    // 门禁汇总只读内存里的缓存结论（gate_snapshot_json），**不触发设备读取**，
    // 因此不需要 device_mutex。status 是诊断命令，绝不能因为它而排队设备访问。
    const readiness::ReadinessMonitor* monitor = runtime_.monitor();
    if (monitor == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "readiness_not_ready",
            "就绪判定尚未装配（守护进程未完成启动）"));
    }

    json::Value v = json::object();
    v.set("running", json::Value(runtime_.running()));
    v.set("gate", gate_snapshot_json(*monitor));
    // 出光会话是否在进行。界面靠它决定「开始检测 / 停止」哪个可点。
    v.set("laser_running", json::Value(runtime_.laser_running()));
    v.set("health", runtime_.health_json());
    v.set("log_records", json::Value(static_cast<double>(runtime_.log().count())));

    // 状态灯：灯色意图与上次下发结果。
    //
    // **灯是门禁之上的纯函数**：这里只用于诊断"灯现在显示出什么"。
    // lamp_applied=false 只说明灯这条串口有问题，与"能不能出光"无关。
    // 预检未装配时**如实说没有**，不要塞一个假的默认灯色 ——
    // "灯是绿的"是现场据以判断安全与否的信息，编不得。
    if (const readiness::PrecheckService* pc = runtime_.precheck()) {
        json::Value lamp = json::object();
        lamp.set("intent", json::Value(pc->lamp_intent()));
        lamp.set("applied", json::Value(pc->last_report().lamp_applied));
        const std::string& lamp_error = pc->last_report().lamp_error;
        if (!lamp_error.empty()) {
            lamp.set("last_error", json::Value(lamp_error));
        }
        v.set("indicator", std::move(lamp));
    } else {
        v.set("indicator", json::Value());   // 尚未装配，不谎报灯色
    }

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
    blocked.push_back(json::Value("gateway_delivery: " + data_delivery::UnsupportedGatewaySink::prerequisites()));
    v.set("unavailable_capabilities", std::move(blocked));
    return Response::success(std::move(v));
}

// `gate` 命令：布防 / 撤防 / 手动刷新一次判定。**纯运维与诊断入口**。
//
// ── 它绝不能变成"开始检测"的前置步骤 ──────────────────────────────────
//
// 本命令只回答"现在能不能出光"并把结论刷到灯上，**不做任何出光动作**。
// 判定真正的执行点仍然是 MeasurementJob::start()。如果界面改成
// "先 gate refresh 拿到绿灯，再发 measurement_start"，判定就出现了两个执行点 ——
// 那是方案 §2 约束 1 明令禁止的形状。
Response BusinessFacade::cmd_gate(const json::Value& args) {
    readiness::ReadinessMonitor* monitor = runtime_.monitor();
    if (monitor == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "readiness_not_ready",
            "就绪判定尚未装配（守护进程未完成启动）"));
    }

    // 参数齐备是一条**配置事实**，可以由调用方同步进来。
    // 注意这里**不重跑判定**：改完参数下次判定自然会用上，
    // 立刻重跑只会让"改参数"顺带变成一次设备读。
    if (args.has("parameters_ready") && args["parameters_ready"].is_bool()) {
        monitor->set_parameters_ready(args["parameters_ready"].as_bool());
    }

    if (args.has("action")) {
        const std::string action = args["action"].as_string();
        // **刻意不取 device_mutex**：事实源 lambda 自己会去拿那把锁，
        // 外层再拿一次就是同一线程重复加锁 —— std::mutex 不自锁，直接死锁。
        if (action == "arm") {
            const libs::VoidResult r = monitor->arm();   // 内部会先 evaluate 一次
            if (!r) return Response::failure(r.error());
        } else if (action == "disarm") {
            const libs::VoidResult r = monitor->disarm();
            if (!r) return Response::failure(r.error());
        } else if (action == "refresh") {
            monitor->evaluate_now();   // 返回值即最新结论，下面统一回显
        } else {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "gate_bad_action",
                "action 只能是 arm / disarm / refresh，收到: " + action));
        }
    }
    return Response::success(gate_snapshot_json(*monitor));
}

// "precheck" 命令：显式跑一次完整设备预检。**全库唯一能被客户端主动触发的判定入口。**
//
// -- 它与 gate 的分工（两条不同的路径，别混用）--------------------------
//
//   * gate     -- 运维/诊断入口。只读缓存结论，或单独布防/撤防，
//                 顺手把结论刷到灯上。它不组装预检报告。
//   * precheck -- 操作员入口。一次完成"布防 -> 判定 -> 按结论刷灯"，
//                 并把**完整报告**回给调用方。
//
// -- 它绝不成为"开始检测"的前置步骤-----------------------------------
//
// 判定真正的执行点仍然只有 MeasurementJob::start()。如果界面改成
// "先 precheck 拿到绿灯，再发 measurement_start"，判定就出现了第二个执行点
// —— 那是方案 §2 约束 1 明令禁止的形状，而且更隐蔽：precheck 走的是
// 预检服务，measurement_start 走的是 monitor，两条路径读的事实可能不是同一时刻。
// 正确形态始终是：measurement_start 一次成功或失败。
Response BusinessFacade::cmd_precheck(const json::Value& args) {
    // 预检没有可调参数：布防、判定、刷灯三件事都是固定的。
    // 保留形参是为了和其他 cmd_* 签名一致（路由表按同一形状调用）。
    (void)args;

    readiness::PrecheckService* pc = runtime_.precheck();
    if (pc == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "precheck_not_ready",
            "预检服务尚未装配（守护进程未完成启动）"));
    }

    // 参数齐备是一条**配置事实**，同步给 monitor。
    // **不重跑判定**：run() 下面马上就会判一次，在那之前改事实只会让
    // 这一次判定的输入来路不明。与 cmd_param_set / cmd_gate 同一条纪律。
    if (readiness::ReadinessMonitor* monitor = runtime_.monitor()) {
        if (config::ParameterStore* store = runtime_.parameters()) {
            monitor->set_parameters_ready(store->readiness().ready);
        }
    }

    // run() 内部：arm() -> evaluate_now() -> 按结论刷灯。
    // **它一条激光写指令都不发** —— 读激光状态可以，写不行。
    const libs::VoidResult r = pc->run();
    json::Value report = pc->to_json(pc->last_report());

    if (!r) {
        // **失败也带完整报告。** 门禁不放行时操作员要的是"哪几条不满足"，
        // 而不是一句"预检失败"。所以用 failure_with_payload 把报告挂在
        // payload.report 下 —— 顶层 error 键保持 failure() 的原样，两者不打架。
        return Response::failure_with_payload(r.error(), std::move(report));
    }
    return Response::success(std::move(report));
}

Response BusinessFacade::cmd_param_set(const json::Value& args) {
    config::ParameterStore* store = runtime_.parameters();
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
    // 参数改动**立即生效**。曾经有 at_next_idle（暂存到下次空闲）这条路，
    // 但全库没有任何 commit 点去应用暂存值 —— 结果是"界面改了参数，
    // 这次采集还是用旧值"。那是个真 bug，不是特性，已连同暂存机制一起删除。
    std::map<std::string, double> values;
    for (const auto& kv : args["values"].as_object()) {
        if (!kv.second.is_number()) {
            return Response::failure(libs::Error::make(
                libs::ErrorKind::invalid_argument, "param_value_not_number",
                "字段 " + kv.first + " 的值必须是数字（缺失/unknown 用不上参数请省略该字段）"));
        }
        values[kv.first] = kv.second.as_number();
    }



    // ── 量程校验：写入前先判，不合格的值根本不该落盘 ───────────────────
    //
    // 参数库此前只拦 NaN/Inf，**不拦越界**。方案 B 把 commissioned 改成
    // "按参数库内容算"之后这就成了真破口：操作员把电流填成 9999，
    // 界面回显"参数设置已完成"，但 commissioned_from_store() 判 false，
    // 门禁报"出光参数未标定" —— **一个字都不告诉操作员错在哪**。
    // 填了值却查不出原因，比直接报错难查得多。
    //
    // 逐字段判（见下）：只判本次传入的字段，缺失的字段不参与判定。
    //
    const std::string group = args["group"].as_string();
    if (group == "laser") {
        // 量程判据只有一份：下面这张表与 config::validate_laser_fire() 一一对应，
        // 数字改了要两处一起改（它们就在同一个仓库里，同一次构建）。
        // 不用 validate_laser_fire() 的原因有二：它在 commissioned=false 时
        // 整段放行（未标定是合法中间态），而这里正是"还没标定"；而且它按
        // **整组**判，会把还没填的框按 0 一起判掉 —— 操作员一个框一个框填，
        // 第二个框的合法值被第一个空框的 0 判成越界，报的还是别人的错。
        // 所以这里**只判本次传入的字段**：填哪个就校哪个，缺的不管。
        struct Range { const char* field; double lo; double hi; const char* unit; };
        static const Range kRanges[] = {
            {"discharge_current_a",  15,  100, "A"},
            {"pulse_width_us",       50,  300, "us"},
            {"frequency_hz",          1,   10, "Hz"},
            {"discharge_delay_us",    0,  500, "us"},
            {"q_delay_us",            2,  500, "us"},
            {"q_warmup_s",            0, 3600, "s"},
            {"settle_timeout_ms",     1, 600000, "ms"},
            {"emission_hold_ms",      0, 600000, "ms"},
            {"min_discharges",        1, 100000, ""},
        };
        for (const Range& r : kRanges) {
            const auto it = values.find(r.field);
            if (it == values.end()) continue;   // 本次没填这个字段，不判
            const double v = it->second;
            std::string reason;
            if (v < 0.0 || v != std::floor(v)) {
                reason = std::string("必须是 0 或正整数");
            } else if (v < r.lo || v > r.hi) {
                reason = std::to_string(static_cast<int>(r.lo)) + "~" +
                         std::to_string(static_cast<int>(r.hi)) + " " + r.unit;
            }
            if (reason.empty()) continue;
            const std::string message =
                "参数被拒绝：" + std::string(r.field) + " " + reason +
                "（协议 §3.1 量程）";
            json::Value rep = json::object();
            rep.set("state", json::Value(config::to_string(config::ApplyState::rejected)));
            rep.set("completed", json::Value(false));
            rep.set("group", json::Value(group));
            rep.set("message", json::Value(message));
            json::Array ff;
            ff.emplace_back(std::string(r.field));
            rep.set("failed_fields", json::Value(std::move(ff)));
            return Response::failure_with_payload(
                libs::Error::make(libs::ErrorKind::invalid_argument, "param_out_of_range", message),
                std::move(rep));
        }
    }
    if (group == "spectrometer") {
        // 光谱侧同理：积分时间/超时越界现在就拒，不让操作员存进去
        // 再在采集时才看到"驱动拒绝"。驱动自己的硬校验是 [0.002, 60000] ms
        //（见 spectrometer.cpp），这里与它保持一致。
        const auto it_i = values.find("integration_ms");
        if (it_i != values.end() &&
            !(it_i->second >= 0.002 && it_i->second <= 60000.0)) {
            json::Value v = json::object();
            v.set("state", json::Value(config::to_string(config::ApplyState::rejected)));
            v.set("completed", json::Value(false));
            v.set("group", json::Value(group));
            v.set("message", json::Value("参数被拒绝：integration_ms 必须在 0.002~60000 ms 之间"));
            json::Array failed;
            failed.emplace_back(std::string("integration_ms"));
            v.set("failed_fields", json::Value(std::move(failed)));
            return Response::failure_with_payload(
                libs::Error::make(libs::ErrorKind::invalid_argument, "param_out_of_range",
                                  "参数被拒绝：integration_ms 必须在 0.002~60000 ms 之间"),
                std::move(v));
        }
    }
    const config::ApplyResult result = store->apply(group, values);
    json::Value v = json::object();
    v.set("state", json::Value(config::to_string(result.state)));
    v.set("completed", json::Value(result.completed()));
    v.set("group", json::Value(result.group));
    v.set("message", json::Value(result.message));
    json::Array failed;
    for (const auto& f : result.failed_fields) failed.emplace_back(f);
    v.set("failed_fields", json::Value(std::move(failed)));
    v.set("note", json::Value("completed=false 表示“收到请求但未完成”；"
                              "调用方必须检查它，不得把非 completed 当成成功（任务书 §4.2）"));

    // 参数齐备与否驱动门禁（3.4.1）。这是参数模块与门禁之间唯一的耦合点。
    const config::ReadinessReport readiness = store->readiness();
    json::Value r = json::object();
    r.set("ready", json::Value(readiness.ready));
    r.set("summary", json::Value(readiness.summary));
    v.set("readiness", std::move(r));
    // 参数库这条视图与门禁那条必须一致。这里只同步**事实**，不重跑判定 ——
    // 判定何时跑由 monitor / MeasurementJob 决定，不由查询命令决定。
    if (readiness::ReadinessMonitor* monitor = runtime_.monitor()) {
        monitor->set_parameters_ready(readiness.ready);
    }
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_param_get(const json::Value& args) {
    config::ParameterStore* store = runtime_.parameters();
    if (store == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::internal, "parameters_absent", "参数存储未初始化"));
    }
    json::Value v = json::object();
    if (args.has("group") && args["group"].is_string()) {
        std::map<std::string, double> out;
        const config::ApplyResult r = store->query(args["group"].as_string(), out);
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
    const config::ReadinessReport readiness = store->readiness();
    json::Value r = json::object();
    r.set("ready", json::Value(readiness.ready));
    r.set("summary", json::Value(readiness.summary));
    v.set("readiness", std::move(r));
    // param_get 与 param_set 读的是同一份存储，两处的"参数齐备"必须一致。
    // 缺这一行会出现：参数已落盘、重启后 param_get 报"核心参数齐备"，
    // 门禁却仍是 parameters_incomplete（因为只有 param_set 才同步过门禁）。
    // 两个视图互相矛盾正是本项目 §6.4 记过的最难排障的一类缺陷。
    // 参数库这条视图与门禁那条必须一致。这里只同步**事实**，不重跑判定 ——
    // 判定何时跑由 monitor / MeasurementJob 决定，不由查询命令决定。
    if (readiness::ReadinessMonitor* monitor = runtime_.monitor()) {
        monitor->set_parameters_ready(readiness.ready);
    }
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_capture(const json::Value& args) {
    (void)args;
    std::unique_lock<std::mutex> acquisition(runtime_.acquisition_mutex(), std::try_to_lock);
    if (!acquisition.owns_lock() || runtime_.laser_session_active())
        return Response::failure(libs::Error::make(libs::ErrorKind::device,
            "acquisition_busy", "测量已占用光谱仪，独立采集被拒绝"));
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());

    // 测量参数**一律**来自参数库（参数管理服务），不参与任何推导 ——
    // 否则同一份积分时间会有两个来源。
    //
    // 注意这里走 spectrometer_measurement() 而不是 fire_parameters()：
    // **单次采集不出光**，所以不该被"激光器未标定"拦住。fire_parameters()
    // 会连 laser 组一起校验，未标定时返回 laser_not_commissioned ——
    // 那对一条只想拍一张光谱的命令是误伤。
    const measurement::SpectrumMeasurement measurement =
        runtime_.parameter_service().spectrometer_measurement();

    // acquisition_mutex and laser session ownership exclude the measurement worker.
    // Display subscriptions remain pure memory operations and never acquire this lock.

    libs::devices::Spectrometer* device = runtime_.devices().spectrometer();
    if (device == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "spectrometer_not_open",
            "光谱仪未打开或未启用，无法采集"));
    }
    // 请求就地构造（原 CaptureCoordinator 已删）：通道/主从/SYNC/是否等触发
    // 是**装机配置**（接法事实），积分时间/超时/积分延时是**测量参数**（参数库）。
    const config::SiteConfig& site = runtime_.config();
    libs::devices::CaptureRequest request;
    request.serials = site.spectrometer.serials;
    request.master_serial = site.spectrometer.master_serial;
    request.synchronized = site.spectrometer.synchronized;
    request.integration_ms = measurement.integration_ms;
    request.timeout_ms = measurement.timeout_ms;
    request.integration_delay_ns = measurement.integration_delay_ns;
    request.scans = 1;
    request.wait_for_laser_trigger = site.spectrometer.wait_for_laser_trigger;
    const libs::Result<libs::devices::CaptureResult> result = device->captureOnce(request);
    if (!result) {
        return Response::failure(result.error());
    }
    json::Value v = json::object();
    v.set("expected_groups", json::Value(static_cast<double>(result.value().expected_groups)));
    v.set("received_groups", json::Value(static_cast<double>(result.value().received_groups)));
    v.set("measurement_source", json::Value("parameters.json"));
    json::Array syncs;
    for (const auto& group : result.value().groups) {
        json::Value g = json::object();
        g.set("shot_id", json::Value(static_cast<double>(group.shot_id)));
        g.set("complete", json::Value(group.complete));
        // 原样上送，不推断：SDK 报什么就是什么。
        g.set("hardware_synchronized", json::Value(group.hardware_synchronized));
        g.set("requested_sync", json::Value(runtime_.config().spectrometer.synchronized));
        json::Value order = json::array();
        for (const auto& s : group.arm_order) order.push_back(json::Value(s));
        g.set("arm_order", std::move(order));
        syncs.push_back(std::move(g));
    }
    v.set("scan_groups", std::move(syncs));
    v.set("note", json::Value("hardware_synchronized 原样上送 SDK 事实；"
                              "业务层不宣称物理同步（范围报告 §3.10.1）"));

    // ── 交付出口（能力 6.3.1）───────────────────────────────────────────────
    // 采集到光谱的命令**每一条都要落盘**，不能一条接了、另一条漏了 ——
    // 漏的那条就是"数据只活在管道响应里"。落盘由 MeasurementJob 内部的
    // Writer 负责；单次 capture 没有 Writer，直接走这一处。
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

Response BusinessFacade::cmd_spectrum_stream(const json::Value& args) {
    // ── 本命令现在是**纯显示通道**，不采集 ─────────────────────────────────
    //
    // 它以前还负责 start/stop 一个后台采集线程，那部分已经搬进
    // MeasurementJob（`measurement_start` / `measurement_stop`）。这里的动作集
    // 缩到 "订阅 / 退订 / 取帧 / 查状态"，UI 的订阅与轮询代码一行都不用改。
    //
    // **故意不再接受 start / stop**：采集只有一个入口，留两条路一定会有人
    // 走错那条 —— 而走错的后果是"以为在测量，实际在拍背景谱"。
    measurement::FrameQueue& queue = runtime_.frame_bus();
    const std::string action = args.has("action") ? args["action"].as_string() : "status";

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
        measurement::SpectrumFrame frame;
        if (!queue.try_pop(id, frame)) {
            // 没数据**不是错误**：这是"暂时还没采到"，UI 应当继续等。
            // 返回 ok=true + null 帧，语义与"采集失败"分开。
            json::Value v = json::object();
            v.set("has_frame", json::Value(false));
            v.set("pending", json::Value(static_cast<double>(queue.pending(id))));
            v.set("note", json::Value("订阅队列为空：本次没有新帧，不是错误"));
            return Response::success(std::move(v));
        }
        json::Value v = measurement::to_json(frame);
        v.set("has_frame", json::Value(true));
        v.set("pending", json::Value(static_cast<double>(queue.pending(id))));
        return Response::success(std::move(v));
    }

    // ── subscribe / unsubscribe ──────────────────────────────────────────
    if (action == "subscribe") {
        const std::uint64_t id = queue.subscribe();
        json::Value v = queue.status();
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
        queue.unsubscribe(static_cast<std::uint64_t>(args["subscriber"].as_number()));
        json::Value v = queue.status();
        v.set("unsubscribed", json::Value(true));
        return Response::success(std::move(v));
    }

    // ── status（默认）─────────────────────────────────────────────────────
    if (action == "status") {
        return Response::success(queue.status());
    }

    if (action == "start" || action == "stop") {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "stream_no_start_stop",
            "spectrum_stream 已不再负责采集，没有 start/stop 可调。"
            "要开工请用 measurement_start（连续出光 + 采集 + 落盘 + 显示），"
            "要停请用 measurement_stop；本命令只做 subscribe / unsubscribe / latest / status"));
    }

    return Response::failure(libs::Error::make(
        libs::ErrorKind::invalid_argument, "stream_bad_action",
        "action 只能是 status / subscribe / unsubscribe / latest，收到: " + action));
}

// ── 连续出光测量（出光 + 采集 + 落盘 + 显示）─────────────────────────────

Response BusinessFacade::cmd_measurement_start(const json::Value& args) {
    measurement::MeasurementJob* job = runtime_.measurement();
    if (job == nullptr) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "measurement_not_available",
            "测量作业尚未装配（守护进程未完成启动）"));
    }

    measurement::FireParameters params;
    libs::Error param_error{};
    if (!parse_fire_parameters(args, params, param_error)) {
        return Response::failure(param_error);
    }

    // 一次只允许一个出光会话。**出光现在只有这一条路径**，所以这把账拦的
    // 只是"重复开工"：measurement 在跑的时候又发一次 start，后发的会
    // 重新走一遍写激光器的流程，等于把上一轮的光悄悄接管掉。
    if (!runtime_.begin_laser_session()) {
        return Response::failure(libs::Error::make(
            libs::ErrorKind::device, "laser_already_running",
            "已有出光会话在进行（测量作业已在运行）；要停止请用 measurement_stop"));
    }

    // ── 判定就在这一行里发生，别处不许再判一次 ──────────────────────────
    //
    // job->start() 的第一步是 readiness_.evaluate_now()，不放行就**在碰激光器
    // 之前**原样返回错误。因此本命令是"要么成功并已开光，要么失败并说明原因"
    // 的原子结果，调用方不需要（也不允许）自己再问一遍门禁。
    //
    // **刻意不取 device_mutex**：门禁判定要读激光 B2，而事实源 lambda 自己
    // 会去拿那把锁。外层已经持有同一把锁的话就是 std::mutex 自锁 —— 死锁。
    // 串行化由事实源那一层负责，不需要门面再包一层。
    const libs::Result<measurement::MeasurementStartRecord> result = job->start(params);

    if (!result) {
        runtime_.end_laser_session();
        runtime_.log().write(logging::RecordKind::exception_recovery, "测量作业启动失败", [&] {
            json::Value f = json::object();
            f.set("error_code", json::Value(result.error().code));
            f.set("error", json::Value(result.error().message));
            return f;
        }());
        return Response::failure(result.error());
    }

    const measurement::MeasurementStartRecord& r = result.value();
    json::Value v = json::object();
    v.set("running", json::Value(true));
    v.set("note", json::Value(r.note));
    v.set("settle_ms", json::Value(static_cast<double>(r.settle_ms)));
    v.set("discharge_counter_at_start",
          json::Value(static_cast<double>(r.discharge_counter_at_start)));
    v.set("session_parameters", json::Value(
        "来自参数库已标定值 + 本次覆盖：电流 " + std::to_string(params.laser.discharge_current_a) +
        "A / 脉宽 " + std::to_string(params.laser.pulse_width_us) + "us / 频率 " +
        std::to_string(params.laser.frequency_hz) + "Hz / Q 延时 " +
        std::to_string(params.laser.q_delay_us) + "us"));
    v.set("display_hint", json::Value(
        "帧已经自动 publish 到帧队列。UI 订阅 spectrum_stream {action:subscribe} 即可实时显示，"
        "无需再单独启动预览流"));
    v.set("warning", json::Value(
        "激光正在持续出光。停止请调用 measurement_stop；守护进程退出时也会自动收光，"
        "但操作员离开前必须确认光闸已关"));
    json::Value frames = json::array();
    for (const std::string& f : r.sent_frames) frames.push_back(json::Value(f));
    v.set("sent_frames", std::move(frames));

    runtime_.log().write(logging::RecordKind::state_change, "连续出光测量作业开始", [&] {
        json::Value f = json::object();
        f.set("settle_ms", json::Value(static_cast<double>(r.settle_ms)));
        f.set("discharge_counter_at_start",
              json::Value(static_cast<double>(r.discharge_counter_at_start)));
        return f;
    }());
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_measurement_stop(const json::Value& args) {
    (void)args;
    measurement::MeasurementJob* job = runtime_.measurement();
    if (job == nullptr) {
        // 作业都没装配起来，就没什么可停的 —— 但**这不等于"安全"**：
        // 守护进程可能已经崩过一次。收光已经在 Runtime::stop 里无条件做了，
        // 这里如实说"无事可停"，不要谎报"已停止"。
        return Response::failure(libs::Error::make(
            libs::ErrorKind::unsupported, "measurement_not_available",
            "测量作业尚未装配（守护进程未完成启动）；本命令无事可停"));
    }

    // stop() 内部会 join 采集线程，而那个线程可能正卡在 capture 上等 TTL。
    // **这里刻意不取 device_mutex** —— 取了就是自己锁住自己等自己的线程。
    // 测量作业的收光是直接对激光器句柄做的，不经过任何中间服务。
    const bool was_running = job->running();
    const libs::VoidResult stopped = job->stop();
    runtime_.end_laser_session();

    json::Value v = json::object();
    v.set("running", json::Value(false));
    v.set("status", measurement::to_json(job->status()));

    if (!stopped) {
        v.set("warning", json::Value(
            "已下发收光与停机，但读回确认没通过 —— 请现场人工确认光闸确实已关闭"));
        runtime_.log().write(logging::RecordKind::exception_recovery, "测量作业停止未确认", [&] {
            json::Value f = json::object();
            f.set("error", json::Value(stopped.error().message));
            return f;
        }());
        return Response::failure(stopped.error());
    }
    v.set("note", json::Value(was_running
        ? "测量作业已停止：光闸关、已停机，读回确认通过；落盘线程已收尾"
        : "已下发收光与停机并读回确认（此前未在运行）"));
    runtime_.log().write(logging::RecordKind::state_change, "测量作业停止", [&] {
        json::Value f = json::object();
        f.set("was_running", json::Value(was_running));
        return f;
    }());
    return Response::success(std::move(v));
}

Response BusinessFacade::cmd_measurement_status(const json::Value& args) {
    (void)args;
    measurement::MeasurementJob* job = runtime_.measurement();
    if (job == nullptr) {
        json::Value v = json::object();
        v.set("available", json::Value(false));
        v.set("running", json::Value(false));
        v.set("note", json::Value("测量作业尚未装配（守护进程未完成启动）"));
        return Response::success(std::move(v));
    }
    json::Value v = measurement::to_json(job->status());
    v.set("available", json::Value(true));
    // 帧号是 UI 唯一能对上的东西：它单调递增，UI 据此发现丢帧/漏采。
    v.set("laser_session_active", json::Value(runtime_.laser_session_active()));
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
    // 快照里带的是**最近一次判定的结论**（只读缓存，不重跑判定）。
    // monitor 未装配时如实留空，不编一个状态。
    if (const readiness::ReadinessMonitor* monitor = runtime_.monitor()) {
        context.machine_state = readiness::to_string(monitor->latest_state());
        const readiness::GateVerdict& verdict = monitor->latest();
        for (readiness::GateReason r : verdict.blocked_by) {
            context.blocked_by.push_back(readiness::to_string(r));
        }
        context.gate_allowed = verdict.allowed;
    }
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

Response BusinessFacade::cmd_health(const json::Value& args) {
    (void)args;
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());
    return Response::success(runtime_.health_json());
}

}  // namespace business::app
