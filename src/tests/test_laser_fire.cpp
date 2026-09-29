// 出光流程测试（本次新增）。
//
// 出光是**唯一有物理后果的业务动作**，也是原方案里明确缺失的一环。
// 本测试钉死四件事：
//
//   1. **门禁不放行时，一条写指令都不许发**。用假串口数"发了多少帧"并断言为 0
//      ——只看返回值不够，指令发出去就晚了。
//   2. **联锁断开/急停/过温必须拦得住**。这三种是真实事故场景。
//   3. **发出去的顺序与字节必须是协议规定的那一串**。抓包对不上就出不了光，
//      而现场往往只有"没出光"这一个现象。
//   4. **失败时也必须收光停机**，且不把半套参数写进 EEPROM。
//
// 全程不碰真实串口、不加载 SDK：FakeLink 顶替 transport::Link。
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "libs/business/app/runtime.hpp"
#include "libs/business/services/laser_fire.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/laser.hpp"

using namespace business;
using libs::ErrorKind;
using libs::Bytes;

namespace {

// 假串口：记录发出去的每一帧，并按预置队列回包。
class FakeLink final : public libs::transport::Link {
public:
    std::vector<Bytes> sent;
    std::deque<libs::Result<libs::transport::Reply>> replies;
    bool fail_writes = false;

    libs::Result<libs::transport::Reply> exchange(const Bytes& request, unsigned timeout_ms = 650,
                                                 unsigned idle_ms = 80) override {
        (void)timeout_ms;
        (void)idle_ms;
        sent.push_back(request);
        if (fail_writes) {
            return libs::Error::make(ErrorKind::transport, "cable_unplugged", "fake write failure");
        }
        // 只有读请求才消费预置回包。写指令按协议原数回显，不该"吃掉"下一条 0xB2。
        const bool is_read = request.size() >= 5 &&
                             (request[4] == libs::laser::command::kLdSettings ||
                              request[4] == libs::laser::command::kLdActual ||
                              request[4] == libs::laser::command::kTec);
        if (is_read && !replies.empty()) {
            auto r = replies.front();
            replies.pop_front();
            return r;
        }
        libs::transport::Reply reply;
        reply.bytes = request;          // 协议要求原数发回
        reply.elapsed_ms = 1;
        return libs::Result<libs::transport::Reply>::success(reply);
    }

    std::string endpoint() const override { return "FAKE12"; }

    void queue(const Bytes& bytes) {
        libs::transport::Reply r;
        r.bytes = bytes;
        r.elapsed_ms = 2;
        replies.push_back(libs::Result<libs::transport::Reply>::success(r));
    }

    // 排一次 0xB2 实际状态读。极性按协议 §3.5：0=安全，1=危险。
    void queue_actual(unsigned counter, bool work_state, bool q_gate_open,
                      bool interlock_ok = true, bool estop_ok = true, bool ot_ok = true,
                      std::uint8_t fault_bits = 0) {
        Bytes d(22, 0);
        d[0]  = work_state ? 1 : 0;
        d[1]  = 0;
        d[2]  = fault_bits;
        d[3]  = interlock_ok ? 0 : 1;
        d[4]  = interlock_ok ? 0 : 1;
        d[5]  = estop_ok ? 0 : 1;
        d[6]  = ot_ok ? 0 : 1;
        // 4 字节大端（协议 §3.5，高位在前）。曾用错字节序，
        // 恰好与 16 位截断的 bug 相互抵消，害得截断一直没被发现。
        d[13] = static_cast<std::uint8_t>((counter >> 24) & 0xFF);
        d[14] = static_cast<std::uint8_t>((counter >> 16) & 0xFF);
        d[15] = static_cast<std::uint8_t>((counter >> 8) & 0xFF);
        d[16] = static_cast<std::uint8_t>(counter & 0xFF);
        d[19] = q_gate_open ? 1 : 0;
        d[20] = work_state ? 1 : 0;
        d[21] = 0;
        queue(libs::laser::make_frame(libs::laser::command::kLdActual, d));
    }

    // 写指令码序列（按发送顺序）。只统计**写帧**。
    //
    // 判据是"指令码是不是读指令"，不能看帧长：0x34 单次触发和 0x31 保存参数
    // 本来就没有载荷，帧长与读请求一样是 4 字节；按长度过滤会漏掉出光最关键
    // 的那两帧。而漏算读请求同样是错的——"门禁拦截时零写帧"是本测试最硬的断言。
    std::vector<std::uint8_t> write_commands() const {
        std::vector<std::uint8_t> out;
        for (const Bytes& f : sent) {
            if (f.size() < 5 || f[0] != 0x5A || f[1] != 0xA5) continue;
            if (f[4] == libs::laser::command::kLdSettings ||
                f[4] == libs::laser::command::kLdActual ||
                f[4] == libs::laser::command::kTec) continue;
            out.push_back(f[4]);
        }
        return out;
    }
};

const config::SiteConfig& template_config() {
    static const config::SiteConfig value = config::default_template().value();
    return value;
}

// 纯软件 Runtime + 注入的假激光器。
struct Fixture {
    app::Runtime runtime;
    FakeLink link;
    std::unique_ptr<libs::devices::Laser> laser;
    std::string data_dir;
    config::SiteConfig site;

    explicit Fixture(const std::string& tag) {
        data_dir = std::string("build/tmp_fire_") + tag;
        site = config::default_template().value();
        // 已标定的出光参数。时间参数压到最小，避免测试变慢。
        site.laser_fire.commissioned = true;
        site.laser_fire.discharge_current_a = 80;
        site.laser_fire.pulse_width_us = 200;
        site.laser_fire.frequency_hz = 1;
        site.laser_fire.discharge_delay_us = 0;
        site.laser_fire.q_delay_us = 210;
        site.laser_fire.q_warmup_s = 0;
        site.laser_fire.settle_timeout_ms = 600;
        site.laser_fire.emission_hold_ms = 1;
        site.laser_fire.min_discharges = 1;

        app::Runtime::Options options;
        options.open_devices = false;      // 不碰真实串口
        options.data_dir = data_dir;
        (void)runtime.start(site, options);
        laser = std::make_unique<libs::devices::Laser>(link);
        runtime.laser_fire().set_laser_for_test(laser.get());
        // 门禁的静态条件：设备使能 + 出光参数已标定 + 参数齐备 + 空调联锁放行。
        // 剩下的安全位由每次 B2 读决定，这里不放宽。
        runtime.gate().set_laser_enabled(true);
        runtime.gate().set_laser_commissioned(true);
        runtime.gate().set_parameters_ready(true);
        runtime.gate().set_air_conditioner_interlock(true);
    }

    // 排好一次成功流程需要的全部 B2 读，顺序与 fire_and_capture 的读点一致：
    //   1 门禁轮询  2 基线  3 等放电(还在自适应)  4 等放电(进入放电)  5 事后确认
    // 第 3 次是刻意排的"还没进放电"：新流程必须真的等到状态位变化，
    // 而不是像旧实现那样在工作使能后固定 sleep 一小段就开光闸。
    void queue_happy_path(unsigned counter_before = 100, unsigned counter_after = 101) {
        link.queue_actual(counter_before, /*work=*/false, /*q_gate=*/false);  // 门禁轮询
        link.queue_actual(counter_before, /*work=*/false, /*q_gate=*/false);  // 基线
        link.queue_actual(counter_before, /*work=*/false, /*q_gate=*/false);  // 等放电：自适应中
        link.queue_actual(counter_before, /*work=*/true,  /*q_gate=*/false);  // 等放电：进入放电
        link.queue_actual(counter_after,  /*work=*/false, /*q_gate=*/false);  // 事后确认
    }
};

}  // namespace

TEST_MAIN_BEGIN()

// ── 1. 设备没开：一条指令都不发 ──────────────────────────────────────────────
CASE(fire_without_open_laser_sends_nothing); {
    app::Runtime runtime;
    app::Runtime::Options options;
    options.open_devices = false;
    options.data_dir = "build/tmp_fire_nodev";
    (void)runtime.start(template_config(), options);
    // 不注入设备 -> laser() 返回 nullptr
    const libs::Result<services::FireRecord> r = runtime.laser_fire().fire_and_capture();
    CHECK(!r);
    if (!r) CHECK(r.error().code == "laser_not_open");
}

// ── 2. 门禁不放行：一条写指令都不发 ──────────────────────────────────────────
// 逐个断开安全位，每种都必须拦得住，且**写帧数为 0**。
CASE(fire_blocked_by_every_unsafe_fact_sends_zero_writes); {
    using Arrange = void (*)(FakeLink&);
    struct Probe { const char* name; Arrange arrange; };
    // 门禁轮询读一次即返回对应的不安全状态。
    const Probe probes[] = {
        {"interlock_open", [](FakeLink& l){ l.queue_actual(10,false,false,/*interlock_ok=*/false); }},
        {"emergency_stop", [](FakeLink& l){ l.queue_actual(10,false,false,true,/*estop_ok=*/false); }},
        {"overtemperature", [](FakeLink& l){ l.queue_actual(10,false,false,true,true,/*ot_ok=*/false); }},
        {"laser_fault", [](FakeLink& l){ l.queue_actual(10,false,false,true,true,true,/*fault=*/0x04); }},
        {"q_gate_open", [](FakeLink& l){ l.queue_actual(10,false,/*q_gate_open=*/true); }},
    };
    for (std::size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); ++i) {
        CASE(fire_unsafe_probe); { (void)i; }
        Fixture f("unsafe_" + std::to_string(i));
        probes[i].arrange(f.link);

        const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
        CHECK(!r);
        // 关键：写指令一帧都不许发出去。
        CHECK_EQ(f.link.write_commands().size(), size_t(0));
    }
}

CASE(fire_blocked_when_parameters_incomplete); {
    Fixture f("param_incomplete");
    f.runtime.gate().set_parameters_ready(false);
    f.link.queue_actual(10, false, false);   // 门禁轮询：安全位都正常
    const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
    CHECK(!r);
    if (!r) CHECK(r.error().code == "gate_blocked");
    CHECK_EQ(f.link.write_commands().size(), size_t(0));
}

// ── 3. 协议极性：0=安全，1=危险（这是本次修复的缺陷，单独钉死）──────────────
CASE(parse_actual_frame_polarity_is_zero_means_safe); {
    Bytes d(22, 0);
    d[3] = 1;   // 联锁1 断开
    d[4] = 1;   // 联锁2 断开
    d[5] = 1;   // 急停
    d[6] = 1;   // 过温
    d[19] = 1;  // 光闸开
    const libs::laser::Frame frame = libs::laser::decode_frame(
        libs::laser::make_frame(libs::laser::command::kLdActual, d),
        libs::laser::command::kLdActual).value();
    const policy::LaserSafetyFacts facts = policy::parse_actual_frame(frame);
    CHECK(!facts.interlock_1);
    CHECK(!facts.interlock_2);
    CHECK(!facts.emergency_stop);
    CHECK(!facts.over_temperature);
    CHECK(!facts.q_gate);
}

CASE(parse_actual_frame_all_zeros_is_safe); {
    Bytes d(22, 0);
    const libs::laser::Frame frame = libs::laser::decode_frame(
        libs::laser::make_frame(libs::laser::command::kLdActual, d),
        libs::laser::command::kLdActual).value();
    const policy::LaserSafetyFacts facts = policy::parse_actual_frame(frame);
    CHECK(facts.available);
    CHECK(facts.interlock_1);
    CHECK(facts.interlock_2);
    CHECK(facts.emergency_stop);
    CHECK(facts.over_temperature);
    CHECK(facts.q_gate);
    CHECK(!facts.has_fault());
}

// ── 4. 完整出光：连续出光时序，且绝不碰机器配置 ──────────────────────────────
CASE(fire_sends_the_configured_session_parameters); {
    Fixture f("sequence");
    f.queue_happy_path(/*counter_before=*/100, /*counter_after=*/101);

    const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
    CHECK(r);
    if (r) {
    const std::vector<std::uint8_t> cmds = f.link.write_commands();

    // 4.1 会话参数按配置下发（高位在前）。值来自 site.laser_fire，
    //     不再是硬编码的"协议量程下限"——量程下限对限制类参数意味着禁止工作。
    struct Expect { std::uint8_t cmd; unsigned value; const char* name; };
    const Expect expects[] = {
        {libs::laser::command::kDischargeCurrent,    80,  "0x11 电流 80A（配置值）"},
        {libs::laser::command::kDischargePulseWidth, 200, "0x12 脉宽 200us（配置值）"},
        {libs::laser::command::kDischargeFrequency,  1,   "0x13 频率 1Hz（配置值）"},
        {libs::laser::command::kDischargeDelay,      0,   "0x14 放电延时 0us"},
        {libs::laser::command::kQDelay,              210, "0x16 Q延时 210us（配置值）"},
        {libs::laser::command::kQWarmup,             0,   "0x19 Q预热 0s"},
    };
    for (const Expect& e : expects) {
        CASE(fire_check_param_bytes); { (void)e; }
        bool found = false;
        for (const Bytes& f : f.link.sent) {
            if (f.size() >= 9 && f[0] == 0x5A && f[1] == 0xA5 && f[4] == e.cmd) {
                CHECK_EQ(static_cast<unsigned>(f[5]), (e.value >> 8) & 0xFF);
                CHECK_EQ(static_cast<unsigned>(f[6]), e.value & 0xFF);
                found = true;
            }
        }
        CHECK(found);
    }

    // 4.2 **机器配置一个字节都不许写。**
    //     0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护是整机标定值，存在设备
    //     EEPROM 里被所有工具共享（厂家工具从不写它们）。现场真实事故：
    //     本项目把 0x17 写成 0V，整机不出光，连厂家工具都点不着了。
    //     0x31 保存 EEPROM 同样禁止：每打一发就固化一次参数，
    //     任何一个写列表错误都会被永久写进设备。
    struct Forbidden { std::uint8_t cmd; const char* name; };
    const Forbidden forbidden[] = {
        {libs::laser::command::kVoltageLimit,       "0x17 电压限值"},
        {libs::laser::command::kQDivider,           "0x18 Q 分频"},
        {libs::laser::command::kLdTemperatureLimit, "0x15 温度保护"},
        {libs::laser::command::kSaveToEeprom,       "0x31 保存 EEPROM"},
    };
    for (const Forbidden& x : forbidden) {
        CASE(fire_check_forbidden_bytes); { (void)x; }
        bool appeared = false;
        for (const Bytes& f : f.link.sent) {
            if (f.size() >= 5 && f[0] == 0x5A && f[1] == 0xA5 && f[4] == x.cmd) appeared = true;
        }
        CHECK(!appeared);
    }

    // 4.3 **不再使用 0x34 单次触发**。协议文本把它写成"单次触发"，但厂家工具
    //     整个日志历史里一次都没用过；本项目照协议文本实现的版本真机零出光。
    int trig_count = 0;
    for (std::uint8_t c : cmds) if (c == libs::laser::command::kSingleTrigger) ++trig_count;
    CHECK_EQ(trig_count, 0);

    // 4.4 连续出光时序：工作=1 -> 光闸=1 -> 光闸=0 -> 工作=0
    int gate_on = -1, gate_off = -1, work_on = -1, work_off = -1;
    for (std::size_t i = 0; i < f.link.sent.size(); ++i) {
        const Bytes& fr = f.link.sent[i];
        if (fr.size() < 7 || fr[0] != 0x5A || fr[1] != 0xA5) continue;
        if (fr[4] == libs::laser::command::kWorkState) {
            if (fr[5] == 0x01 && work_on < 0) work_on = static_cast<int>(i);
            if (fr[5] == 0x00 && work_off < 0) work_off = static_cast<int>(i);
        }
        if (fr[4] == libs::laser::command::kQGate) {
            if (fr[5] == 0x01 && gate_on < 0) gate_on = static_cast<int>(i);
            if (fr[5] == 0x00 && gate_off < 0) gate_off = static_cast<int>(i);
        }
    }
    CHECK(work_on >= 0);
    CHECK(gate_on >= 0);
    CHECK(gate_off >= 0);
    CHECK(work_off >= 0);
    CHECK(work_on < gate_on);    // 先使能，等设备真的放电后才开光闸
    CHECK(gate_on < gate_off);   // 出光窗口
    CHECK(gate_off < work_off);  // 先收光再停机

    // 4.5 光闸与工作状态各只写两次（开一次、关一次），不留半开状态
    int gate_writes = 0, work_writes = 0;
    for (std::uint8_t c : cmds) {
        if (c == libs::laser::command::kQGate) ++gate_writes;
        if (c == libs::laser::command::kWorkState) ++work_writes;
    }
    CHECK_EQ(gate_writes, 2);
    CHECK_EQ(work_writes, 2);

    // 4.6 出光证据是计数器增量，不是"我发过指令"
    CHECK_EQ(r.value().discharges, 1u);

    // 4.7 设备不是一使能就能出光：必须先看到「正在工作且放电」。
    //     queue_happy_path 刻意排了一次"还在自适应"的读，settle_ms 必须 > 0。
    CHECK(r.value().settle_ms > 0u);
    }
}

CASE(fire_confirms_discharge_counter_moved); {
    Fixture f("counter");
    f.queue_happy_path(100, 101);
    const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
    CHECK(r);
    if (r) {
        CHECK_EQ(r.value().discharge_counter_before, 100u);
        CHECK_EQ(r.value().discharge_counter_after, 101u);
    }
}

// 回归（现场事故）：一串写指令全部成功，但激光器根本没放电，
// 软件却回"出光已执行并已保存参数"。放电计数器是唯一的事后证据，
// 没涨就必须判失败——否则"发了"被当成"成了"，操作员只能靠眼睛发现。
CASE(fire_fails_when_discharge_counter_does_not_move); {
    Fixture f("counter_stuck");
    f.queue_happy_path(100, 100);          // 出光前后计数器完全相同
    const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
    CHECK(!r);
    if (!r) CHECK(r.error().code == "no_discharge_detected");

    // 收光与停机仍然必须执行过，且没放电就不该把参数写进 EEPROM。
    int gate_off = 0, work_off = 0;
    bool saved = false;
    for (const Bytes& fr : f.link.sent) {
        if (fr.size() < 7 || fr[0] != 0x5A || fr[1] != 0xA5) continue;
        if (fr[4] == libs::laser::command::kQGate && fr[5] == 0x00) ++gate_off;
        if (fr[4] == libs::laser::command::kWorkState && fr[5] == 0x00) ++work_off;
        if (fr[4] == libs::laser::command::kSaveToEeprom) saved = true;
    }
    CHECK(gate_off >= 1);
    CHECK(work_off >= 1);
    CHECK(!saved);
}

// ── 5. 设备一直没进放电：必须判失败，且仍然停机 ──────────────────────────────
//
// 设备使能后会先停在 02「自适应中」。如果它一直不转到 01「正在工作且放电」，
// 说明这一发根本没打成 —— 必须判失败，且**绝不能打开光闸**。
CASE(fire_fails_and_cleans_up_when_device_never_reaches_discharge); {
    Fixture f("never_discharge");
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);   // 门禁轮询
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);   // 基线
    // 之后每次轮询都回"还在自适应"（work_state=0），直到超时。
    for (int i = 0; i < 12; ++i) {
        f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);
    }

    const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
    CHECK(!r);
    if (!r) CHECK(r.error().code == "laser_not_discharging");

    int gate_on = 0, gate_off = 0, work_on = 0, work_off = 0;
    bool saved = false;
    for (const Bytes& fr : f.link.sent) {
        if (fr.size() < 7 || fr[0] != 0x5A || fr[1] != 0xA5) continue;
        if (fr[4] == libs::laser::command::kQGate) {
            if (fr[5] == 0x01) ++gate_on; else ++gate_off;
        }
        if (fr[4] == libs::laser::command::kWorkState) {
            if (fr[5] == 0x01) ++work_on; else ++work_off;
        }
        if (fr[4] == libs::laser::command::kSaveToEeprom) saved = true;
    }
    CHECK(work_on >= 1);     // 工作使能确实发过
    CHECK_EQ(gate_on, 0);    // **设备没放电，光闸一次都不许开**
    CHECK(gate_off >= 1);    // 收光仍无条件执行
    CHECK(work_off >= 1);    // 停机仍无条件执行
    CHECK(!saved);           // 失败不得固化参数到 EEPROM
}

// ── 6. 未标定：一条写指令都不发 ──────────────────────────────────────────────
//
// 没有 commissioned=true 就不知道该发什么电流/脉宽/Q 延时。这些值过去被硬编码成
// "协议量程下限"，而量程下限对限制类参数意味着**禁止工作**（电压限值 0V、
// Q 延时 2us），现场后果是整机不出光，还把机器配置写坏。所以未标定一律拒绝。
CASE(fire_refuses_when_not_commissioned); {
    Fixture f("uncommissioned");
    f.runtime.gate().set_laser_commissioned(false);
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);   // 门禁轮询

    const libs::Result<services::FireRecord> r = f.runtime.laser_fire().fire_and_capture();
    CHECK(!r);
    if (!r) CHECK(r.error().code == "gate_blocked");
    CHECK_EQ(f.link.write_commands().size(), size_t(0));
}

// ── 6. 预检不写任何东西 ─────────────────────────────────────────────────────
CASE(precheck_sends_nothing_when_gate_blocks); {
    Fixture f("precheck");
    f.runtime.gate().set_parameters_ready(false);
    f.link.queue_actual(10, false, false);
    const libs::VoidResult r = f.runtime.laser_fire().precheck();
    CHECK(!r);
    CHECK_EQ(f.link.write_commands().size(), size_t(0));
}

// 回归：precheck() 曾经只 evaluate() 不 arm()，而 allowed = 拦截项为空 **且已布防**，
// 所以预检永远回"未布防"——安全位全正常也过不了，与真实出光时的判定不一致。
// 预检必须与 fire_and_capture() 走同一套：先布防再判定，且仍然一帧写指令都不发。
CASE(precheck_allows_when_every_safety_fact_is_safe); {
    Fixture f("precheck_ok");
    f.link.queue_actual(10, /*work=*/false, /*q_gate=*/false);   // 门禁轮询读一次
    const libs::VoidResult r = f.runtime.laser_fire().precheck();
    CHECK(r);                                    // 安全位全正常 -> 必须放行
    CHECK(f.runtime.gate().armed());             // 布防是判定的一部分
    CHECK_EQ(f.link.write_commands().size(), size_t(0));   // 但一条写指令都不发
}

// 回归：**界面上调的参数必须真的下发**。
//
// 曾经的形态是：界面的激光器参数写进业务参数库（param_set），而出光读的是
// 另一份站点配置（site.json 的 laser_fire）—— 操作员把电流改成 50 A，
// 门禁齐备性变了，可打出去还是 80 A。这条钉住"覆盖值真的进了串口帧"。
CASE(fire_uses_overridden_session_parameters); {
    Fixture f("overrides");
    f.queue_happy_path(100, 101);

    services::FireParameters params;
    params.laser = f.site.laser_fire;
    params.laser.discharge_current_a = 50;    // 覆盖标定值 80 A
    params.laser.q_delay_us = 100;            // 覆盖标定值 210 us
    params.integration_ms = 10.0;
    params.capture_timeout_ms = 10000;

    const libs::Result<services::FireRecord> r =
        f.runtime.laser_fire().fire_and_capture(params);
    CHECK(r);

    // 帧里必须是覆盖后的值：0x0032 = 50 A，0x0064 = 100 us（协议高位在前）。
    bool cur_ok = false, q_ok = false;
    for (const Bytes& fr : f.link.sent) {
        if (fr.size() < 7 || fr[0] != 0x5A || fr[1] != 0xA5) continue;
        if (fr.size() >= 9 && fr[4] == libs::laser::command::kDischargeCurrent) {
            cur_ok = (fr[5] == 0x00 && fr[6] == 0x32);
        }
        if (fr.size() >= 9 && fr[4] == libs::laser::command::kQDelay) {
            q_ok = (fr[5] == 0x00 && fr[6] == 0x64);
        }
    }
    CHECK(cur_ok);
    CHECK(q_ok);
}

// ── 7. 连续出光 ─────────────────────────────────────────────────────────────
//
// run_continuous 与 fire_and_capture 的**安全前置必须逐字相同**（门禁 → 基线
// → 会话参数 → 工作使能 → 等放电），区别只在最后：这里开到光闸就返回。
// 两条路径分开写的话迟早有一条会被漏掉，所以这里逐字节断言会话参数一致。
CASE(run_continuous_sends_the_same_session_parameters_and_leaves_the_gate_open); {
    Fixture f("run_cfg");
    // 队列：门禁轮询、基线、等放电(自适应中)、等放电(已放电)
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);
    f.link.queue_actual(100, /*work=*/true,  /*q_gate=*/false);

    services::FireParameters params;
    params.laser = f.site.laser_fire;

    const libs::Result<services::RunRecord> r =
        f.runtime.laser_fire().run_continuous(params);
    CHECK(r);
    if (r) CHECK(r.value().discharge_counter_at_start == 100u);

    // 会话参数六条都在，且值与配置一致（高位在前）。
    struct Expect { std::uint8_t cmd; unsigned value; };
    const Expect expects[] = {
        {libs::laser::command::kDischargeCurrent,    80},
        {libs::laser::command::kDischargePulseWidth, 200},
        {libs::laser::command::kDischargeFrequency,  1},
        {libs::laser::command::kDischargeDelay,      0},
        {libs::laser::command::kQDelay,              210},
        {libs::laser::command::kQWarmup,             0},
    };
    for (const Expect& e : expects) {
        bool found = false;
        for (const Bytes& fr : f.link.sent) {
            if (fr.size() >= 9 && fr[0] == 0x5A && fr[1] == 0xA5 && fr[4] == e.cmd) {
                found = (fr[5] == ((e.value >> 8) & 0xFF)) && (fr[6] == (e.value & 0xFF));
            }
        }
        CHECK(found);
    }

    // 光闸必须**停在开着** —— 连续出光就是要它一直打着。
    bool gate_open = false;
    for (const Bytes& fr : f.link.sent) {
        if (fr.size() >= 7 && fr[0] == 0x5A && fr[1] == 0xA5 &&
            fr[4] == libs::laser::command::kQGate && fr[5] == 0x01) {
            gate_open = true;
        }
    }
    CHECK(gate_open);

    // 机器配置与 EEPROM 在这条路径上同样一个字节都不许写。
    for (const Bytes& fr : f.link.sent) {
        if (fr.size() < 5 || fr[0] != 0x5A || fr[1] != 0xA5) continue;
        CHECK(fr[4] != libs::laser::command::kVoltageLimit);
        CHECK(fr[4] != libs::laser::command::kQDivider);
        CHECK(fr[4] != libs::laser::command::kLdTemperatureLimit);
        CHECK(fr[4] != libs::laser::command::kSaveToEeprom);
    }

    // 停止：光闸关 + 停机，且读回确认通过（回一帧"光闸关、已停机"）。
    f.link.queue_actual(100, /*work=*/false, /*q_gate=*/false);
    const libs::VoidResult stopped = f.runtime.laser_fire().stop_continuous();
    CHECK(stopped);
    int gate_off = 0, work_off = 0;
    for (const Bytes& fr : f.link.sent) {
        if (fr.size() < 7 || fr[0] != 0x5A || fr[1] != 0xA5) continue;
        if (fr[4] == libs::laser::command::kQGate && fr[5] == 0x00) ++gate_off;
        if (fr[4] == libs::laser::command::kWorkState && fr[5] == 0x00) ++work_off;
    }
    CHECK(gate_off >= 1);
    CHECK(work_off >= 1);
}

// 停止后读回如果仍显示光闸开着，**必须报失败**，不能因为"我发过 0x24=0"就
// 认为收光了。现场要靠这条把人叫回来人工确认。
CASE(stop_continuous_fails_when_readback_still_shows_the_gate_open); {
    Fixture f("stop_bad");
    // 读回：光闸还开着（q_gate_open=true → 安全位 q_gate=false）
    f.link.queue_actual(100, /*work=*/true, /*q_gate=*/true);
    const libs::VoidResult r = f.runtime.laser_fire().stop_continuous();
    CHECK(!r);
    if (!r) CHECK(r.error().code == "laser_stop_incomplete");
}

TEST_MAIN_RETURN()
