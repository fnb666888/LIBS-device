// 业务层离线测试（三）：门面（能力 9、10、12 的对外形态）。
//
// 门面是守护进程与 CLI 之间**唯一**的协议面，所以这里逐条钉住它的失败形状：
// 坏 JSON、缺字段、未知命令都必须返回 JSON 错误，而不是抛异常、也不是静默成功。
// 全程 open_devices=false：不开任何串口、不加载 SDK。
#include <string>

#include "check.hpp"
#include "libs/business/app/runtime.hpp"
#include "libs/business/services/facade.hpp"

using namespace business;
using libs::ErrorKind;
using business::policy::ApplyState;

namespace {

// default_template() 返回 Result；测试里直接取值。
const config::SiteConfig& template_config() {
    static const config::SiteConfig value = config::default_template().value();
    return value;
}

// 起一个"纯软件"的 Runtime：数据落在临时目录，不碰设备。
struct Fixture {
    app::Runtime runtime;
    app::BusinessFacade facade;
    std::string data_dir;

    explicit Fixture(const std::string& tag) : facade(runtime) {
        data_dir = std::string("build/tmp_facade_") + tag;
        app::Runtime::Options options;
        options.open_devices = false;
        options.data_dir = data_dir;
        const libs::VoidResult started = runtime.start(template_config(), options);
        // 离线测试必须能起来；起不来就没法测后面的失败路径。
        (void)started;
    }
};

const char* error_kind_of(const app::Response& response) {
    if (!response.payload.has("error")) return "";
    if (!response.payload["error"].has("kind")) return "";
    return response.payload["error"]["kind"].as_string().c_str();
}

}  // namespace

TEST_MAIN_BEGIN()

CASE(facade_ping_reports_running_and_command_list); {
    Fixture f("ping");
    const app::Response response = f.facade.dispatch(R"({"command":"ping","args":{}})");
    CHECK(response.ok);
    CHECK(response.payload["pong"].as_bool());
    CHECK(response.payload["running"].as_bool());
    CHECK(response.payload.has("commands"));
    CHECK(!response.payload["commands"].as_array().empty());
}

CASE(facade_bad_json_is_invalid_argument_not_a_crash); {
    Fixture f("badjson");
    const app::Response response = f.facade.dispatch("{not json");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
    CHECK_EQ(std::string(error_kind_of(response)), std::string("invalid_argument"));
}

CASE(facade_missing_command_is_invalid_argument); {
    Fixture f("nocmd");
    const app::Response response = f.facade.dispatch(R"({"args":{}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
}

CASE(facade_command_must_be_string); {
    Fixture f("cmdtype");
    const app::Response response = f.facade.dispatch(R"({"command":7,"args":{}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
}

CASE(facade_args_must_be_object); {
    Fixture f("argstype");
    const app::Response response = f.facade.dispatch(R"({"command":"ping","args":[1,2]})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
}

CASE(unknown_command_is_unsupported_not_invalid_argument); {
    // 未知命令不是"请求写错了"，是"这台机器没这个能力"，两者要能分开。
    Fixture f("unknown");
    const app::Response response = f.facade.dispatch(R"({"command":"teleport","args":{}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
}

CASE(config_show_echoes_the_running_config); {
    Fixture f("cfg");
    const app::Response response = f.facade.dispatch(R"({"command":"config_show","args":{}})");
    CHECK(response.ok);
    CHECK(response.payload.has("station_id"));
    CHECK(response.payload.has("laser"));
    // 模板里所有设备都是关的，echo 必须如实反映。
    CHECK(!response.payload["laser"]["enabled"].as_bool(true));
}

CASE(heike_report_preview_never_sends_and_keeps_libs_distinct); {
    Fixture f("heike_preview");
    const app::Response response = f.facade.dispatch(R"({"command":"heike_report_preview","args":{
        "kind":"libs_raw","biz_id":"DECL-1","customs_code":"4401",
        "belt_code":"B01","edge_code":"ZG","collect_time":"2026-10-03 12:00:00",
        "seq_no":1,"message_id":"123e4567-e89b-12d3-a456-426614174000",
        "event_code":"LIBS_RAW_V1","raw_capture":{"spectrum_source":"LIBS","scan_groups":[]},
        "event_content_as_string":true}})");
    CHECK(response.ok);
    if (response.ok) {
        CHECK(!response.payload["sent"].as_bool(true));
        CHECK_EQ(response.payload["transport"].as_string(), std::string("dry_run"));
        CHECK(response.payload["payload"]["eventContent"].is_string());
        CHECK(response.payload["warning"].as_string().find("不能冒充") != std::string::npos);
    }
}

CASE(heike_report_preview_rejects_invalid_model_without_fabricating_result); {
    Fixture f("heike_model_bad");
    const app::Response response = f.facade.dispatch(R"({"command":"heike_report_preview","args":{
        "kind":"model","biz_id":"DECL-1","customs_code":"4401",
        "event_code":"MODEL003","event_source":"LIBS-device",
        "message_id":"not-a-uuid","event_content":{}}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
}

CASE(status_reports_gate_and_stage_without_claiming_sync); {
    Fixture f("status");
    const app::Response response = f.facade.dispatch(R"({"command":"status","args":{}})");
    CHECK(response.ok);
    CHECK(response.payload.has("gate"));
    CHECK(response.payload.has("stage"));
    // 没配置频率就必须说"未配置"，不能猜一个 Hz 出来。
    // 频率未配置 -> 如实说未配置，不能猜 Hz。
    CHECK(!response.payload["ac_interlock_participates"].as_bool(true));
    CHECK(response.payload.has("unavailable_capabilities"));
    CHECK(!response.payload["unavailable_capabilities"].as_array().empty());
    CHECK(response.payload["ore"]["present"].is_null());
}

CASE(health_never_throws_even_with_no_devices_open); {
    Fixture f("health");
    const app::Response response = f.facade.dispatch(R"({"command":"health","args":{}})");
    CHECK(response.ok);
    // 设备一个没开：必须如实报告，而不是"全绿"。
    CHECK(response.payload.has("all_required_open"));
    CHECK(!response.payload["summary"].as_string().empty());
    // 模板里全禁用：逐个如实报告 configured=false / opened=false。
    // all_required_open 在"一个都没要求接"时为真是对的——它问的是
    // "你让我接的我接上了吗"，不是"我是不是全接上了"。
    const char* devices[] = {"laser", "rangefinder", "indicator", "air_conditioner",
                             "spectrometer"};
    for (const char* name : devices) {
        CHECK(response.payload[name].has("configured"));
        CHECK(!response.payload[name]["configured"].as_bool(true));
        CHECK(!response.payload[name]["opened"].as_bool(true));
    }
}

CASE(param_get_of_unknown_parameter_is_reported); {
    Fixture f("paramget");
    const app::Response response =
        f.facade.dispatch(R"({"command":"param_get","args":{"group":"nonexistent"}})");
    CHECK(!response.ok);
    CHECK(response.error.kind != ErrorKind::internal);
}

CASE(param_set_reports_incomplete_rather_than_pretending_applied); {
    // 两态语义：只给了部分字段就返回 completed=false，不说"已下发"。
    Fixture f("paramset");
    const app::Response response =
        f.facade.dispatch(R"({"command":"param_set","args":{"group":"belt","values":{"speed":0.2}}})");
    CHECK(response.ok);
    // 默认 at_next_idle：只是暂存，**没有生效**。所以不能报 completed。
    CHECK(!response.payload["completed"].as_bool(true));
    CHECK_EQ(response.payload["state"].as_string(), std::string("staged"));
    // 齐备性独立于"完成"语义：belt 组不在必填清单里，所以仍然不齐备。
    CHECK(!response.payload["readiness"]["ready"].as_bool(true));
}

CASE(composition_reports_algorithm_missing_not_zeroes); {
    Fixture f("comp");
    const app::Response response =
        f.facade.dispatch(R"({"command":"composition","args":{"source":"s"}})");
    CHECK(response.ok);
    CHECK_EQ(response.payload["availability"].as_string(), std::string("algorithm_missing"));
    // 数值位必须一个都不是 0：十项全在，但值全为 null。
    const json::Value& values = response.payload["values"];
    CHECK(values.is_object());
    if (values.is_object()) {
        CHECK_EQ(values.as_object().size(), static_cast<std::size_t>(10));
        for (const auto& kv : values.as_object()) {
            CHECK(kv.second.is_null());
        }
    }
    CHECK(response.payload.has("prerequisites"));
}

CASE(capture_without_configured_frequency_is_invalid_argument); {
    // §3.10.2：频率缺失就不推导节奏。必须 invalid_argument，
    // 不能猜一个默认 Hz 也不能返回"空采集成功"。
    Fixture f("capture");
    const app::Response response = f.facade.dispatch(R"({"command":"capture","args":{}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
}

CASE(capture_with_pace_but_no_device_is_unsupported); {
    // 节奏能从配置给出，但光谱仪没开 -> unsupported，不是"采到 0 条光谱"。
    Fixture f("capnodev");
    const app::Response response =
        f.facade.dispatch(R"({"command":"capture","args":{"from_config":true}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
}

CASE(detection_start_is_a_signal_not_a_physical_trigger); {
    // 任务书 §2.1：这是**业务信号**，不是物理触发。信号要被接受，
    // 但响应里绝不能说"已经开始检测"/"已出光"。
    Fixture f("start");
    const app::Response response =
        f.facade.dispatch(R"({"command":"detection_start","args":{"target_belt":"B1"}})");
    CHECK(response.ok);
    CHECK(response.payload["accepted"].as_bool());
    CHECK(response.payload.has("request"));
    CHECK(response.payload["request"]["target_belt"].as_string() == "B1");
    // 业务进度推进到了"已选矿带"，但门禁与物理执行都还没发生。
    CHECK(f.runtime.stage() == events::DetectionStage::selected_belt);
    CHECK(response.payload.has("note"));
}

CASE(recovery_write_only_writes_and_never_resumes); {
    Fixture f("recov");
    const app::Response response =
        f.facade.dispatch(R"({"command":"recovery_write","args":{"reason":"test"}})");
    CHECK(response.ok);
    CHECK(response.payload["written"].as_bool());
    CHECK(response.payload.has("path"));

    const app::Response listed = f.facade.dispatch(R"({"command":"recovery_list","args":{}})");
    CHECK(listed.ok);
    CHECK(!listed.payload["snapshots"].as_array().empty());
}

// ── 实时光谱流（能力 10 展示侧）的协议面 ─────────────────────────────────
//
// 门面的 stream 命令在 open_devices=false 的离线环境里也必须可用：
// 流会"跑起来但如实报设备没开"，这正是现场排障需要看到的状态
// （起不来的流 + 沉默的守护进程，排障时最难查）。

CASE(spectrum_stream_status_is_honest_without_devices); {
    Fixture f("stream_status");
    const app::Response response =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"status"}})");
    CHECK(response.ok);
    CHECK(!response.payload["running"].as_bool());
    CHECK(!response.payload["spectrometer_open"].as_bool());
    // 没有设备就写没有，不能写"正常"。
    CHECK(response.payload.has("last_error_code"));
    // 免责说明必须在：preview 拍到的不是测量谱。
    CHECK(!response.payload["note"].as_string().empty());
}

CASE(spectrum_stream_start_and_stop_round_trip); {
    Fixture f("stream_start");
    const app::Response started =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"start"}})");
    CHECK(started.ok);
    CHECK(started.payload["started"].as_bool());
    CHECK(started.payload["running"].as_bool());
    // 启动必须带上"这不是测量谱"的警告，别让人误读。
    CHECK(!started.payload["warning"].as_string().empty());

    const app::Response stopped =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"stop"}})");
    CHECK(stopped.ok);
    CHECK(stopped.payload["stopped"].as_bool());
    CHECK(!stopped.payload["running"].as_bool());
}

CASE(spectrum_stream_subscribe_then_latest_is_not_an_error_when_empty); {
    Fixture f("stream_sub");
    const app::Response sub =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"subscribe"}})");
    CHECK(sub.ok);
    CHECK(sub.payload["subscribed"].as_bool());
    CHECK(sub.payload.has("subscriber"));

    // 空队列取帧：ok=true + has_frame=false。**不是错误**——
    // UI 应当继续等，而不是弹一个"失败"。
    const app::Response latest = f.facade.dispatch(
        R"({"command":"spectrum_stream","args":{"action":"latest","subscriber":1}})");
    CHECK(latest.ok);
    CHECK(!latest.payload["has_frame"].as_bool());
    CHECK(latest.payload.has("pending"));
}

CASE(spectrum_stream_latest_without_subscriber_is_invalid_argument); {
    Fixture f("stream_nosub");
    const app::Response response =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"latest"}})");
    CHECK(!response.ok);
    CHECK_EQ(std::string(error_kind_of(response)), std::string("invalid_argument"));
    CHECK(response.error.code == "stream_subscriber_required");
}

CASE(spectrum_stream_bad_action_is_rejected); {
    Fixture f("stream_bad");
    const app::Response response =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"turbo"}})");
    CHECK(!response.ok);
    CHECK_EQ(std::string(error_kind_of(response)), std::string("invalid_argument"));
    CHECK(response.error.code == "stream_bad_action");
}

// 回归测试：曾经 spectrum_stream_.configure() 被放在 devices_.open() **之前**，
// 于是 devices_.spectrometer() 返回 nullptr，流一直报 spectrometer_not_open，
// 而 health 同时说"光谱仪: 已打开"——两个命令自相矛盾，现场无从下手。
// 这条断言钉住"两者必须一致"：流看到的设备状态 = health 看到的设备状态。
CASE(spectrum_stream_agrees_with_health_about_the_spectrometer); {
    // 离线夹具本来就不开设备，所以这条只在"两边都说没开"时成立；
    // 它的价值在于**一旦有人把 configure 挪到 open 之前、而夹具又开了设备，
    // 这条立刻会红**。真正开设备的路径由 test_spectrum_stream 的采集替身覆盖。
    Fixture f("stream_vs_health");
    const app::Response health =
        f.facade.dispatch(R"({"command":"health","args":{}})");
    CHECK(health.ok);
    const bool health_open = health.payload["spectrometer"]["opened"].as_bool();

    const app::Response st =
        f.facade.dispatch(R"({"command":"spectrum_stream","args":{"action":"status"}})");
    CHECK(st.ok);
    const bool stream_open = st.payload["spectrometer_open"].as_bool();
    // 一个说开了另一个说没开 = 装配顺序错位。这是必须钉死的不变量。
    CHECK_EQ(stream_open, health_open);
}

CASE(spectrum_stream_appears_in_commands_list); {
    Fixture f("stream_cmd");
    // 自省列表要能看到它，否则 CLI 没法做"有哪些命令"的展示。
    const app::Response response = f.facade.dispatch(R"({"command":"ping","args":{}})");
    CHECK(response.ok);
    bool found = false;
    for (const auto& c : response.payload["commands"].as_array()) {
        if (c.as_string() == "spectrum_stream") found = true;
    }
    CHECK(found);
}

// 回归：laser_fire 曾经能被 dispatch() 派发，却不在 commands() 里。
// 后果是核心的物理动作从自省接口和 CLI 上都消失——文档说命令面里有出光，
// 实际客户端连命令名都不知道。这里钉死它必须出现。
CASE(laser_fire_appears_in_commands_list); {
    Fixture f("fire_cmd");
    const app::Response response = f.facade.dispatch(R"({"command":"ping","args":{}})");
    CHECK(response.ok);
    bool found = false;
    for (const auto& c : response.payload["commands"].as_array()) {
        if (c.as_string() == "laser_fire") found = true;
    }
    CHECK(found);
}

// 回归：参数落盘后重启，门禁必须**立刻**知道参数齐备。
//
// 曾经有两处毛病叠在一起：param_get 不把齐备状态推给门禁，而启动时也不推 ——
// 于是守护进程每次重启后门禁都卡在 parameters_incomplete，要等某个客户端
// 碰巧调一次 param_get 才恢复。现场表现是"刚开机预检莫名不通过"。
// 这条直接断言：**重启后什么都不用调，门禁就该是齐备的**。
CASE(param_get_after_restart_keeps_the_gate_in_sync); {
    const char* tag = "persist_gate";
    {
        Fixture f(tag);
        (void)f.facade.dispatch(R"({"command":"param_set","args":{"group":"spectrometer","values":{"a":1},"timing":"immediately"}})");
        (void)f.facade.dispatch(R"({"command":"param_set","args":{"group":"laser","values":{"b":1},"timing":"immediately"}})");
        (void)f.facade.dispatch(R"({"command":"param_set","args":{"group":"trigger","values":{"c":1},"timing":"immediately"}})");
    }
    // 同一个 data_dir 再起一次 Runtime：参数从磁盘加载，模拟守护进程重启。
    Fixture f(tag);
    // 先什么都不调，直接看门禁 —— 这是本用例的重点。
    const app::Response gate_first = f.facade.dispatch(R"({"command":"gate","args":{}})");
    CHECK(gate_first.ok);
    bool incomplete_before_any_call = false;
    for (const auto& r : gate_first.payload["blocked_by"].as_array()) {
        if (r.as_string() == "parameters_incomplete") incomplete_before_any_call = true;
    }
    CHECK(!incomplete_before_any_call);

    const app::Response get = f.facade.dispatch(R"({"command":"param_get","args":{}})");
    CHECK(get.ok);
    CHECK(get.payload["readiness"]["ready"].as_bool());
    const app::Response gate = f.facade.dispatch(R"({"command":"gate","args":{}})");
    CHECK(gate.ok);
    bool incomplete = false;
    for (const auto& r : gate.payload["blocked_by"].as_array()) {
        if (r.as_string() == "parameters_incomplete") incomplete = true;
    }
    CHECK(!incomplete);
}
TEST_MAIN_RETURN()
