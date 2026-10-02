// 业务层离线测试（一）：JSON、站点配置、门禁、参数。
//
// 这些用例全部不碰串口、不加载 DLL——它们验的是**纪律**，不是设备行为。
// 任务书 §7 的每一条红线在这里都有一个对应的可执行断言。
#include <filesystem>
#include <string>

#include "check.hpp"
#include "libs/business/json/json.hpp"
#include "libs/business/config/site_config.hpp"
#include "libs/business/integration/heike_events.hpp"
#include "libs/business/policy/detection_config.hpp"
#include "libs/business/policy/gate.hpp"

using namespace business;
using json::Value;

namespace {

Value parsed(const std::string& text, bool& ok) {
    Value v;
    const json::ParseError err = json::parse(text, v);
    ok = err.ok;
    return v;
}

}  // namespace

// ── JSON ───────────────────────────────────────────────────────────────────

namespace {

// 全部安全位的"最安全"事实。逐项改坏来测每条拦截路径。
policy::LaserSafetyFacts safe_facts() {
    policy::LaserSafetyFacts f;
    f.available = true;
    f.work_state = false;
    f.interlock_1 = true;
    f.interlock_2 = true;
    f.emergency_stop = true;
    f.q_gate = true;
    f.work_enabled = true;
    f.over_temperature = true;
    f.fault_bits = 0;
    return f;
}

bool blocked_by(const policy::GateVerdict& verdict, policy::GateReason reason) {
    for (policy::GateReason r : verdict.blocked_by) {
        if (r == reason) return true;
    }
    return false;
}

}  // namespace

TEST_MAIN_BEGIN()

CASE(json_roundtrip_object); {
    Value v = json::object();
    v.set("name", Value(std::string("libs")));
    v.set("count", Value(3));
    v.set("ratio", Value(0.25));
    v.set("on", Value(true));
    v.set("missing", Value(nullptr));
    Value arr = json::array();
    arr.push_back(Value(1));
    arr.push_back(Value(std::string("two")));
    v.set("list", std::move(arr));

    const std::string text = json::stringify(v);
    bool ok = false;
    const Value back = parsed(text, ok);
    CHECK(ok);
    CHECK_EQ(back["name"].as_string(), std::string("libs"));
    CHECK_EQ(back["count"].as_number(), 3.0);
    CHECK_EQ(back["ratio"].as_number(), 0.25);
    CHECK(back["on"].as_bool());
    CHECK(back["missing"].is_null());
    CHECK_EQ(back["list"].as_array().size(), std::size_t(2));
}

CASE(json_absent_member_is_null_not_crash); {
    // 缺字段绝不能变成 false/0——上层要靠这个区分"没有"和"是 0"。
    bool ok = false;
    const Value v = parsed(R"({"a":1})", ok);
    CHECK(ok);
    CHECK(v["nope"].is_null());
    CHECK(v.has("a"));
    CHECK(!v.has("nope"));
}

CASE(json_rejects_trailing_garbage); {
    bool ok = true;
    (void)parsed(R"({"a":1} trailing)", ok);
    CHECK(!ok);
}

CASE(json_rejects_trailing_comma); {
    bool ok = true;
    (void)parsed(R"({"a":1,})", ok);
    CHECK(!ok);
}

CASE(json_rejects_truncated); {
    bool ok = true;
    (void)parsed(R"({"a":)", ok);
    CHECK(!ok);
}

CASE(json_nan_becomes_null_not_a_number); {
    // NaN/Inf 不是合法 JSON，也不能悄悄变成 0。
    const std::string text = R"({"x":1e999})";
    bool ok = false;
    const Value v = parsed(text, ok);
    // 1e999 溢出：解析要么失败，要么得到不可用数；两种都不允许变成 0。
    if (ok) {
        CHECK(!v["x"].is_number() || v["x"].as_number() != 0.0);
    }
}

// ── 站点配置（能力 1）─────────────────────────────────────────────────────

CASE(config_rejects_enabled_without_calibrated_interlock); {
    // 3.9.2 是 C 类：启用了联锁却没有标定阈值，是自相矛盾的配置，必须拒绝。
    const std::string text = R"({
        "station_id": "s",
        "ac_interlock": { "enabled": true, "calibrated": false, "trigger_indoor_temp_c": 0 }
    })";
    bool ok = false;
    const Value v = parsed(text, ok);
    CHECK(ok);
    const auto loaded = config::from_json(v);
    CHECK(!loaded.ok());
    CHECK_EQ(static_cast<int>(loaded.error().kind), static_cast<int>(libs::ErrorKind::invalid_argument));
}

CASE(config_rejects_non_string_port); {
    // 端口必须是字符串；写成数字说明配置来源不对，宁可报错也不猜。
    const std::string text = R"({"station_id":"s","laser":{"port":10}})";
    bool ok = false;
    const Value v = parsed(text, ok);
    CHECK(ok);
    const auto loaded = config::from_json(v);
    CHECK(!loaded.ok());
}

CASE(config_accepts_minimal_valid); {
    const std::string text = R"({
        "station_id": "s1",
        "laser":           { "port": "COM10", "enabled": false },
        "rangefinder":     { "enabled": false },
        "indicator":       { "enabled": false },
        "air_conditioner": { "enabled": false },
        "spectrometer":    { "enabled": false },
        "ac_interlock":    { "enabled": false, "calibrated": false }
    })";
    bool ok = false;
    const Value v = parsed(text, ok);
    CHECK(ok);
    const auto loaded = config::from_json(v);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.value().station_id, std::string("s1"));
        CHECK_EQ(loaded.value().laser.port, std::string("COM10"));
        // 空调联锁默认不参与门禁（C 类纪律）。
        CHECK(!loaded.value().ac_interlock.enabled);
    }
}

CASE(config_default_template_passes_validation); {
    // --print-default-config 必须给出一份**能通过校验**的骨架。
    const auto tpl = config::default_template();
    CHECK(tpl.ok());
    if (tpl.ok()) {
        CHECK(config::save_string(tpl.value()).ok());
        CHECK(!tpl.value().laser.enabled);
        CHECK(!tpl.value().spectrometer.enabled);
        CHECK(!tpl.value().gateway.enabled);
        CHECK(!tpl.value().ac_interlock.enabled);
    }
}

CASE(config_default_is_rejected_not_silently_accepted); {
    // 反过来：空 SiteConfig 必须被 validate 拒绝（禁止静默回落默认值）。
    const libs::VoidResult v = config::validate(config::SiteConfig{});
    CHECK(!v.ok());
    CHECK_EQ(static_cast<int>(v.error().kind), static_cast<int>(libs::ErrorKind::invalid_argument));
}

CASE(config_gateway_default_is_disabled_and_portless); {
    // 3.5.2：协议未定，端口不能猜。默认必须是 0 + disabled。
    const config::SiteConfig fresh;
    CHECK(!fresh.gateway.enabled);
    CHECK_EQ(fresh.gateway.port, 0u);
}

CASE(config_gateway_enabled_requires_real_edge_fields); {
    bool ok = false;
    const Value bad = parsed(R"({
        "station_id":"s1",
        "laser":{"enabled":false}, "rangefinder":{"enabled":false},
        "indicator":{"enabled":false}, "air_conditioner":{"enabled":false},
        "spectrometer":{"enabled":false},
        "gateway":{"enabled":true,"base_url":"http://127.0.0.1:18100",
          "product_id":"p","device_id":"d","token":"secret"}
    })", ok);
    CHECK(ok);
    CHECK(!config::from_json(bad).ok());

    const Value good = parsed(R"({
        "station_id":"s1",
        "laser":{"enabled":false}, "rangefinder":{"enabled":false},
        "indicator":{"enabled":false}, "air_conditioner":{"enabled":false},
        "spectrometer":{"enabled":false},
        "gateway":{"enabled":true,"base_url":"http://127.0.0.1:18100",
          "product_id":"p","device_id":"d","token":"secret",
          "equipment_id":"e","equipment_type":"LIBS"}
    })", ok);
    CHECK(ok);
    const auto loaded = config::from_json(good);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK(config::to_json(loaded.value())["gateway"]["token"].as_string() == "secret");
        CHECK(config::to_json_redacted(loaded.value())["gateway"]["token"].is_null());
        CHECK(config::to_json_redacted(loaded.value())["gateway"]["token_configured"].as_bool());
    }
}

CASE(heike_device_reports_are_separate_from_model_envelopes); {
    integration::heike::DeviceIdentity identity{"dev-1", "LIBS", "黄埔海关",
                                                 "2026-10-03 12:00:00"};
    const auto heartbeat = integration::heike::make_heartbeat(identity);
    CHECK(heartbeat.ok());
    if (heartbeat.ok()) {
        CHECK(heartbeat.value()["equipmentID"].as_string() == "dev-1");
        CHECK(heartbeat.value()["timeseries"].as_string() == "2026-10-03 12:00:00");
        CHECK(!heartbeat.value().has("eventCode"));
    }

    const auto event = integration::heike::make_runtime_event(identity, "1", "1");
    CHECK(event.ok());
    if (event.ok()) CHECK(event.value()["data"]["code"].as_string() == "1");

    integration::heike::EnvelopeInput input;
    input.biz_id = "DECL-1";
    input.customs_code = "4401";
    input.event_code = "MODEL003";
    input.event_source = "LIBS-device";
    input.message_id = "123e4567-e89b-12d3-a456-426614174000";
    input.event_content = json::object();
    input.event_content.set("nirSimlarityRESULT", json::object());
    const auto model = integration::heike::make_model_event(input);
    CHECK(model.ok());
    if (model.ok()) {
        CHECK(model.value()["eventCode"].as_string() == "MODEL003");
        CHECK(model.value()["eventContent"].is_object());
        CHECK_EQ(integration::heike::model_event_name("MODEL003"),
                 std::string("SpectrumIdentify"));
    }

    input.event_content = json::Value(R"({"nirSimlarityRESULT":{"similarity":0.9}})");
    input.event_content_as_string = true;
    const auto model_string = integration::heike::make_model_event(input);
    CHECK(model_string.ok());
    if (model_string.ok()) CHECK(model_string.value()["eventContent"].is_string());
}

CASE(heike_libs_raw_spectrum_is_explicitly_not_nir); {
    integration::heike::LibsCaptureContext context;
    context.biz_id = "DECL-1";
    context.customs_code = "4401";
    context.belt_code = "B01";
    context.edge_code = "ZG";
    context.collect_time = "2026-10-03 12:00:00";
    context.seq_no = 1;
    context.message_id = "123e4567-e89b-12d3-a456-426614174000";
    context.event_code = "LIBS_RAW_V1";
    json::Value raw = json::object();
    raw.set("scan_groups", json::array());
    const auto result = integration::heike::make_libs_raw_event(context, raw, true);
    CHECK(result.ok());
    if (result.ok()) {
        CHECK(result.value()["eventContent"].is_string());
        CHECK(!result.value()["contentSize"].as_string().empty());
        CHECK(result.value()["eventCode"].as_string() == "LIBS_RAW_V1");
    }
}

CASE(heike_libs_raw_rejects_reserved_nir_and_model_routes); {
    integration::heike::LibsCaptureContext context;
    context.biz_id = "DECL-1";
    context.customs_code = "4401";
    context.belt_code = "B01";
    context.edge_code = "ZG";
    context.collect_time = "2026-10-03 12:00:00";
    context.message_id = "123e4567-e89b-12d3-a456-426614174000";
    json::Value raw = json::object();
    raw.set("scan_groups", json::array());
    for (const char* code : {"PISYS001", "MODEL003"}) {
        context.event_code = code;
        const auto result = integration::heike::make_libs_raw_event(context, raw);
        CHECK(!result.ok());
    }
}

CASE(heike_model_rejects_libs_raw_content); {
    integration::heike::EnvelopeInput input;
    input.biz_id = "DECL-1";
    input.customs_code = "4401";
    input.event_code = "MODEL003";
    input.event_source = "LIBS-device";
    input.message_id = "123e4567-e89b-12d3-a456-426614174000";
    input.event_content = json::object();
    input.event_content.set("SPECTRUM_SOURCE", json::Value("LIBS"));
    const auto result = integration::heike::make_model_event(input);
    CHECK(!result.ok());
}

CASE(config_conveyor_not_software_controlled_by_default); {
    // 传送带是否由本软件控制尚未确认，默认 false。
    const config::SiteConfig fresh;
    CHECK(!fresh.conveyor.software_controlled);
}

// ── 出光参数：未标定不得启用（与 ac_interlock 同一套纪律）──────────────────
CASE(config_laser_fire_defaults_to_uncommissioned); {
    // 默认未标定。这条很重要：默认值意味着"能出光"就是替现场做了未标定的决定。
    const config::SiteConfig fresh;
    CHECK(!fresh.laser_fire.commissioned);
}

CASE(config_laser_fire_commissioned_without_parameters_is_rejected); {
    // 声明已标定却没给电流/脉宽/Q 延时 = 自相矛盾，必须拒绝而不是拿 0 去点激光。
    auto text = std::string(R"({
        "station_id": "s1",
        "laser":           { "enabled": false },
        "rangefinder":     { "enabled": false },
        "indicator":       { "enabled": false },
        "air_conditioner": { "enabled": false },
        "spectrometer":    { "enabled": false },
        "laser_fire":      { "commissioned": true, "frequency_hz": 1 }
    })");
    bool ok = false;
    const Value v = parsed(text, ok);
    CHECK(ok);
    const auto loaded = config::from_json(v);
    CHECK(!loaded.ok());
}

CASE(config_laser_fire_commissioned_rejects_out_of_range_values); {
    // 已标定就走协议量程校验：15~100A / 50~300us / 1~10Hz / 2~500us。
    // 这条能挡住"把量程下限当成安全值"那类错误：电流 0A 不是最安全，是打不着。
    bool ok = false;
    const Value good = parsed(R"({
        "station_id": "s1",
        "laser":           { "enabled": false },
        "rangefinder":     { "enabled": false },
        "indicator":       { "enabled": false },
        "air_conditioner": { "enabled": false },
        "spectrometer":    { "enabled": false },
        "laser_fire":      { "commissioned": true, "discharge_current_a": 80,
                             "pulse_width_us": 200, "frequency_hz": 1,
                             "q_delay_us": 210, "emission_hold_ms": 3000,
                             "min_discharges": 1 }
    })", ok);
    CHECK(ok);
    CHECK(config::from_json(good).ok());

    // 电流低于量程下限：不是"更安全"，是根本不出光。
    const Value bad = parsed(R"({
        "station_id": "s1",
        "laser":           { "enabled": false },
        "rangefinder":     { "enabled": false },
        "indicator":       { "enabled": false },
        "air_conditioner": { "enabled": false },
        "spectrometer":    { "enabled": false },
        "laser_fire":      { "commissioned": true, "discharge_current_a": 0,
                             "pulse_width_us": 200, "frequency_hz": 1,
                             "q_delay_us": 210, "emission_hold_ms": 3000,
                             "min_discharges": 1 }
    })", ok);
    CHECK(ok);
    CHECK(!config::from_json(bad).ok());
}

CASE(config_laser_fire_roundtrip_keeps_commissioned_parameters); {
    // 落盘再读回，标定标记与数值都不能丢——丢了就会变成"未标定"，
    // 现场表现为"明明标定过却拒绝出光"。
    config::SiteConfig c;
    c.station_id = "lf";
    c.laser.enabled = false;
    c.rangefinder.enabled = false;
    c.indicator.enabled = false;
    c.air_conditioner.enabled = false;
    c.spectrometer.enabled = false;
    c.laser_fire.commissioned = true;
    c.laser_fire.discharge_current_a = 80;
    c.laser_fire.pulse_width_us = 200;
    c.laser_fire.frequency_hz = 1;
    c.laser_fire.q_delay_us = 210;
    c.laser_fire.emission_hold_ms = 3000;
    c.laser_fire.min_discharges = 1;

    const auto text = config::save_string(c);
    CHECK(text.ok());
    if (text.ok()) {
        bool ok = false;
        const Value v = parsed(text.value(), ok);
        CHECK(ok);
        const auto back = config::from_json(v);
        CHECK(back.ok());
        if (back.ok()) {
            CHECK(back.value().laser_fire.commissioned);
            CHECK_EQ(back.value().laser_fire.discharge_current_a, 80u);
            CHECK_EQ(back.value().laser_fire.q_delay_us, 210u);
            CHECK_EQ(back.value().laser_fire.emission_hold_ms, 3000u);
        }
    }
}

CASE(config_roundtrip); {
    config::SiteConfig c;
    c.station_id = "round";
    c.laser.enabled = false;
    c.rangefinder.enabled = false;
    c.indicator.enabled = false;
    c.air_conditioner.enabled = false;
    c.spectrometer.enabled = false;
    const auto text = config::save_string(c);
    CHECK(text.ok());
    if (text.ok()) {
        bool ok = false;
        const Value v = parsed(text.value(), ok);
        CHECK(ok);
        const auto back = config::from_json(v);
        CHECK(back.ok());
        if (back.ok()) {
            CHECK_EQ(back.value().station_id, std::string("round"));
            CHECK(!back.value().laser.enabled);
            CHECK(!back.value().spectrometer.enabled);
        }
    }
}

// ── 门禁（能力 4）─────────────────────────────────────────────────────────


CASE(gate_all_safe_and_parameters_ready_allows); {
    policy::Gate gate;
    gate.set_parameters_ready(true);
    gate.set_laser_commissioned(true);   // 出光参数已标定（未标定一律拦截，见下）
    gate.update_laser_facts(safe_facts());
    gate.arm();
    const policy::GateVerdict verdict = gate.evaluate();
    CHECK(verdict.allowed);
    CHECK(verdict.blocked_by.empty());
}

// 未标定 = 不知道该发什么参数 = 必须拦截。这条与 ac_interlock 的
// "未标定不得启用" 是同一套纪律；区别是这里的默认值就是"未标定"。
CASE(gate_uncommissioned_laser_blocks_even_when_everything_else_is_safe); {
    policy::Gate gate;
    gate.set_parameters_ready(true);
    gate.set_laser_commissioned(false);
    gate.update_laser_facts(safe_facts());
    CHECK(gate.arm().ok());
    const policy::GateVerdict verdict = gate.evaluate();
    CHECK(!verdict.allowed);
    CHECK(blocked_by(verdict, policy::GateReason::laser_not_commissioned));
}

CASE(gate_unreadable_laser_is_not_safe); {
    // 读不到 B2 == 不知道 == 拦截。available=false 绝不能被当成"没有故障"。
    policy::Gate gate;
    gate.set_parameters_ready(true);
    policy::LaserSafetyFacts f;      // available 默认 false
    f.interlock_1 = true;            // 即使位全好也无效
    f.interlock_2 = true;
    f.emergency_stop = true;
    f.q_gate = true;
    f.over_temperature = true;
    f.work_enabled = true;
    gate.update_laser_facts(f);
    const policy::GateVerdict verdict = gate.evaluate();
    CHECK(!verdict.allowed);
    CHECK(blocked_by(verdict, policy::GateReason::laser_unavailable));
}

CASE(gate_incomplete_parameters_block); {
    policy::Gate gate;
    gate.set_parameters_ready(false);
    gate.update_laser_facts(safe_facts());
    const policy::GateVerdict verdict = gate.evaluate();
    CHECK(!verdict.allowed);
    CHECK(blocked_by(verdict, policy::GateReason::parameters_incomplete));
}

CASE(gate_every_safety_bit_has_its_own_reason); {
    struct Probe {
        const char* name;
        policy::GateReason expected;
        void (*mutate)(policy::LaserSafetyFacts&);
    };
    const Probe probes[] = {
        {"interlock_1", policy::GateReason::interlock_open,
         [](policy::LaserSafetyFacts& f) { f.interlock_1 = false; }},
        {"emergency_stop", policy::GateReason::emergency_stopped,
         [](policy::LaserSafetyFacts& f) { f.emergency_stop = false; }},
        {"q_gate", policy::GateReason::q_gate_open,
         [](policy::LaserSafetyFacts& f) { f.q_gate = false; }},
        {"over_temperature", policy::GateReason::over_temperature,
         [](policy::LaserSafetyFacts& f) { f.over_temperature = false; }},
        {"fault_bits", policy::GateReason::laser_fault,
         [](policy::LaserSafetyFacts& f) { f.fault_bits = 0x0004; }},
        {"not_enabled", policy::GateReason::not_enabled,
         [](policy::LaserSafetyFacts&) {}},
    };
    for (const Probe& probe : probes) {
        CASE(gate_bit_probe); { (void)probe; }
        policy::Gate gate;
        gate.set_parameters_ready(true);
        gate.set_laser_enabled(probe.expected != policy::GateReason::not_enabled);
        policy::LaserSafetyFacts f = safe_facts();
        probe.mutate(f);
        gate.update_laser_facts(f);
        const policy::GateVerdict verdict = gate.evaluate();
        CHECK(!verdict.allowed);
        if (!blocked_by(verdict, probe.expected)) {
            std::printf("      (probe %s missed its reason)\n", probe.name);
        }
        CHECK(blocked_by(verdict, probe.expected));
    }
}

CASE(gate_never_returns_empty_blocked_list_when_denied); {
    // 排障需要"到底哪几条不满足"，不能只给一个 bool。
    policy::Gate gate;   // 参数未齐备 + 激光读不到 + 设备未使能
    const policy::GateVerdict verdict = gate.evaluate();
    CHECK(!verdict.allowed);
    CHECK(!verdict.blocked_by.empty());
    CHECK(verdict.primary_reason() != nullptr);
}

CASE(gate_uncalibrated_ac_interlock_never_blocks); {
    // 3.9.2 是 C 类。默认 ac_interlock_ok_ = true（不拦截）。
    // 子项不得互相代偿：3.4.2 出光门禁不得顺手把 3.9.2 的活干了。
    policy::Gate gate;
    gate.set_parameters_ready(true);
    gate.set_laser_commissioned(true);
    gate.update_laser_facts(safe_facts());
    CHECK(gate.arm().ok());
    const policy::GateVerdict verdict = gate.evaluate();
    CHECK(verdict.allowed);
    CHECK(!blocked_by(verdict, policy::GateReason::air_conditioner_interlock));
}

CASE(gate_indicator_mapping_is_pure_and_does_not_affect_verdict); {
    policy::Gate gate;
    gate.set_parameters_ready(true);
    gate.update_laser_facts(safe_facts());
    const policy::GateVerdict before = gate.evaluate();

    policy::IndicatorMapping mapping;
    mapping.table[policy::MachineState::ready][policy::LampChannel::green] =
        policy::LampState::on;
    const auto intent = mapping.map_state(policy::MachineState::ready);
    CHECK_EQ(static_cast<int>(intent.count(policy::LampChannel::green)), 1);
    CHECK_EQ(static_cast<int>(intent.at(policy::LampChannel::green)),
             static_cast<int>(policy::LampState::on));

    const policy::GateVerdict after = gate.evaluate();
    CHECK_EQ(after.allowed, before.allowed);
}

CASE(gate_unknown_machine_state_maps_to_all_off); {
    // 指示层的"未知"是全灭，不是沿用上一次的颜色。
    policy::IndicatorMapping mapping;
    const auto intent = mapping.map_state(policy::MachineState::offline);
    CHECK(intent.empty());
}

CASE(staged_parameters_are_not_reported_as_completed); {
    // 回归测试：at_next_idle 策略下参数只是暂存。此前它报 completed=true，
    // 也就是"说已完成但其实没生效"——正是本层要防的那种撒谎。
    // 落盘路径要自己先建出来：CMake 与 build.ps1 的 CWD 布局不同，
    // 指望"上一轮跑过、build/ 已经在"是把自己绑在别人的行为上。
    std::error_code ec;
    std::filesystem::create_directories("build", ec);
    policy::FileParameterStore store("build/tmp_core_params_staged.json", {"belt"}, {{"belt", {"speed"}}});
    (void)store.load();
    const policy::ApplyResult staged =
        store.apply("belt", {{"speed", 0.2}}, policy::ApplyTiming::at_next_idle);
    CHECK(!staged.completed());
    CHECK(staged.state == policy::ApplyState::staged);
    CHECK_EQ(std::string(policy::to_string(staged.state)), std::string("staged"));

    // immediately 才是真的完成。
    const policy::ApplyResult applied =
        store.apply("belt", {{"speed", 0.2}}, policy::ApplyTiming::immediately);
    CHECK(applied.completed());
    CHECK(applied.state == policy::ApplyState::completed);
}

CASE(apply_state_to_string_covers_every_state); {
    CHECK_EQ(std::string(policy::to_string(policy::ApplyState::completed)), std::string("completed"));
    CHECK_EQ(std::string(policy::to_string(policy::ApplyState::staged)), std::string("staged"));
    CHECK_EQ(std::string(policy::to_string(policy::ApplyState::rejected)), std::string("rejected"));
    CHECK_EQ(std::string(policy::to_string(policy::ApplyState::failed)), std::string("failed"));
}

TEST_MAIN_RETURN()
