// 业务层离线测试（一）：JSON、站点配置、门禁、参数。
//
// 这些用例全部不碰串口、不加载 DLL——它们验的是**纪律**，不是设备行为。
// 任务书 §7 的每一条红线在这里都有一个对应的可执行断言。
#include <filesystem>
#include <string>

#include "check.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/business/shared/config/site_config.hpp"
#include "libs/business/shared/config/parameter_store.hpp"
#include "libs/business/readiness/facts.hpp"
#include "libs/business/readiness/verdict.hpp"
#include "libs/business/readiness/lamp.hpp"

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
readiness::LaserSafetyFacts safe_facts() {
    readiness::LaserSafetyFacts f;
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

// 一份"全部条件都满足"的判定输入。逐项改坏来测每条拦截路径。
readiness::ReadinessFacts armed_facts() {
    readiness::ReadinessFacts f;
    f.laser = safe_facts();
    f.devices_opened = true;
    f.spectrometer_ready = true;
    f.parameters_complete = true;
    f.laser_enabled = true;
    f.laser_commissioned = true;
    f.armed = true;
    return f;
}
bool blocked_by(const readiness::GateVerdict& verdict, readiness::GateReason reason) {
    for (readiness::GateReason r : verdict.blocked_by) {
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
        "indicator":       { "enabled": false },
        "spectrometer":    { "enabled": false }
    })";
    bool ok = false;
    const Value v = parsed(text, ok);
    CHECK(ok);
    const auto loaded = config::from_json(v);
    CHECK(loaded.ok());
    if (loaded.ok()) {
        CHECK_EQ(loaded.value().station_id, std::string("s1"));
        CHECK_EQ(loaded.value().laser.port, std::string("COM10"));
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
    }
}

CASE(config_default_is_rejected_not_silently_accepted); {
    // 反过来：空 SiteConfig 必须被 validate 拒绝（禁止静默回落默认值）。
    const libs::VoidResult v = config::validate(config::SiteConfig{});
    CHECK(!v.ok());
    CHECK_EQ(static_cast<int>(v.error().kind), static_cast<int>(libs::ErrorKind::invalid_argument));
}

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
        "indicator":       { "enabled": false },
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
        "indicator":       { "enabled": false },
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
        "indicator":       { "enabled": false },
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
    c.indicator.enabled = false;
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
    c.indicator.enabled = false;
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


CASE(evaluate_all_safe_and_parameters_ready_allows); {
    const readiness::GateVerdict verdict = readiness::evaluate(armed_facts());
    CHECK(verdict.allowed);
    CHECK(verdict.blocked_by.empty());
}

// "未标定不得启用" 是同一套纪律；区别是默认值就是"未标定"。
CASE(evaluate_uncommissioned_laser_blocks_even_when_everything_else_is_safe); {
    readiness::ReadinessFacts f = armed_facts();
    f.laser_commissioned = false;
    const readiness::GateVerdict verdict = readiness::evaluate(f);
    CHECK(!verdict.allowed);
    CHECK(blocked_by(verdict, readiness::GateReason::laser_not_commissioned));
}

CASE(evaluate_unreadable_laser_is_not_safe); {
    // 读不到 B2 == 不知道 == 拦截。available=false 绝不能被当成"没有故障"。
    readiness::ReadinessFacts f = armed_facts();
    f.laser = readiness::LaserSafetyFacts();   // available 默认 false
    f.laser.interlock_1 = true;                // 即使位全好也无效
    f.laser.interlock_2 = true;
    f.laser.emergency_stop = true;
    f.laser.q_gate = true;
    f.laser.over_temperature = true;
    f.laser.work_enabled = true;
    const readiness::GateVerdict verdict = readiness::evaluate(f);
    CHECK(!verdict.allowed);
    CHECK(blocked_by(verdict, readiness::GateReason::laser_unavailable));
}

CASE(evaluate_incomplete_parameters_block); {
    readiness::ReadinessFacts f = armed_facts();
    f.parameters_complete = false;
    const readiness::GateVerdict verdict = readiness::evaluate(f);
    CHECK(!verdict.allowed);
    CHECK(blocked_by(verdict, readiness::GateReason::parameters_incomplete));
}

CASE(evaluate_requires_arming); {
    // 布防是状态机的一步：disarm -> (条件齐备) -> arm -> 可出光。
    // 条件齐备但没 arm，仍然不放行 —— 否则"自动布防"就成了默认开光。
    //
    // 未布防**不是**一个 GateReason：它不是"某条条件不满足"，
    // 而是"还没按下布防"。blocked_by 为空、allowed=false、detail 说清原因，
    // 这样 UI 不会把"没布防"错报成"联锁断开"。
    readiness::ReadinessFacts f = armed_facts();
    f.armed = false;
    const readiness::GateVerdict verdict = readiness::evaluate(f);
    CHECK(!verdict.allowed);
    CHECK(verdict.blocked_by.empty());
    CHECK_EQ(verdict.detail, std::string("未布防"));
}

CASE(evaluate_every_safety_bit_has_its_own_reason); {
    struct Probe {
        const char* name;
        readiness::GateReason expected;
        void (*mutate)(readiness::ReadinessFacts&);
    };
    const Probe probes[] = {
        {"interlock_1", readiness::GateReason::interlock_open,
         [](readiness::ReadinessFacts& f) { f.laser.interlock_1 = false; }},
        {"interlock_2", readiness::GateReason::interlock_open,
         [](readiness::ReadinessFacts& f) { f.laser.interlock_2 = false; }},
        {"emergency_stop", readiness::GateReason::emergency_stopped,
         [](readiness::ReadinessFacts& f) { f.laser.emergency_stop = false; }},
        {"q_gate", readiness::GateReason::q_gate_open,
         [](readiness::ReadinessFacts& f) { f.laser.q_gate = false; }},
        {"over_temperature", readiness::GateReason::over_temperature,
         [](readiness::ReadinessFacts& f) { f.laser.over_temperature = false; }},
        {"fault_bits", readiness::GateReason::laser_fault,
         [](readiness::ReadinessFacts& f) { f.laser.fault_bits = 0x0004; }},
        {"devices_opened", readiness::GateReason::not_enabled,
         [](readiness::ReadinessFacts& f) { f.devices_opened = false; }},
        {"laser_enabled", readiness::GateReason::not_enabled,
         [](readiness::ReadinessFacts& f) { f.laser_enabled = false; }},
        {"spectrometer_ready", readiness::GateReason::not_enabled,
         [](readiness::ReadinessFacts& f) { f.spectrometer_ready = false; }},
    };
    for (const Probe& probe : probes) {
        readiness::ReadinessFacts f = armed_facts();
        probe.mutate(f);
        const readiness::GateVerdict verdict = readiness::evaluate(f);
        CHECK(!verdict.allowed);
        if (!blocked_by(verdict, probe.expected)) {
            std::printf("      (probe %s missed its reason)\n", probe.name);
        }
        CHECK(blocked_by(verdict, probe.expected));
    }
}

CASE(emission_phase_preserves_interlocks_while_accepting_the_commanded_q_gate); {
    auto facts = armed_facts();
    facts.laser.q_gate = false;  // B2 physically reports the gate open.
    CHECK(!readiness::evaluate(facts).allowed);
    CHECK(blocked_by(readiness::evaluate(facts), readiness::GateReason::q_gate_open));
    const auto phase = readiness::GateCheckPhase::during_emission;
    CHECK(readiness::evaluate(facts, phase).allowed);
    const auto safe = facts;
    const auto denied = [&](readiness::GateReason reason) {
        const auto verdict = readiness::evaluate(facts, phase);
        CHECK(!verdict.allowed);
        CHECK(blocked_by(verdict, reason));
        facts = safe;
    };
    facts.laser.interlock_1 = false; denied(readiness::GateReason::interlock_open);
    facts.laser.interlock_2 = false; denied(readiness::GateReason::interlock_open);
    facts.laser.emergency_stop = false; denied(readiness::GateReason::emergency_stopped);
    facts.laser.over_temperature = false; denied(readiness::GateReason::over_temperature);
    facts.laser.fault_bits = 4; denied(readiness::GateReason::laser_fault);
    facts.laser.available = false; denied(readiness::GateReason::laser_unavailable);
    facts.parameters_complete = false; denied(readiness::GateReason::parameters_incomplete);
    facts.laser_commissioned = false; denied(readiness::GateReason::laser_not_commissioned);
    facts.devices_opened = false; denied(readiness::GateReason::not_enabled);
    facts.laser_enabled = false; denied(readiness::GateReason::not_enabled);
    facts.spectrometer_ready = false; denied(readiness::GateReason::not_enabled);
    facts.armed = false; CHECK(!readiness::evaluate(facts, phase).allowed);
}

CASE(evaluate_never_returns_empty_blocked_list_when_denied); {
    // 排障需要"到底哪几条不满足"，不能只给一个 bool。
    const readiness::ReadinessFacts f{};   // 全默认：未布防 + 读不到激光 + 未使能
    const readiness::GateVerdict verdict = readiness::evaluate(f);
    CHECK(!verdict.allowed);
    CHECK(!verdict.blocked_by.empty());
    CHECK(verdict.primary_reason() != nullptr);
}

// 判定是纯函数：同一份 facts 反复算，结论必须一模一样。
// 这条不是形式主义 —— 现场重判会跑成千上万次，非幂等就意味着"上一帧放行、
// 这一帧拦下"的抖动，操作员会看到按钮一会儿能点一会儿不能点。
CASE(evaluate_is_idempotent_on_identical_facts); {
    const readiness::ReadinessFacts f = armed_facts();
    const readiness::GateVerdict a = readiness::evaluate(f);
    const readiness::GateVerdict b = readiness::evaluate(f);
    CHECK_EQ(a.allowed, b.allowed);
    CHECK_EQ(a.blocked_by.size(), b.blocked_by.size());
    CHECK_EQ(a.detail, b.detail);
}
CASE(parameters_apply_takes_effect_immediately); {
    // 回归测试：曾经有 at_next_idle（暂存到下次空闲）这条路，但全库没有任何
    // commit 点去应用暂存值 —— 结果是"界面改了参数，这次采集还是旧值"。
    // 那是本层最典型的撒谎（说暂存却不生效，还报 completed），已连同
    // staged 状态一起删除。现在 apply 必须当场改、当场可读回。
    std::error_code ec;
    std::filesystem::create_directories("build", ec);
    const std::string path =
        ::testing::scratch("build/tmp_core_params_immediate.json", false);
    config::FileParameterStore store(path, {"laser"}, {{"laser", {"frequency_hz"}}});
    (void)store.load();
    const config::ApplyResult applied =
        store.apply("laser", {{"frequency_hz", 2}});
    CHECK(applied.completed());
    CHECK(applied.state == config::ApplyState::completed);

    std::map<std::string, double> values;
    CHECK(store.query("laser", values).state == config::ApplyState::completed);
    const auto it = values.find("frequency_hz");
    CHECK(it != values.end());
    CHECK_EQ(it->second, 2.0);
}

CASE(apply_state_to_string_covers_every_state); {
    CHECK_EQ(std::string(config::to_string(config::ApplyState::completed)), std::string("completed"));
    CHECK_EQ(std::string(config::to_string(config::ApplyState::rejected)), std::string("rejected"));
    CHECK_EQ(std::string(config::to_string(config::ApplyState::failed)), std::string("failed"));
}

TEST_MAIN_RETURN()
