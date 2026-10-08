//
// 门面是守护进程与 CLI 之间**唯一**的协议面，所以这里逐条钉住它的失败形状：
// 坏 JSON、缺字段、未知命令都必须返回 JSON 错误，而不是抛异常、也不是静默成功。
// 全程 open_devices=false：不开任何串口、不加载 SDK。
#include <string>

#include "check.hpp"
#include "libs/business/app/runtime.hpp"
#include "libs/business/app/facade.hpp"

using namespace business;
using libs::ErrorKind;
using business::config::ApplyState;

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

    explicit Fixture(const std::string& tag) : Fixture(tag, template_config()) {}

    // 需要自定义配置的用例走这里。laser.enabled 尤其重要：
    // 布防第一步就查它，模板里是 false，所以默认夹具永远走不到判定那一步。
    Fixture(const std::string& tag, const config::SiteConfig& config) : facade(runtime) {
        data_dir = ::testing::scratch(std::string("build/tmp_facade_") + tag);
        app::Runtime::Options options;
        options.open_devices = false;
        options.data_dir = data_dir;
        const libs::VoidResult started = runtime.start(config, options);
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

CASE(status_reports_gate_and_indicator_without_claiming_sync); {
    Fixture f("status");
    const app::Response response = f.facade.dispatch(R"({"command":"status","args":{}})");
    CHECK(response.ok);
    CHECK(response.payload.has("gate"));
    CHECK(response.payload.has("indicator"));
    // 没配置频率就必须说"未配置"，不能猜一个 Hz 出来。
    // 频率未配置 -> 如实说未配置，不能猜 Hz。
    CHECK(response.payload.has("unavailable_capabilities"));
    CHECK(!response.payload["unavailable_capabilities"].as_array().empty());
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
    const char* devices[] = {"laser", "indicator", "spectrometer"};
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

CASE(fresh_store_is_seeded_with_the_real_device_parameters); {
    // 回归：进程启动后**立即**就能用真机参数检测，界面第一次 param_get
    // 拿到的也是真机参数。默认值来自 docs/real-device-parameters-20260929.md，
    // 由 ParameterService::seed_defaults() 在 Runtime::start 里补进参数库。
    //
    // 在此之前：激光侧完全不播种（9 个框全是量程下限、门禁卡未标定），
    // 光谱侧种的是通用占位值 10 ms / 3000 ms / 5000 ns，与真机对不上。
    Fixture f("realseed");
    const app::Response get = f.facade.dispatch(R"({"command":"param_get","args":{}})");
    CHECK(get.ok);
    CHECK(get.payload["readiness"]["ready"].as_bool(false));

    const json::Value& laser = get.payload["groups"]["laser"]["values"];
    CHECK_EQ(laser["discharge_current_a"].as_number(), 80.0);     // 0x11
    CHECK_EQ(laser["pulse_width_us"].as_number(), 200.0);         // 0x12
    CHECK_EQ(laser["frequency_hz"].as_number(), 1.0);             // 0x13
    CHECK_EQ(laser["discharge_delay_us"].as_number(), 0.0);       // 0x14
    CHECK_EQ(laser["q_delay_us"].as_number(), 210.0);             // 0x16
    CHECK_EQ(laser["q_warmup_s"].as_number(), 0.0);               // 0x19
    CHECK_EQ(laser["settle_timeout_ms"].as_number(), 20000.0);
    CHECK_EQ(laser["emission_hold_ms"].as_number(), 3000.0);
    CHECK_EQ(laser["min_discharges"].as_number(), 1.0);

    const json::Value& spec = get.payload["groups"]["spectrometer"]["values"];
    CHECK_EQ(spec["integration_ms"].as_number(), 1.05);
    CHECK_EQ(spec["capture_timeout_ms"].as_number(), 10000.0);
    CHECK_EQ(spec["integration_delay_ns"].as_number(), 1280.0);

    // 9 个字段齐备且合法 -> 视为已标定：门禁不会因为参数卡住。
    const app::Response gate = f.facade.dispatch(R"({"command":"gate","args":{}})");
    CHECK(gate.ok);
    for (const auto& r : gate.payload["blocked_by"].as_array()) {
        CHECK(r.as_string() != std::string("parameters_incomplete"));
        CHECK(r.as_string() != std::string("laser_not_commissioned"));
    }
}

CASE(param_set_of_an_unused_group_does_not_affect_measurement_readiness); {
    // 往一个既非出光用、也非光谱侧用的分组写值：能写进去（参数库是通用容器），
    // 但**不影响"能不能测"**。这里断言齐备状态不因为它而改变，
    // 是为了钉住"齐备只看必填组"这条规则，防止有人把 belt 又加回必填清单。
    Fixture f("paramset");
    const app::Response seed = f.facade.dispatch(R"({"command":"param_set","args":{"group":"laser","values":{"discharge_current_a":80,"pulse_width_us":200,"frequency_hz":1,"discharge_delay_us":0,"q_delay_us":210,"q_warmup_s":0,"settle_timeout_ms":20000,"emission_hold_ms":3000,"min_discharges":1}}})");
    CHECK(seed.ok);
    CHECK(seed.payload["readiness"]["ready"].as_bool(false));

    const app::Response response =
        f.facade.dispatch(R"({"command":"param_set","args":{"group":"belt","values":{"speed":0.2}}})");
    CHECK(response.ok);
    // 改动**当场生效**，不再有"暂存但不生效"那条路。
    CHECK(response.payload["completed"].as_bool(false));
    CHECK_EQ(response.payload["state"].as_string(), std::string("completed"));
    // 必填组只有 spectrometer + laser（trigger 归装机配置，不是测量参数），
    // 而 belt 不在其中 —— 齐备状态必须与写入前完全一致。
    CHECK(response.payload["readiness"]["ready"].as_bool(false));
}

CASE(capture_without_configured_frequency_is_not_an_error); {
    // 契约变更：工作频率**不再**是采集的前提。
    //
    // 以前 capture 因为"频率没配"直接 invalid_argument。现在频率只决定
    // 帧周期，而周期不是采集能不能做的条件 —— 测量参数一律来自参数库。
    // 所以缺频率时 capture 照常往下走，只会在设备层如实失败。
    //
    // 离线夹具里光谱仪没开，因此最终是 unsupported（设备没开），
    // 而不是 invalid_argument（频率没配）。这两者的区别正是本用例要点：
    // 报 unsupported 说明它**没有**停在频率上编造一个理由。
    Fixture f("capture");
    const app::Response response = f.facade.dispatch(R"({"command":"capture","args":{}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
}

CASE(capture_with_pace_but_no_device_is_unsupported); {
    // 节奏能从配置给出，但光谱仪没开 -> unsupported，不是"采到 0 条光谱"。
    Fixture f("capnodev");
    const app::Response response =
        f.facade.dispatch(R"({"command":"capture","args":{"from_config":true}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
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
    // 帧队列**没有 running 概念** —— 它不采集。字段缺失是对的：
    // 写一个 running=false 反而是在暗示"它能跑"。
    CHECK(!response.payload.has("running"));
    // 没有设备就写没有，不能写"正常"。
    CHECK(response.payload["spectrometer_open"].is_null());
    // 免责说明必须在：这里只有帧的搬运，测量由 measurement_* 负责。
    CHECK(!response.payload["note"].as_string().empty());
}

CASE(spectrum_stream_start_and_stop_are_now_refused_with_a_pointer); {
    // 这是拆分留下的**接口纪律**：采集入口只有一个。
    //
    // 旧 spectrum_stream 能 start，UI 于是有两条路开工：一条只显示、
    // 一条连出光。结果两条都"能跑"，操作员选了预览那条就以为在测量，
    // 而设备其实在拍背景谱。现在 start/stop 必须**明确报错并指路**，
    // 而不是静默忽略 —— 静默忽略会让人以为已经开工了。
    Fixture f("stream_start");
    for (const char* action : {"start", "stop"}) {
        // 用自定义分隔符拼 JSON：默认的 )" 会在字符串中间提前结束。
        const std::string body = std::string(R"json({"command":"spectrum_stream","args":{"action":")json") +
                                 action + R"json("}})json";
        const app::Response r = f.facade.dispatch(body);
        CHECK(!r.ok);
        CHECK_EQ(std::string(error_kind_of(r)), std::string("unsupported"));
        CHECK(r.error.code == "stream_no_start_stop");
        // 错误信息必须指路，否则用户只能猜。
        CHECK(r.error.message.find("measurement_start") != std::string::npos);
    }
}

CASE(measurement_commands_appear_in_commands_list); {
    // 三条新命令必须能被自省看到，否则 CLI 根本不知道该发什么。
    Fixture f("measure_cmd");
    const app::Response response = f.facade.dispatch(R"({"command":"ping","args":{}})");
    CHECK(response.ok);
    bool start = false, stop = false, status = false;
    for (const auto& c : response.payload["commands"].as_array()) {
        const std::string name = c.as_string();
        if (name == "measurement_start") start = true;
        if (name == "measurement_stop") stop = true;
        if (name == "measurement_status") status = true;
    }
    CHECK(start);
    CHECK(stop);
    CHECK(status);
}

CASE(measurement_status_is_honest_before_anything_starts); {
    // 离线、无设备时也必须答得上来，且**不能**假装在跑。
    Fixture f("measure_status");
    const app::Response r =
        f.facade.dispatch(R"({"command":"measurement_status","args":{}})");
    CHECK(r.ok);
    CHECK(!r.payload["running"].as_bool());
    CHECK_EQ(r.payload["shots_captured"].as_number(), 0.0);
    CHECK_EQ(r.payload["shots_failed"].as_number(), 0.0);
    CHECK_EQ(r.payload["disk_writes_ok"].as_number(), 0.0);
    CHECK_EQ(r.payload["disk_writes_failed"].as_number(), 0.0);
    CHECK_EQ(r.payload["disk_backlog"].as_number(), 0.0);
}

CASE(measurement_start_without_a_laser_reports_the_real_reason); {
    // 离线夹具没开激光器：必须报"激光器没开"，
    // **不能**报"门禁不放行" —— 那是误导，现场会去查联锁，
    // 而真实原因是这台机器根本没连上。
    Fixture f("measure_start");
    const app::Response r =
        f.facade.dispatch(R"({"command":"measurement_start","args":{}})");
    CHECK(!r.ok);
    // 具体是哪个错取决于设备是否在场；但**绝不能**是 gate_blocked：
    // 门禁被问的时候还没激光器，报门禁不放行会让人去查联锁。
    CHECK(r.error.code != "gate_blocked");
    // 失败的启动不能留下"出光会话"占着锁，否则之后再也开不了工。
    const app::Response st =
        f.facade.dispatch(R"({"command":"measurement_status","args":{}})");
    CHECK(st.ok);
    CHECK(!st.payload["running"].as_bool());
}

CASE(measurement_stop_without_a_laser_is_reported_not_crashed); {
    // 停止必须**无条件可调**：守护进程退出、UI 点停止、作业收尾都会调它。
    Fixture f("measure_stop");
    const app::Response r =
        f.facade.dispatch(R"({"command":"measurement_stop","args":{}})");
    CHECK(!r.ok);
    CHECK(r.error.code == "laser_not_open");
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

// 回归：laser_fire / laser_run / laser_stop 三条命令已经删除。
//
// 旧命名说的是“按一下灯灯就出光”，它绕过了测量作业、
// 回归到这台星的整个生产链路。现在出光只有 measurement_start
// 一条路径，而且它必须走门禁。
// 这里钉死两件事：不得再出现在自省列表里，且 dispatch 必须硬拒绝。
CASE(legacy_laser_commands_are_gone_from_the_command_surface); {
    Fixture f("fire_cmd");
    const app::Response response = f.facade.dispatch(R"({"command":"ping","args":{}})");
    CHECK(response.ok);
    for (const auto& c : response.payload["commands"].as_array()) {
        const std::string name = c.as_string();
        CHECK(name != "laser_fire");
        CHECK(name != "laser_run");
        CHECK(name != "laser_stop");
    }
    // 出光必须仍然可以被发起，只是改名了。
    bool found = false;
    for (const auto& c : response.payload["commands"].as_array()) {
        if (c.as_string() == "measurement_start") found = true;
    }
    CHECK(found);
}

CASE(legacy_laser_commands_are_rejected_as_unknown); {
    // 只不列在 commands() 里却仍能被 dispatch 派发，等于删了表面下的门守。
    Fixture f("fire_reject");
    for (const char* legacy : {"laser_fire", "laser_run", "laser_stop"}) {
        const app::Response r = f.facade.dispatch(
            std::string(R"({"command":")") + legacy + R"(","args":{}})");
        CHECK(!r.ok);
        if (!r.ok) CHECK_EQ(r.error.code, std::string("unknown_command"));
    }
}

// 回归：参数落盘后重启，门禁必须**立刻**知道参数齐备。
//
// 曾经有两处毛病叠在一起：param_get 不把齐备状态推给门禁，而启动时也不推 ——
// 于是守护进程每次重启后门禁都卡在 parameters_incomplete，要等某个客户端
// 碰巧调一次 param_get 才恢复。现场表现是"刚开机预检莫名不通过"。
// 这条直接断言：**重启后什么都不用调，门禁就该是齐备的**。
// 启动时 seed_defaults() 已把真机参数补进参数库（只补缺失），
// 所以这里无需先手工填 9 个值；下面的 param_set 只是把磁盘内容写成
// 与默认值不同的形状，用来验证"重启后从磁盘读回的那份也齐备"。
CASE(param_get_after_restart_keeps_the_gate_in_sync); {
    const char* tag = "persist_gate";
    {
        Fixture f(tag);
        (void)f.facade.dispatch(R"({"command":"param_set","args":{"group":"spectrometer","values":{"a":1},"timing":"immediately"}})");
        (void)f.facade.dispatch(R"({"command":"param_set","args":{"group":"laser","values":{"discharge_current_a":80,"pulse_width_us":200,"frequency_hz":1,"discharge_delay_us":0,"q_delay_us":210,"q_warmup_s":0,"settle_timeout_ms":20000,"emission_hold_ms":3000,"min_discharges":1},"timing":"immediately"}})");
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


// ── 设备预检（业务一）的 IPC 出口 ────────────────────────────────────────────────
//
// PrecheckService::run() 在 Runtime::start() 里就装配好了，但在这条命令
// 出现之前**全库没有任何生产调用者**：这台机器能判"能不能出光"，却没有任何
// 一个客户端能主动让它判一次。下面这组用例钉住补上的这唯一一环。
CASE(precheck_is_in_the_command_surface); {
    const std::vector<std::string>& cmds = app::BusinessFacade::commands();
    bool found = false;
    for (const std::string& c : cmds) {
        if (c == "precheck") found = true;
    }
    CHECK(found);
    // 路由表和分派必须对齐：登记了却没接上，等于没有。
    Fixture f("precheck_cmd");
    const app::Response response = f.facade.dispatch(R"({"command":"precheck","args":{}})");
    // 离线夹具里激光器没开 -> 布防失败。这是**如实失败**，不是崩溃。
    CHECK(!response.ok);
    CHECK_EQ(response.error.code, std::string("gate_laser_disabled"));
}

CASE(precheck_without_a_laser_is_blocked_but_still_reports_why); {
    // 门禁不放行时，返回体里必须带完整报告 —— 否则操作员只看到
    // 一句"预检失败"，不知道该去查哪一条。
    Fixture f("precheck_report");
    const app::Response response = f.facade.dispatch(R"({"command":"precheck","args":{}})");
    CHECK(!response.ok);
    CHECK(response.payload.has("error"));
    CHECK(response.payload.has("report"));
    const json::Value& report = response.payload["report"];
    CHECK(!report["allowed"].as_bool(true));
    // 报告必须能说清"是没读到"还是"读到了但不安全"。
    CHECK(!report["laser_readable"].as_bool(true));
    CHECK(!report["detail"].as_string().empty());
    // 灯的字段同样要带回来：预检的产出之一就是刷灯。
    CHECK(report.has("lamp_intent"));
    CHECK(report.has("lamp_applied"));
    // 关键：报告挂在 report 子对象下，**不能覆盖顶层 error**。
    CHECK(response.payload["error"].has("code"));
    CHECK_EQ(response.payload["error"]["code"].as_string(), std::string("gate_laser_disabled"));
}

CASE(precheck_does_not_fire_the_laser); {
    // 预检必须是只读的：布防 + 判定 + 刷灯，一条激光写指令都不发。
    // 这里断言的是"出光作业没有被动过"——
    // 真去数激光指令数需要替身设备，超出这一层的职责；
    // 但"跑完预检后作业没起来"是能从外部看到的，必须钉住。
    Fixture f("precheck_no_fire");
    (void)f.facade.dispatch(R"({"command":"precheck","args":{}})");
    const app::Response status = f.facade.dispatch(R"({"command":"status","args":{}})");
    CHECK(status.ok);
    CHECK(!status.payload["laser_running"].as_bool(true));
    // 判定执行点只有一个：作业没启动，判定就不该由预检之外的地方产生。
    const app::Response ms = f.facade.dispatch(R"({"command":"measurement_status","args":{}})");
    CHECK(ms.ok);
    CHECK(!ms.payload["running"].as_bool(true));
}

CASE(precheck_does_not_stand_in_front_of_measurement_start); {
    // 这条是纪律测试，不是功能测试。
    // 判定真正的执行点只有 MeasurementJob::start()；预检通了不代表
    // measurement_start 会通过。界面**绝不能**"先预检拿绿灯再开始检测"，
    // 否则判定出现第二个执行点，而且两条路径读的事实不是同一时刻。
    Fixture f("precheck_not_gate");
    const app::Response pre = f.facade.dispatch(R"({"command":"precheck","args":{}})");
    CHECK(!pre.ok);
    const app::Response started =
        f.facade.dispatch(R"({"command":"measurement_start","args":{}})");
    // 设备没开，作业自己会失败 —— 无论预检结论如何。
    CHECK(!started.ok);
    // 两条路径都必须如实报告自己那一侧的结果，不能有一方"替另一方说话"。
    CHECK(started.payload.has("error"));
}

CASE(precheck_reaches_the_verdict_when_the_laser_is_configured_but_closed); {
    // 布防只看**配置里有没有使能**，不看设备开没开。
    // 所以把 laser.enabled 打开、设备仍然不开，就能在离线夹具里
    // 走到 evaluate_now()，从而断言"预检确实触发了判定"而不只是布防。
    config::SiteConfig cfg = template_config();
    cfg.laser.enabled = true;
    Fixture f("precheck_verdict", cfg);
    const app::Response response = f.facade.dispatch(R"({"command":"precheck","args":{}})");
    CHECK(!response.ok);
    CHECK(response.payload.has("report"));
    const json::Value& report = response.payload["report"];
    CHECK(!report["allowed"].as_bool(true));
    CHECK(!report["laser_readable"].as_bool(true));
    bool has_reason = false;
    for (const auto& r : report["blocked_by"].as_array()) {
        const std::string code = r.as_string();
        // 设备没开 -> 事实读不到 -> laser_unavailable。
        if (code == "laser_unavailable") has_reason = true;
        // 反向断言：布防成功了，报告里**不能**出现"没布防"这条理由。
        CHECK(code != "gate_disarmed");
    }
    CHECK(has_reason);
    // 布防成功 => armed 为真；否则判定恒假，blocked_by 就只剩 disarm。
    const app::Response gate = f.facade.dispatch(R"({"command":"gate","args":{}})");
    CHECK(gate.ok);
    CHECK(gate.payload["armed"].as_bool(false));
}
TEST_MAIN_RETURN()
