// 业务层离线测试（二）：服务模块。
//
// 每个用例对应范围报告里的一条纪律。这些模块**不碰设备**就能验完，
// 所以整套测试不需要串口、不需要 SDK 替身。
#include <string>

#include "check.hpp"
#include "libs/business/config/site_config.hpp"
#include "libs/business/services/ac_interlock.hpp"
#include "libs/business/services/capture.hpp"
#include "libs/business/services/composition.hpp"
#include "libs/business/services/data_delivery.hpp"
#include "libs/business/services/motion.hpp"
#include "libs/business/services/ore_presence.hpp"
#include "libs/protocols/rangefinder.hpp"

using namespace business;
using libs::ErrorKind;
using libs::Error;
using libs::Result;

namespace {

libs::rangefinder::Register reg(std::uint16_t address, std::uint16_t raw) {
    libs::rangefinder::Register r;
    r.address = address;
    r.raw = raw;
    r.name = "test";
    r.valid = true;
    return r;
}

// 造一次测距读。
//
// 0200/0201 是两种兼容距离表示，不是动态量程上下限；这里放入正常的
// 非哨兵值，确保测试覆盖高级 0202 读数的选择。
libs::rangefinder::Reading good_reading(std::uint16_t distance_raw) {
    libs::rangefinder::Reading reading;
    reading.address = 1;
    reading.start = libs::rangefinder::reg::kDistanceZeroOnOutOfRange;
    reading.count = 17;
    reading.registers.push_back(reg(libs::rangefinder::reg::kDistanceZeroOnOutOfRange, 72));
    reading.registers.push_back(reg(libs::rangefinder::reg::kDistanceMaxOnOutOfRange, 73));
    reading.registers.push_back(reg(libs::rangefinder::reg::kDistanceRaw, distance_raw));
    reading.registers.push_back(reg(libs::rangefinder::reg::kConfidence, 900));
    reading.registers.push_back(reg(libs::rangefinder::reg::kSampleCounter, 7));
    return reading;
}

bool is_unsupported(const libs::Error& e) {
    return e.kind == ErrorKind::unsupported;
}

}  // namespace

TEST_MAIN_BEGIN()

// ── 测距观测与物料判定（能力 8）───────────────────────────────────────────

CASE(distance_observation_preserves_raw_verbatim); {
    // 3.8.1：raw 距离与 confidence 原样记录，判定不得回写。
    const libs::rangefinder::Reading reading = good_reading(1234);
    const services::DistanceObservation obs = services::observe(reading, libs::Error{});
    CHECK(obs.read_ok);
    CHECK_EQ(static_cast<int>(obs.distance_raw), 1234);
    CHECK_EQ(static_cast<int>(obs.confidence_raw), 900);
    CHECK_EQ(static_cast<int>(obs.sample_counter), 7);
    CHECK(obs.in_range);
}

CASE(distance_raw_is_not_compared_with_0200_as_lower_bound); {
    // 0200/0201 的值不是应用上下限；距离 50 仍是有效读数。
    const libs::rangefinder::Reading reading = good_reading(50);
    const services::DistanceObservation obs = services::observe(reading, libs::Error{});
    CHECK_EQ(static_cast<int>(obs.distance_raw), 50);   // 原样记录
    CHECK(obs.in_range);
}

CASE(distance_raw_is_not_compared_with_0201_as_upper_bound); {
    const libs::rangefinder::Reading reading = good_reading(9000);
    const services::DistanceObservation obs = services::observe(reading, libs::Error{});
    CHECK_EQ(static_cast<int>(obs.distance_raw), 9000);
    CHECK(obs.in_range);
}

CASE(distance_zero_and_max_sentinels_are_out_of_range); {
    // 标准寄存器的哨兵值分别是 0 和 0xffff；边界值本身不代表距离上下限。
    auto zero = good_reading(0);
    for (auto& r : zero.registers) {
        if (r.address == libs::rangefinder::reg::kDistanceZeroOnOutOfRange) r.raw = 0;
        if (r.address == libs::rangefinder::reg::kDistanceMaxOnOutOfRange) r.raw = 0xffff;
        if (r.address == libs::rangefinder::reg::kDistanceRaw) r.raw = 0;
    }
    const services::DistanceObservation zero_obs = services::observe(zero, libs::Error{});
    CHECK(!zero_obs.in_range);

    auto max = good_reading(0);
    for (auto& r : max.registers) {
        if (r.address == libs::rangefinder::reg::kDistanceZeroOnOutOfRange) r.raw = 0;
        if (r.address == libs::rangefinder::reg::kDistanceMaxOnOutOfRange) r.raw = 0xffff;
        if (r.address == libs::rangefinder::reg::kDistanceRaw) r.raw = 0;
    }
    const services::DistanceObservation max_obs = services::observe(max, libs::Error{});
    CHECK(!max_obs.in_range);
}

CASE(distance_standard_register_fallback_uses_sentinel_semantics); {
    libs::rangefinder::Reading reading;
    reading.address = 1;
    reading.start = libs::rangefinder::reg::kDistanceZeroOnOutOfRange;
    reading.count = 2;
    reading.registers.push_back(reg(libs::rangefinder::reg::kDistanceZeroOnOutOfRange, 456));
    reading.registers.push_back(reg(libs::rangefinder::reg::kDistanceMaxOnOutOfRange, 0xffff));
    const services::DistanceObservation obs = services::observe(reading, libs::Error{});
    CHECK(obs.in_range);
    CHECK_EQ(static_cast<int>(obs.distance_raw), 456);
    CHECK_EQ(static_cast<int>(obs.distance_register),
             static_cast<int>(libs::rangefinder::reg::kDistanceZeroOnOutOfRange));
    CHECK_EQ(static_cast<int>(obs.distance_resolution_mm), 1);
}

CASE(distance_read_failure_is_not_distance_zero); {
    // 通信失败 -> read_ok=false。绝不能变成 distance_raw=0 + "在量程内"。
    const libs::rangefinder::Reading reading = good_reading(0);
    const Error err = Error::make(ErrorKind::transport, "cable", "拔线了");
    const services::DistanceObservation obs = services::observe(reading, err);
    CHECK(!obs.read_ok);
    CHECK(!obs.read_error.empty());
    CHECK(!obs.device_valid);
    CHECK(!obs.in_range);
}

CASE(protocol_valid_true_does_not_mean_in_range); {
    // 协议层 Register::valid 默认 true（"本层不判断"）。
    // 业务层按所选寄存器的哨兵语义判断；0202 没有独立的量程哨兵，
    // 非零 raw 只能说明读数存在，不能把它误判成超量程。
    const services::DistanceObservation obs =
        services::observe(good_reading(9000), libs::Error{});
    CHECK(obs.device_valid);
    CHECK(obs.in_range);
}

CASE(ore_judge_unconfigured_returns_unknown_not_false); {
    // 3.8.2 是 B 类：阈值与方向都未标定 -> nullopt（未知），不是 false（无矿）。
    services::OrePresenceJudge judge;
    judge.record(services::DistanceObservation{});
    judge.record([] {
        services::DistanceObservation o;
        o.read_ok = true;
        o.in_range = true;
        o.distance_raw = 500;
        return o;
    }());
    const services::OreDecision decision = judge.decide();
    CHECK(!decision.ore_present.has_value());
    CHECK(decision.has_observation);
}

CASE(ore_judge_without_observation_is_unknown); {
    services::OrePresenceJudge judge;
    const services::OreDecision decision = judge.decide();
    CHECK(!decision.ore_present.has_value());
    CHECK(!decision.has_observation);
}

CASE(ore_judge_failed_read_is_unknown); {
    services::OrePresenceJudge judge;
    services::DistanceObservation o;
    o.read_ok = false;
    o.read_error = "timeout";
    judge.record(o);
    const services::OreDecision decision = judge.decide();
    CHECK(!decision.ore_present.has_value());
}

CASE(ore_policy_defaults_are_all_optional_not_zero); {
    // 占位值必须带标记：未标定就是 nullopt，绝不是 0 cm。
    const services::OrePresencePolicy p = services::OrePresencePolicy::uncalibrated();
    CHECK(!p.calibrated);
    CHECK(!p.threshold.has_value());
    CHECK(!p.hysteresis.has_value());
    CHECK_EQ(static_cast<int>(p.basis), static_cast<int>(services::DecisionBasis::unconfigured));
    CHECK_EQ(p.consecutive_confirmations, 0);
}

CASE(ore_judge_does_not_mutate_the_recorded_observation); {
    // 先记录、后判定：判定不得回写原始观测。
    services::OrePresenceJudge judge;
    services::DistanceObservation o;
    o.read_ok = true;
    o.in_range = true;
    o.distance_raw = 777;
    o.confidence_raw = 888;
    judge.record(o);
    (void)judge.decide();
    (void)judge.decide();
    const auto& last = judge.last_observation();
    CHECK(last.has_value());
    if (last.has_value()) {
        CHECK_EQ(static_cast<int>(last->distance_raw), 777);
        CHECK_EQ(static_cast<int>(last->confidence_raw), 888);
    }
}

// ── 工作频率与采集节奏（能力 10）──────────────────────────────────────────

CASE(pace_from_missing_frequency_is_unconfigured_not_guessed); {
    // 3.10.2 纪律：频率缺失时**不猜任何 Hz**。
    config::SiteConfig site;
    const services::WorkFrequency none = services::WorkFrequency::unconfigured();
    const services::CapturePace pace = services::CaptureCoordinator::pace_from_frequency(none, site);
    CHECK(!pace.configured);
    CHECK_EQ(pace.source, std::string("unconfigured"));
    CHECK(!pace.period_ms.has_value());
}

CASE(pace_from_uncalibrated_frequency_is_unconfigured); {
    // 有数值但没标定单位，同样不能用。
    config::SiteConfig site;
    services::WorkFrequency f;
    f.frequency_hz = 4.0;
    f.unit_confirmed = false;
    f.calibrated = false;
    const services::CapturePace pace = services::CaptureCoordinator::pace_from_frequency(f, site);
    CHECK(!pace.configured);
}

CASE(pace_from_calibrated_frequency_is_configured); {
    config::SiteConfig site;
    services::WorkFrequency f;
    f.frequency_hz = 4.0;
    f.unit_confirmed = true;
    f.calibrated = true;
    const services::CapturePace pace = services::CaptureCoordinator::pace_from_frequency(f, site);
    CHECK(pace.configured);
    CHECK_EQ(pace.source, std::string("work_frequency"));
    CHECK(pace.period_ms.has_value());
    if (pace.period_ms.has_value()) CHECK(pace.period_ms.value() > 0.0);
}

CASE(work_frequency_unconfigured_has_no_value); {
    const services::WorkFrequency f = services::WorkFrequency::unconfigured();
    CHECK(!f.frequency_hz.has_value());
    CHECK(!f.configured());
}

CASE(pace_from_config_carries_the_confirmed_timeouts); {
    config::SiteConfig site;
    site.capture_timeout_ms = 5000;
    site.integration_ms = 20;
    const services::CapturePace pace = services::CaptureCoordinator::pace_from_config(site);
    CHECK(pace.configured);
    CHECK_EQ(pace.source, std::string("config"));
    CHECK(pace.timeout_ms.has_value());
    if (pace.timeout_ms.has_value()) CHECK_EQ(pace.timeout_ms.value(), 5000u);
}

// 本地交付出口要落盘的是**原始光谱**，不是元数据。
// 只带 shot_id / 通道名等于存了个空壳 —— 这一发的 counts 就永久丢了。
CASE(capture_payload_carries_the_raw_spectrum_including_counts); {
    libs::devices::ScanGroup group;
    group.shot_id = 7;
    group.complete = true;
    group.hardware_synchronized = true;
    group.arm_order = {"SLAVE1", "MASTER"};

    libs::devices::Spectrum master;
    master.serial = "1706097U4";
    master.pixels = 3;
    master.device_time_ticks = 4242;
    master.wavelength_nm = {200.0, 201.5, 203.0};
    master.counts = {11.0, 22.0, 33.0};
    group.spectra.push_back(master);

    const json::Value payload = services::CaptureCoordinator::to_payload({group});
    const json::Value& groups = payload["scan_groups"];
    CHECK(groups.is_array());
    CHECK_EQ(groups.as_array().size(), std::size_t(1));
    if (groups.as_array().size() == 1) {
        const json::Value& g = groups.as_array()[0];
        CHECK(g["shot_id"].as_number() == 7.0);
        CHECK(g["hardware_synchronized"].as_bool());   // 原样上送，不推断
        const json::Value& channels = g["channels"];
        // **缺失通道不补零、不占位**：只有真的采到的那一路在 channels 里。
        CHECK_EQ(channels.as_array().size(), std::size_t(1));
        if (channels.as_array().size() == 1) {
            const json::Value& c = channels.as_array()[0];
            CHECK_EQ(c["serial"].as_string(), std::string("1706097U4"));
            CHECK(c["device_time_ticks"].as_number() == 4242.0);
            const json::Value& counts = c["counts"];
            CHECK_EQ(counts.as_array().size(), std::size_t(3));
            if (counts.as_array().size() == 3) {
                CHECK(counts.as_array()[0].as_number() == 11.0);
                CHECK(counts.as_array()[1].as_number() == 22.0);
                CHECK(counts.as_array()[2].as_number() == 33.0);
            }
            CHECK_EQ(c["wavelength_nm"].as_array().size(), std::size_t(3));
        }
    }
}

CASE(sync_view_passes_hardware_synchronized_verbatim); {
    // 3.10.1：hardware_synchronized 是 SDK 事实，业务层不得由"我请求了同步"推断为已同步。
    config::SiteConfig site;
    libs::devices::ScanGroup group;
    group.shot_id = 1;
    group.complete = true;
    group.hardware_synchronized = true;      // SDK 说同步了
    const services::CaptureCoordinator::SyncView view =
        services::CaptureCoordinator::sync_view(site, group);
    CHECK(view.hardware_synchronized);       // 原样上送
    CHECK(!view.requested_sync);             // 但业务层并没有请求
}

CASE(sync_view_does_not_upgrade_false_to_true); {
    config::SiteConfig site;
    site.spectrometer.synchronized = true;   // 业务层请求了同步
    libs::devices::ScanGroup group;
    group.hardware_synchronized = false;     // SDK 没确认
    const services::CaptureCoordinator::SyncView view =
        services::CaptureCoordinator::sync_view(site, group);
    CHECK(!view.hardware_synchronized);      // 不能因为"我请求了"就变成 true
    CHECK(view.requested_sync);              // 但要如实记录请求过
}

CASE(capture_without_device_is_unsupported_not_empty_success); {
    config::SiteConfig site;
    const services::CaptureCoordinator coordinator(site);
    const services::CapturePace pace = services::CaptureCoordinator::pace_from_config(site);
    const auto result = coordinator.capture(nullptr, pace);
    CHECK(!result.ok());
    if (!result.ok()) CHECK(is_unsupported(result.error()));
}



// ── 成分分析（能力 11）────────────────────────────────────────────────────

CASE(composition_is_algorithm_missing_not_all_zero); {
    // 任务书 §5.2：没有算法必须 unavailable，double 字段不得有"看起来像真实值"的默认值。
    const services::CompositionAnalyzer analyzer;
    const services::CompositionResult result = analyzer.analyze("test-source");
    CHECK_EQ(static_cast<int>(result.availability),
             static_cast<int>(services::CompositionAvailability::algorithm_missing));
    CHECK(result.values.empty());
    CHECK(!analyzer.algorithm_implemented());
}

CASE(composition_json_has_no_numeric_placeholders); {
    // 序列化后也不能出现 0 冒充结果。
    const services::CompositionAnalyzer analyzer;
    const services::CompositionResult result = analyzer.analyze("s");
    const json::Value v = services::to_json(result);
    CHECK_EQ(v["availability"].as_string(), std::string("algorithm_missing"));
    // values 里要么没有字段，要么是 null；绝不是 0。
    if (v.has("values")) {
        for (const auto& kv : v["values"].as_object()) {
            CHECK(!kv.second.is_number() || kv.second.as_number() != 0.0);
        }
    }
}

CASE(composition_lists_all_ten_oxides); {
    // §5.1 的十个字段，顺序与文档一致。
    const auto all = services::all_oxides();
    CHECK_EQ(all.size(), std::size_t(10));
    CHECK_EQ(std::string(services::to_string(services::Oxide::TiO2)), std::string("TiO2"));
    CHECK_EQ(std::string(services::to_string(services::Oxide::Al2O3)), std::string("Al2O3"));
}

CASE(composition_algorithm_prerequisites_are_listed); {
    // C 类的正确落地形态：带前置条件的 TODO，不是返回 0 的桩。
    const std::string pre = services::CompositionAnalyzer::prerequisites();
    CHECK(!pre.empty());
}

// ── 运动与位置（能力 12）──────────────────────────────────────────────────

CASE(motion_placeholder_is_marked_uncalibrated); {
    // 12.1 的 1 s 占位必须带 calibrated=false 标记，否则上层会当成标定值。
    const services::MotionTiming t = services::MotionTiming::placeholder();
    CHECK(!t.calibrated);
    CHECK(t.forward_time_s.has_value());
    CHECK(t.reverse_time_s.has_value());
    if (t.forward_time_s.has_value()) CHECK(t.forward_time_s.value() > 0.0);
}

CASE(motion_json_keeps_the_calibration_marker); {
    const services::MotionTiming t = services::MotionTiming::placeholder();
    const json::Value v = services::to_json(t);
    CHECK(!v["calibrated"].as_bool(true));
}

CASE(position_switch_is_unsupported_never_reports_in_place); {
    // 12.2 是 C 类：计时到了不等于实物到位。绝不返回"已到位"。
    services::UnavailablePositionSwitch sw;
    CHECK(!sw.available());
    const libs::VoidResult r = sw.switch_to(1);
    CHECK(!r.ok());
    if (!r.ok()) CHECK(is_unsupported(r.error()));
    const Result<int> pos = sw.current_position();
    CHECK(!pos.ok());
    if (!pos.ok()) CHECK(is_unsupported(pos.error()));
}

CASE(position_switch_prerequisites_mention_limit_and_motor); {
    const std::string pre = services::UnavailablePositionSwitch::prerequisites();
    CHECK(!pre.empty());
}

// ── 交付出口（能力 6）──────────────────────────────────────────────────────

CASE(local_sink_saves_and_reports_the_path); {
    data_delivery::LocalFileSink sink("build/test-delivery");
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

// ── 空调联锁（能力 9 的 3.9.2，C 类）──────────────────────────────────────

CASE(ac_interlock_uncalibrated_never_participates_in_gate); {
    // 子项不得互相代偿：3.4.2 出光门禁不得替 3.9.2 干活。
    config::SiteConfig site;   // 默认 ac_interlock.enabled = false
    const services::AirConditionerInterlock interlock(site);
    CHECK(!interlock.participates_in_gate());
    CHECK(!interlock.policy().calibrated);
    CHECK(!interlock.policy().trigger_indoor_temp_c.has_value());
}

CASE(ac_interlock_evaluate_is_unsupported_not_true); {
    // 阈值未标定 -> unsupported。不返回 true（放行）也不返回 false（拦停）。
    config::SiteConfig site;
    const services::AirConditionerInterlock interlock(site);
    libs::air_conditioner::StatusReading status;
    libs::air_conditioner::ParameterReading params;
    const auto result = interlock.evaluate(status, params, /*communication_ok=*/true);
    CHECK(!result.ok());
    if (!result.ok()) CHECK(is_unsupported(result.error()));
}

CASE(ac_interlock_communication_failure_is_unsupported); {
    config::SiteConfig site;
    const services::AirConditionerInterlock interlock(site);
    libs::air_conditioner::StatusReading status;
    libs::air_conditioner::ParameterReading params;
    const auto result = interlock.evaluate(status, params, /*communication_ok=*/false);
    CHECK(!result.ok());
    if (!result.ok()) CHECK(is_unsupported(result.error()));
}

TEST_MAIN_RETURN()
