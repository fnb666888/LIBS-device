// 业务层离线测试（二）：服务模块。
//
// 覆盖：采集节奏（能力 10）、采集同步视图、交付出口（能力 6）、
// 以及状态灯同步（预检通过 = 绿灯）。这些模块**不碰设备**就能验完，
// 所以整套测试不需要串口、不需要 SDK 替身。
#include <string>

#include "check.hpp"
#include "libs/business/shared/config/site_config.hpp"#include "libs/business/shared/data_delivery.hpp"
#include "libs/business/readiness/lamp.hpp"

using namespace business;
using libs::ErrorKind;
using libs::Error;
using libs::Result;

namespace {

bool is_unsupported(const libs::Error& e) {
    return e.kind == ErrorKind::unsupported;
}

// 取某一路上该点亮的灯色。map 里没这一路 = 驱动不会去写它。
libs::indicator::State lamp_of(const std::map<libs::indicator::Channel, libs::indicator::State>& m,
                               libs::indicator::Channel ch) {
    const auto it = m.find(ch);
    return it == m.end() ? libs::indicator::State::off : it->second;
}

bool green_only(const std::map<libs::indicator::Channel, libs::indicator::State>& m) {
    return lamp_of(m, libs::indicator::Channel::green) == libs::indicator::State::on &&
           lamp_of(m, libs::indicator::Channel::red)   == libs::indicator::State::off &&
           lamp_of(m, libs::indicator::Channel::yellow) == libs::indicator::State::off;
}
}  // namespace

TEST_MAIN_BEGIN()

// ── 交付出口（能力 6）──────────────────────────────────────────────────────

CASE(local_sink_saves_and_reports_the_path); {
    data_delivery::LocalFileSink sink(::testing::scratch("build/tmp_delivery"));
    data_delivery::CaptureReference ref;
    ref.station_id = "s1";
    ref.shot_id = 42;
    const auto result = sink.deliver(ref, "{\"spectra\":[]}");
    CHECK(result.ok());
    if (result.ok()) {
        CHECK(result.value().local_saved);
        CHECK(!result.value().local_path.empty());
        // 本地存了**不等于**海关收到了。
        CHECK(!result.value().gateway_delivered);
    }
}

CASE(local_sink_failure_does_not_claim_success); {
    // 写到不存在的路径 -> local_saved=false 且带原因，不假装成功。
    data_delivery::LocalFileSink sink("Z:/definitely/not/a/real/path/xyz");
    data_delivery::CaptureReference ref;
    const auto result = sink.deliver(ref, "blob");
    CHECK(!result.ok() || !result.value().local_saved);
}

CASE(gateway_sink_never_claims_delivery); {
    // 3.6.2 是 C 类：绝不返回 gateway_delivered=true。
    // 形态是"调用成功但状态位为 false + 写明前置条件"，不是抛 unsupported：
    // §3.6.3 要求两个交付状态彼此独立，不能因为网关不可用就吞掉本地结果。
    data_delivery::UnsupportedGatewaySink sink;
    data_delivery::CaptureReference ref;
    const auto result = sink.deliver(ref, "blob");
    CHECK(result.ok());
    if (result.ok()) {
        CHECK(!result.value().gateway_delivered);
        CHECK(!result.value().gateway_error.empty());
        // 本地状态位不受网关影响。
        CHECK(!result.value().local_saved);
    }
    CHECK(!data_delivery::UnsupportedGatewaySink::prerequisites().empty());
}

CASE(delivery_status_fields_are_independent); {
    // 3.6.3：两个状态位彼此独立，不是一个三态枚举。
    data_delivery::DeliveryStatus s;
    CHECK(!s.local_saved);
    CHECK(!s.gateway_delivered);
    s.local_saved = true;
    CHECK(!s.gateway_delivered);   // 本地存了，海关没收到
    s.gateway_delivered = true;
    s.local_saved = false;         // 反过来也成立
    CHECK(s.gateway_delivered);
    CHECK(!s.local_saved);
}

// ── 状态灯同步（"预检通过就亮绿灯"这条纪律的落点）────────────────────────

CASE(precheck_pass_lights_the_green_lamp); {
    // 这是本次唯一新增的业务规则：门禁放行 = 绿灯常亮。
    const auto m = readiness::lamp_states(readiness::MachineState::ready);
    CHECK(green_only(m));
}

CASE(acquiring_is_a_normal_state_and_also_green); {
    // 出光采集中不是异常 —— 现场看到的就是绿灯，绝不能变黄变红。
    const auto m = readiness::lamp_states(readiness::MachineState::acquiring);
    CHECK(green_only(m));
}

CASE(not_ready_never_lights_green); {
    // 绿灯只能由 ready/acquiring 换来。其余状态一律不得出现绿灯，
    // 否则"灯是绿的"就不再等价于"可以出光"。
    for (const auto st : {readiness::MachineState::standby,
                          readiness::MachineState::fault,
                          readiness::MachineState::offline,
                          readiness::MachineState::recovering}) {
        const auto m = readiness::lamp_states(st);
        CHECK(lamp_of(m, libs::indicator::Channel::green) != libs::indicator::State::on);
    }
}

CASE(standby_is_yellow_and_fault_is_red); {
    const auto standby = readiness::lamp_states(readiness::MachineState::standby);
    CHECK(lamp_of(standby, libs::indicator::Channel::yellow) == libs::indicator::State::on);
    CHECK(lamp_of(standby, libs::indicator::Channel::red) == libs::indicator::State::off);
    const auto fault = readiness::lamp_states(readiness::MachineState::fault);
    CHECK(lamp_of(fault, libs::indicator::Channel::red) == libs::indicator::State::on);
    CHECK(lamp_of(fault, libs::indicator::Channel::yellow) == libs::indicator::State::off);
}

CASE(offline_turns_every_lamp_off); {
    // 设备没打开 = 全灭。不能留一圈灯在假装系统还活着。
    const auto m = readiness::lamp_states(readiness::MachineState::offline);
    for (const auto ch : {libs::indicator::Channel::red, libs::indicator::Channel::yellow,
                          libs::indicator::Channel::green, libs::indicator::Channel::blue,
                          libs::indicator::Channel::white, libs::indicator::Channel::buzzer}) {
        CHECK(lamp_of(m, ch) == libs::indicator::State::off);
    }
}

CASE(buzzer_is_never_rung_by_policy); {
    // 现场噪声大，误鸣比不鸣更糟。所有状态下蜂鸣器都必须关着。
    for (const auto st : {readiness::MachineState::ready, readiness::MachineState::acquiring,
                          readiness::MachineState::standby, readiness::MachineState::fault,
                          readiness::MachineState::recovering, readiness::MachineState::offline}) {
        const auto m = readiness::lamp_states(st);
        CHECK(lamp_of(m, libs::indicator::Channel::buzzer) == libs::indicator::State::off);
    }
}

CASE(precheck_describes_the_complete_lamp_intent); {
    // 六路是业务意图；实际全灭/点亮指令由驱动按型号选择。
    const auto m = readiness::lamp_states(readiness::MachineState::ready);
    CHECK_EQ(m.size(), std::size_t(6));
    CHECK(m.at(libs::indicator::Channel::buzzer) == libs::indicator::State::off);
}

CASE(precheck_lamp_writes_are_never_silent); {
    // 意图名与灯色必须一一对应，且不为空的意图名 —— status 直接把它给人看。
    CHECK_EQ(std::string(readiness::lamp_intent_name(readiness::MachineState::standby)),
             std::string("standby_yellow"));
    CHECK_EQ(std::string(readiness::lamp_intent_name(readiness::MachineState::fault)),
             std::string("fault_red"));
}

TEST_MAIN_RETURN()
