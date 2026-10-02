#include "libs/business/services/laser_fire.hpp"

#include <chrono>
#include <sstream>
#include <thread>

#include "libs/business/app/runtime.hpp"
#include "libs/business/services/capture.hpp"
#include "libs/business/services/spectrum_stream.hpp"
#include "libs/business/time_util.hpp"

namespace business::services {

namespace {

namespace cmd = libs::laser::command;

// 出光后等待光谱仪收尾的固定时间。未标定，用一个保守的固定值并如实标注。
constexpr unsigned kPostFireSettleMs = 200;

// 轮询"设备是否已经进入放电"的间隔。
// 设备使能后会先停在 02「自适应中」，厂家日志里要 5~6 秒才转到
// 01「正在工作且放电」。200 ms 的粒度足够，也不会把串口刷爆。
constexpr unsigned kSettlePollIntervalMs = 200;

// 出光参数不再有硬编码的"安全默认值"：
// 对限制类参数（电压限值、Q 延时），协议量程的下限意味着"禁止工作"而不是
// "最小能量"。过去照量程下限下发，现场后果是整机不出光。可用工作点只能来自
// 厂家标定或现场工艺，因此一律来自 config::LaserFireConfig，且未标定即拒绝。

libs::Error fail(FireRecord& record, const char* step, libs::ErrorKind kind,
                 std::string code, std::string message) {
    record.failed_step = step;
    record.error_code = code;
    record.error_message = message;
    return libs::Error::make(kind, std::move(code), std::move(message));
}

libs::Error fail_run(RunRecord& record, const char* step, libs::ErrorKind kind,
                     std::string code, std::string message) {
    record.failed_step = step;
    record.error_code = code;
    record.error_message = message;
    return libs::Error::make(kind, std::move(code), std::move(message));
}

}  // namespace

libs::VoidResult LaserFireService::step(libs::devices::Laser* laser,
                                        std::uint8_t command,
                                        const libs::Bytes& payload,
                                        FireRecord& record,
                                        const char* step_name) {
    const libs::Result<libs::devices::LaserTransaction> tx = laser->write(command, payload);
    if (!tx) {
        return libs::Error::make(libs::ErrorKind::device, "laser_step_failed",
                                 std::string(step_name) + " 失败：" + tx.error().message);
    }
    record.sent_frames.push_back(libs::to_hex(tx.value().request));
    return libs::VoidResult();
}

libs::Result<policy::LaserSafetyFacts> LaserFireService::read_facts(libs::devices::Laser* laser,
                                                                   FireRecord& record) {
    const libs::Result<libs::devices::LaserTransaction> tx = laser->read(cmd::kLdActual);
    if (!tx) {
        record.failed_step = "read_actual";
        record.error_code = tx.error().code;
        record.error_message = "读取实际状态 0xB2 失败：" + tx.error().message;
        return tx.error();
    }
    record.sent_frames.push_back(libs::to_hex(tx.value().request));

    // 极性换算与门禁轮询共用同一处实现（policy::parse_actual_frame），
    // 两处各写一遍必然有一处写反——这正是本次修复的缺陷。
    return policy::parse_actual_frame(tx.value().frame);
}

// 从一帧 0xB2 里取放电计数器。单独抽出来，因为它是 32 位字段，
// 漏取或取错一次，出光确认就会静默地变成"永远没出光"。
std::uint32_t discharge_counter_of(const libs::laser::Frame& frame) {
    for (const auto& field : frame.fields) {
        if (field.name == "discharge_counter") return field.wide ? field.raw32 : field.raw;
    }
    return 0;
}

libs::devices::Laser* LaserFireService::laser() const {
    return laser_override_ != nullptr ? laser_override_ : runtime_.devices().laser();
}

libs::VoidResult LaserFireService::precheck() {
    libs::devices::Laser* laser = this->laser();
    if (laser == nullptr) {
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open",
                                 "激光器未打开或未启用，无法出光");
    }
    const libs::VoidResult polled = runtime_.poll_laser_facts(laser);
    if (!polled) return polled.error();
    // 门禁的 allowed = blocked_by 为空 **且** 已布防。预检问的是
    // "真出光时会不会被放行"，所以必须和 fire_and_capture() 走同一套：
    // 先布防再判定。否则预检只会回一句"未布防"，与真实判定不一致，
    // UI 拿到的是一个永远为假的预检结果。布防本身不发任何指令。
    const libs::VoidResult armed = runtime_.gate().arm();
    if (!armed) return armed.error();
    const policy::GateVerdict verdict = runtime_.gate().evaluate();
    if (!verdict.allowed) {
        std::ostringstream os;
        os << "门禁未放行：" << verdict.detail;
        return libs::Error::make(libs::ErrorKind::device, "gate_blocked", os.str());
    }
    return libs::VoidResult();
}

libs::Result<FireRecord> LaserFireService::fire_and_capture() {
    // 不带覆盖：全部取站点配置，等价于"按已标定值出光"。
    FireParameters params;
    params.laser = runtime_.config().laser_fire;
    params.laser_clock_mode = static_cast<std::uint8_t>(params.laser.laser_clock_mode);
    params.integration_ms = static_cast<double>(runtime_.config().integration_ms);
    params.capture_timeout_ms = runtime_.config().capture_timeout_ms;
    return fire_and_capture(params);
}

libs::Result<FireRecord> LaserFireService::fire_and_capture(const FireParameters& params) {
    FireRecord record;
    const bool spectrometer_fires = params.spectrometer_triggers_laser;
    record.trigger_direction = spectrometer_fires ? "spectrometer_to_laser"
                                                  : "laser_to_spectrometer";

    // ── 1. 设备在位 ──────────────────────────────────────────────────────────
    libs::devices::Laser* laser = this->laser();
    if (laser == nullptr) {
        return fail(record, "laser_open", libs::ErrorKind::unsupported,
                    "laser_not_open", "激光器未打开或未启用，无法出光");
    }

    // ── 2. 门禁：唯一判定入口，且必须先有事实 ────────────────────────────────
    // poll_laser_facts 读不到 B2 时会把 available=false 喂进门禁，
    // 门禁必然拦截——不会出现"没有事实却放行"。
    const libs::VoidResult polled = runtime_.poll_laser_facts(laser);
    const libs::VoidResult armed = runtime_.gate().arm();
    const policy::GateVerdict verdict = runtime_.gate().evaluate();
    if (!verdict.allowed) {
        std::string why = verdict.detail;
        if (!polled) why += "（读激光状态也失败了：" + polled.error().message + "）";
        return fail(record, "gate", libs::ErrorKind::device, "gate_blocked",
                    "门禁未放行，**未发出任何写指令**：" + why);
    }
    if (!armed) {
        return fail(record, "gate_arm", armed.error().kind, armed.error().code,
                    "门禁布防失败：" + armed.error().message);
    }

    // ── 3. 基线：记录放电计数器起点 ───────────────────────────────────────────
    const auto baseline_tx = laser->read(cmd::kLdActual);
    if (!baseline_tx) {
        record.failed_step = "read_actual_baseline";
        record.error_code = baseline_tx.error().code;
        record.error_message = "读取出光前状态 0xB2 失败：" + baseline_tx.error().message;
        return baseline_tx.error();
    }
    record.sent_frames.push_back(libs::to_hex(baseline_tx.value().request));
    const policy::LaserSafetyFacts before = policy::parse_actual_frame(baseline_tx.value().frame);
    record.discharge_counter_before = discharge_counter_of(baseline_tx.value().frame);
    if (before.has_fault()) {
        return fail(record, "baseline_fault", libs::ErrorKind::device, "laser_fault",
                    "出光前读到的故障位非零，拒绝出光");
    }

    // ── 4. 光谱仪先进入等待状态（后台线程）──────────────────────────────────
    //
    // 方向：**激光器触发光谱仪**（AvaSoft 手册 §4.9 接法二）。
    // 这里显式强制 wait_for_laser_trigger = true —— 出光流程里激光器就是唯一
    // 触发源，这是物理事实，不依赖站点配置里那个开关填没填。
    // 光谱仪从 pin 6 等激光器 TTL，收到后延迟 integration_delay_ns 再积分。
    //
    // 两个线程操作的是不同设备（USB 光谱仪 vs 串口激光器），不争 device_mutex。
    //
    // **arm 的时机：等设备确认进入放电之后、开光闸之前**（见 5d）。
    // 不能一开始就 arm：设备从工作使能到真正放电要 5~6 秒，而光谱仪在
    // pin 6 上等 TTL 是有超时的（capture_timeout_ms）。早 arm 会让采集超时
    // 在激光还没出光时就到期，现场表现为 capture_timeout —— 而激光其实
    // 打得好好的。这个坑真机上踩过一次。
    libs::devices::Spectrometer* spectrometer = runtime_.devices().spectrometer();
    CapturePace pace = CaptureCoordinator::pace_from_config(runtime_.config());
    // **界面上的数字就是本次要用的数字**：积分时间与等触发超时以本次请求为准。
    // 不这么接的话，界面里那两个框就是骗人的（能调、不生效）。
    pace.integration_ms = params.integration_ms;
    pace.timeout_ms = params.capture_timeout_ms;
    const double delay_ns = runtime_.config().spectrometer.integration_delay_ns;

    // 采集租约：必须在 arm 之前取，否则实时预览可能先吃掉那个 TTL。
    // 理由见 spectrum_stream.hpp 文件头"第四条"：外触发模式下每条采集路径
    // 都会去等 pin 6，谁先到谁拿走触发，测量那次就永远等不到。
    // 租约在函数返回时（析构）自动释放，含失败与提前 return 路径。
    services::AcquisitionLease lease(runtime_.spectrum_stream());
    libs::Result<libs::devices::CaptureResult> captured =
        libs::Error::make(libs::ErrorKind::unsupported, "capture_not_started", "");
    std::thread capture_thread;

    // ── 5. 出光序列（连续出光，不用 0x34）────────────────────────────────────
    //
    // 时序依据是厂家工具的实测日志，不是协议文本：协议把 0x34 写成"单次触发"，
    // 但厂家工具整个日志历史里从没用过它，跑的是
    //   工作=1 -> 等设备进入「正在工作且放电」-> 光闸=1 -> 保持 -> 光闸=0 -> 停机。
    const config::LaserFireConfig& cfg = params.laser;
    libs::Error fire_error{};

    // 反向路径必须先把激光器切到外部时钟模式，否则 Pin 23 的 TTL
    // 只会在光谱仪端有波形，激光器仍按内部时钟工作，表现为“采到背景谱”。
    // 0x22 不是 EEPROM 保存动作；仅在本次会话下发。正向会话写 0 恢复
    // 内部时钟，避免前一发反向会话的外部时钟设置残留。
    std::uint32_t laser_trigger_delay_cycles = 0;
    std::uint32_t laser_trigger_width_cycles = 0;
    if (spectrometer_fires) {
        if (spectrometer == nullptr) {
            fire_error = libs::Error::make(libs::ErrorKind::unsupported,
                                           "spectrometer_not_open",
                                           "反向触发需要已打开的光谱仪，无法输出 Pin 23 TTL");
            record.failed_step = "spectrometer_open";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
        if (runtime_.config().spectrometer.master_serial.empty()) {
            fire_error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                           "spectrometer_master_required",
                                           "反向触发需要在 spectrometer.master_serial 中明确指定主通道");
            record.failed_step = "spectrometer_master";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
        if (params.laser_clock_mode != 1 && params.laser_clock_mode != 2) {
            fire_error = libs::Error::make(libs::ErrorKind::invalid_argument,
                                           "bad_laser_clock_mode",
                                           "反向触发要求 laser_clock_mode 为 1 或 2");
            record.failed_step = "set_clock_mode";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        } else {
            const double width_ns = params.laser_trigger_width_ns > 0.0
                                        ? params.laser_trigger_width_ns
                                        : static_cast<double>(cfg.pulse_width_us) * 1000.0;
            if (!libs::devices::FpgaTime::laser_nanos_to_cycles(
                    params.laser_trigger_delay_ns, &laser_trigger_delay_cycles) ||
                !libs::devices::FpgaTime::laser_nanos_to_cycles(
                    width_ns, &laser_trigger_width_cycles) ||
                laser_trigger_width_cycles == 0) {
                fire_error = libs::Error::make(
                    libs::ErrorKind::invalid_argument, "bad_laser_output_timing",
                    "反向触发的 Pin 23 延时/脉宽无法换算为有效 FPGA 计数");
                record.failed_step = "set_laser_output_timing";
                record.error_code = fire_error.code;
                record.error_message = fire_error.message;
            }
        }
    }

    // 只写**会话参数**（每次检测可调的部分）。
    // 机器配置 0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护一律不碰：
    // 它们是整机标定值，存在设备 EEPROM 里被所有工具共享，厂家工具从不写。
    struct Param { std::uint8_t command; unsigned value; const char* name; };
    const Param session_writes[] = {
        {cmd::kDischargeCurrent,    cfg.discharge_current_a, "set_discharge_current"},
        {cmd::kDischargePulseWidth, cfg.pulse_width_us,      "set_pulse_width"},
        {cmd::kDischargeFrequency,  cfg.frequency_hz,        "set_frequency"},
        {cmd::kDischargeDelay,      cfg.discharge_delay_us,  "set_discharge_delay"},
        {cmd::kQDelay,              cfg.q_delay_us,          "set_q_delay"},
        {cmd::kQWarmup,             cfg.q_warmup_s,          "set_q_warmup"},
    };

    // 5a. 写会话参数（协议 §3.1，高位在前）
    if (fire_error.ok()) {
        for (const Param& p : session_writes) {
            const libs::Bytes payload{static_cast<std::uint8_t>((p.value >> 8) & 0xFF),
                                      static_cast<std::uint8_t>(p.value & 0xFF)};
            const libs::VoidResult r = step(laser, p.command, payload, record, p.name);
            if (!r) { fire_error = r.error(); break; }
        }
    }

    // 5a.1 明确设置本次会话的时钟来源。反向模式必须使用外部时钟；
    // 正向模式也要恢复内部时钟，避免上一次反向会话把设备留在外时钟状态。
    if (fire_error.ok()) {
        const libs::VoidResult r = step(laser, cmd::kClockMode,
                                        {static_cast<std::uint8_t>(spectrometer_fires
                                                                        ? params.laser_clock_mode
                                                                        : 0)},
                                        record, spectrometer_fires ? "set_external_clock_mode"
                                                                    : "set_internal_clock_mode");
        if (!r) fire_error = r.error();
    }

    // 5b. 工作状态 = 1（0x21）
    if (fire_error.ok()) {
        const libs::VoidResult r = step(laser, cmd::kWorkState, {0x01}, record, "set_work_state_1");
        if (!r) fire_error = r.error();
    }

    // 5c. 等设备**真的进入放电**。
    //
    // 工作使能后设备先停在 02「自适应中」，厂家日志里要 5~6 秒才转到
    // 01「正在工作且放电」。固定 sleep 解决不了：太短则触发落空
    // （本项目原先只等 50 ms），太长则白等。所以轮询状态位，看到放电或超时。
    if (fire_error.ok() && !spectrometer_fires) {
        const auto settle_start = std::chrono::steady_clock::now();
        const auto deadline = settle_start + std::chrono::milliseconds(cfg.settle_timeout_ms);
        bool discharging = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto tx = laser->read(cmd::kLdActual);
            if (!tx) {
                fire_error = libs::Error::make(libs::ErrorKind::device, "settle_read_failed",
                                               "等待放电期间读 0xB2 失败：" + tx.error().message);
                record.failed_step = "settle_read";
                record.error_code = fire_error.code;
                record.error_message = fire_error.message;
                break;
            }
            record.sent_frames.push_back(libs::to_hex(tx.value().request));
            const policy::LaserSafetyFacts f = policy::parse_actual_frame(tx.value().frame);
            if (f.has_fault()) {
                fire_error = libs::Error::make(libs::ErrorKind::device, "laser_fault_during_settle",
                                               "等待放电期间激光器报了故障位");
                record.failed_step = "settle_fault";
                record.error_code = fire_error.code;
                record.error_message = fire_error.message;
                break;
            }
            if (f.work_state) { discharging = true; break; }   // raw==1 = 正在工作且放电
            std::this_thread::sleep_for(std::chrono::milliseconds(kSettlePollIntervalMs));
        }
        record.settle_ms = static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - settle_start).count());
        if (fire_error.ok() && !discharging) {
            fire_error = libs::Error::make(
                libs::ErrorKind::device, "laser_not_discharging",
                "工作使能后设备在 " + std::to_string(cfg.settle_timeout_ms) +
                " ms 内没有进入「正在工作且放电」（一直停在自适应状态），拒绝开光闸");
            record.failed_step = "settle";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
    }

    // 5d. 按方向布防触发链。
    //
    // 放在这里而不是流程开头：设备此刻已确认在放电，光闸马上要开，
    // 采集超时才只需要覆盖出光窗口本身，而不是把 5~6 秒的自适应时间也算进去。
    // 正向时光谱仪等 Pin 6 外部脉冲；反向时由主通道 Pin 23 发脉冲。
    if (fire_error.ok() && spectrometer_fires) {
        // 反向时先打开 Q 门，再让 AVS_Measure 发 Pin 23；不能等待 Pin 6。
        const libs::VoidResult r = step(laser, cmd::kQGate, {0x01}, record,
                                        "set_q_gate_1_before_spectrometer_trigger");
        if (!r) fire_error = r.error();
        if (fire_error.ok()) {
            capture_thread = std::thread([&]() {
                captured = runtime_.capture().capture(
                    spectrometer, pace, false, delay_ns, true,
                    laser_trigger_delay_cycles, laser_trigger_width_cycles);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // 必须在收光前等 SDK 完成本次测量，避免 TTL 在线程退出后才到达。
            capture_thread.join();
            record.trigger_output_programmed = true;
        }
    } else if (fire_error.ok() && spectrometer != nullptr) {
        capture_thread = std::thread([&]() {
            captured = runtime_.capture().capture(spectrometer, pace, true, delay_ns);
        });
        // 给 SDK 一点时间把各通道 arm 到"在 pin 6 上等脉冲"的状态。
        // 未标定，取保守的 50 ms。
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 5e. 光闸 = 1（0x24）。设备此刻已在放电，打开光闸即开始连续出光。
    if (fire_error.ok() && !spectrometer_fires) {
        const libs::VoidResult r = step(laser, cmd::kQGate, {0x01}, record, "set_q_gate_1");
        if (!r) fire_error = r.error();
    }

    // 5f. 连续出光窗口：按配置保持，光谱仪在这个窗口里等 pin 6 的 TTL。
    if (fire_error.ok() && !spectrometer_fires) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.emission_hold_ms));
    }

    // 5g. 收光、停机 —— 无条件执行，优先于任何后续步骤与错误上报
    step(laser, cmd::kQGate, {0x00}, record, "clear_q_gate_0");
    step(laser, cmd::kWorkState, {0x00}, record, "clear_work_state_0");

    // ── 6. 等采集线程收尾 ────────────────────────────────────────────────────
    if (spectrometer != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPostFireSettleMs));
        if (capture_thread.joinable()) capture_thread.join();
    }

    // ── 7. 读回确认 ──────────────────────────────────────────────────────────
    const auto confirm_tx = laser->read(cmd::kLdActual);
    policy::LaserSafetyFacts after;
    if (confirm_tx) {
        record.sent_frames.push_back(libs::to_hex(confirm_tx.value().request));
        after = policy::parse_actual_frame(confirm_tx.value().frame);
        record.discharge_counter_after = discharge_counter_of(confirm_tx.value().frame);
    } else if (fire_error.ok()) {
        // 读回确认失败也要如实记下来：光闸/工作状态此时是"未确认"而不是"已关闭"，
        // 上层必须知道现场要人工看一眼。
        record.failed_step = "read_actual_confirm";
        record.error_code = confirm_tx.error().code;
        record.error_message = "出光后读取确认状态 0xB2 失败：" + confirm_tx.error().message;
        fire_error = confirm_tx.error();
    }
    if (confirm_tx) {
        record.confirmed_work_state = after.work_state;
        record.confirmed_q_gate_open = !after.q_gate;   // 读回 0=关=已收光
        if (fire_error.ok() && after.has_fault()) {
            fire_error = libs::Error::make(libs::ErrorKind::device, "laser_fault",
                                           "出光后激光器报了故障位");
            record.failed_step = "post_fire_fault";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
        // 出光是物理动作：一串指令都写成功，不等于激光真的放了电。
        // 唯一的事后证据是放电计数器，计数器没涨够就必须判失败。
        //
        // 现场真实发生过一次：软件回"出光已执行并已保存参数"，而计数器
        // 一个脉冲都没涨、操作员只能靠眼睛发现没出光。此前本函数只把
        // before/after 记进 record，从不比较——正是文件头第 3 条纪律
        // （"发了"不等于"成了"）被实现漏掉的那一半。
        if (record.discharge_counter_after > record.discharge_counter_before) {
            record.discharges = record.discharge_counter_after - record.discharge_counter_before;
        }
        if (fire_error.ok() && record.discharges < cfg.min_discharges) {
            fire_error = libs::Error::make(
                libs::ErrorKind::device, "no_discharge_detected",
                "出光序列已发完，但放电计数器只涨了 " + std::to_string(record.discharges) +
                " 次（要求至少 " + std::to_string(cfg.min_discharges) +
                " 次；前 " + std::to_string(record.discharge_counter_before) +
                "，后 " + std::to_string(record.discharge_counter_after) +
                "）：激光器没有真正放电。指令已发出 != 已经出光。");
            record.failed_step = "no_discharge";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
        // 收光与停机同样要**读回确认**，不能只看"我发过 0x24=0"。
        // q_gate 的语义是 true=光闸关（安全），所以这里判 false 才算没收光。
        if (fire_error.ok() && !after.q_gate) {
            fire_error = libs::Error::make(libs::ErrorKind::device, "q_gate_not_closed",
                                           "已下发收光，但读回状态仍显示光闸开着；"
                                           "请现场人工确认光闸已关闭");
            record.failed_step = "q_gate_not_closed";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
        if (fire_error.ok() && after.work_state) {
            fire_error = libs::Error::make(libs::ErrorKind::device, "work_state_not_cleared",
                                           "已下发停机，但读回状态仍显示正在工作；"
                                           "请现场人工确认泵浦已停止");
            record.failed_step = "work_state_not_cleared";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
    }

    // 注意：本流程**不做 0x31 保存 EEPROM**。每打一发就往激光器 EEPROM 写一次
    // 参数，任何一个写列表的错误都会被永久固化（现场已经因此把机器配置写坏过一次）。
    // 保存是独立的、由操作员显式发起的动作，不属于出光。

    if (!fire_error.ok()) {
        record.ok = false;
        if (record.failed_step.empty()) {
            record.failed_step = "fire_sequence";
            record.error_code = fire_error.code;
            record.error_message = fire_error.message;
        }
        return fire_error;
    }

    if (spectrometer == nullptr) {
        record.capture_attempted = false;
        record.note = "光谱仪未打开：本次只出光，未采集";
    } else {
        record.capture_attempted = true;
        if (!captured) {
            return fail(record, "capture", captured.error().kind, captured.error().code,
                        "出光已执行（放电 " + std::to_string(record.discharges) +
                        " 次），但采集失败：" + captured.error().message);
        }
        record.spectrum_groups = static_cast<unsigned>(captured.value().groups.size());
        record.spectra = captured.value().groups;   // 带出去交给交付出口落盘
        record.note = "出光、收光、停机与光谱采集均已完成（放电 " +
                      std::to_string(record.discharges) + " 次）";
    }

    record.ok = true;
    return record;
}

// ── 连续出光 ─────────────────────────────────────────────────────────────────
//
// 前置顺序与 fire_and_capture **逐字相同**（门禁 → 基线 → 会话参数 → 工作使能
// → 等放电），区别只在最后：这里开到光闸就返回，把"打到什么时候停"交给操作员。
// 两条路径的安全前置分开写的话，迟早有一条会被漏掉 —— 这个项目已经吃过一次。
libs::Result<RunRecord> LaserFireService::run_continuous(const FireParameters& params) {
    RunRecord record;
    const config::LaserFireConfig& cfg = params.laser;

    libs::devices::Laser* laser = this->laser();
    if (laser == nullptr) {
        return fail_run(record, "laser_open", libs::ErrorKind::unsupported,
                        "laser_not_open", "激光器未打开或未启用，无法出光");
    }

    // 门禁：唯一判定入口。不放行 = 一条写指令都不发。
    const libs::VoidResult polled = runtime_.poll_laser_facts(laser);
    const libs::VoidResult armed = runtime_.gate().arm();
    const policy::GateVerdict verdict = runtime_.gate().evaluate();
    if (!verdict.allowed) {
        std::string why = verdict.detail;
        if (!polled) why += "（读激光状态也失败了：" + polled.error().message + "）";
        return fail_run(record, "gate", libs::ErrorKind::device, "gate_blocked",
                        "门禁未放行，**未发出任何写指令**：" + why);
    }
    if (!armed) {
        return fail_run(record, "gate_arm", armed.error().kind, armed.error().code,
                        "门禁布防失败：" + armed.error().message);
    }

    // 基线：记下放电计数器起点（连续出光没有"事后确认"，起点只用于留证）。
    const auto baseline_tx = laser->read(cmd::kLdActual);
    if (!baseline_tx) {
        record.failed_step = "read_actual_baseline";
        record.error_code = baseline_tx.error().code;
        record.error_message = "读取状态 0xB2 失败：" + baseline_tx.error().message;
        return baseline_tx.error();
    }
    record.sent_frames.push_back(libs::to_hex(baseline_tx.value().request));
    const policy::LaserSafetyFacts before = policy::parse_actual_frame(baseline_tx.value().frame);
    record.discharge_counter_at_start = discharge_counter_of(baseline_tx.value().frame);
    if (before.has_fault()) {
        return fail_run(record, "baseline_fault", libs::ErrorKind::device, "laser_fault",
                        "出光前读到的故障位非零，拒绝出光");
    }

    // 会话参数。**与 fire_and_capture 同一张表、同一组值**。
    struct Param { std::uint8_t command; unsigned value; const char* name; };
    const Param session_writes[] = {
        {cmd::kDischargeCurrent,    cfg.discharge_current_a, "set_discharge_current"},
        {cmd::kDischargePulseWidth, cfg.pulse_width_us,      "set_pulse_width"},
        {cmd::kDischargeFrequency,  cfg.frequency_hz,        "set_frequency"},
        {cmd::kDischargeDelay,      cfg.discharge_delay_us,  "set_discharge_delay"},
        {cmd::kQDelay,              cfg.q_delay_us,          "set_q_delay"},
        {cmd::kQWarmup,             cfg.q_warmup_s,          "set_q_warmup"},
    };

    libs::Error run_error{};
    const auto record_step = [&](std::uint8_t command, const libs::Bytes& payload,
                                 const char* name) {
        const libs::Result<libs::devices::LaserTransaction> tx = laser->write(command, payload);
        if (!tx) {
            run_error = libs::Error::make(libs::ErrorKind::device, "laser_step_failed",
                                          std::string(name) + " 失败：" + tx.error().message);
            return false;
        }
        record.sent_frames.push_back(libs::to_hex(tx.value().request));
        return true;
    };

    for (const Param& p : session_writes) {
        const libs::Bytes payload{static_cast<std::uint8_t>((p.value >> 8) & 0xFF),
                                  static_cast<std::uint8_t>(p.value & 0xFF)};
        if (!record_step(p.command, payload, p.name)) {
            record.failed_step = "set_session_parameters";
            record.error_code = run_error.code;
            record.error_message = run_error.message;
            return run_error;
        }
    }

    // 明确恢复内部时钟，避免上一次反向会话留下外部时钟模式。
    if (!record_step(cmd::kClockMode, {0x00}, "set_internal_clock_mode")) {
        record.failed_step = "set_internal_clock_mode";
        record.error_code = run_error.code;
        record.error_message = run_error.message;
        return run_error;
    }

    // 工作 = 1
    if (!record_step(cmd::kWorkState, {0x01}, "set_work_state_1")) {
        record.failed_step = "set_work_state_1";
        record.error_code = run_error.code;
        record.error_message = run_error.message;
        return run_error;
    }

    // 等设备**真的进入放电**（自适应约 5~6 秒，不是常数）。
    const auto settle_start = std::chrono::steady_clock::now();
    const auto deadline = settle_start + std::chrono::milliseconds(cfg.settle_timeout_ms);
    bool discharging = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto tx = laser->read(cmd::kLdActual);
        if (!tx) {
            record.failed_step = "settle_read";
            record.error_code = tx.error().code;
            record.error_message = "等待放电期间读 0xB2 失败：" + tx.error().message;
            (void)stop_continuous();   // 已经使能过，别把设备撂在"工作中"
            return tx.error();
        }
        record.sent_frames.push_back(libs::to_hex(tx.value().request));
        const policy::LaserSafetyFacts f = policy::parse_actual_frame(tx.value().frame);
        if (f.has_fault()) {
            (void)stop_continuous();
            return fail_run(record, "settle_fault", libs::ErrorKind::device, "laser_fault",
                            "等待放电期间激光器报了故障位");
        }
        if (f.work_state) { discharging = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(kSettlePollIntervalMs));
    }
    record.settle_ms = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - settle_start).count());
    if (!discharging) {
        // 没进放电就停机再退出：设备已经被使能，不能就这么放着。
        (void)stop_continuous();
        return fail_run(record, "settle", libs::ErrorKind::device, "laser_not_discharging",
                        "工作使能后设备在 " + std::to_string(cfg.settle_timeout_ms) +
                        " ms 内没有进入「正在工作且放电」，拒绝开光闸");
    }

    // 开光闸 —— **然后返回**。激光保持出光，直到 stop_continuous()。
    if (!record_step(cmd::kQGate, {0x01}, "set_q_gate_1")) {
        (void)stop_continuous();
        record.failed_step = "set_q_gate_1";
        record.error_code = run_error.code;
        record.error_message = run_error.message;
        return run_error;
    }

    record.ok = true;
    record.note = "连续出光已开始；激光按设定频率持续出光，直到手动停止";
    return record;
}

libs::VoidResult LaserFireService::stop_continuous() {
    libs::devices::Laser* laser = this->laser();
    if (laser == nullptr) {
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open",
                                 "激光器未打开，无法收光");
    }

    // **先关光闸、再停机**，顺序不能反：反了这一发可能正卡在光闸开着的窗口里。
    // 两步都做，任一步失败也继续做另一步 —— 收光优先于错误上报。
    std::string first_error;
    const auto gate_off = laser->write(cmd::kQGate, {0x00});
    if (!gate_off) first_error = "收光(0x24=0)失败：" + gate_off.error().message;
    const auto work_off = laser->write(cmd::kWorkState, {0x00});
    if (!work_off && first_error.empty()) {
        first_error = "停机(0x21=0)失败：" + work_off.error().message;
    }

    // 读回确认：**"我发过了"不等于"关上了"**。
    const auto tx = laser->read(cmd::kLdActual);
    if (!tx) {
        if (first_error.empty()) first_error = "读回确认失败：" + tx.error().message;
    } else {
        const policy::LaserSafetyFacts f = policy::parse_actual_frame(tx.value().frame);
        if (!f.q_gate && first_error.empty()) {
            first_error = "已下发收光，但读回仍显示光闸开着；请现场人工确认";
        } else if (f.work_state && first_error.empty()) {
            first_error = "已下发停机，但读回仍显示正在工作；请现场人工确认";
        }
    }

    if (!first_error.empty()) {
        return libs::Error::make(libs::ErrorKind::device, "laser_stop_incomplete", first_error);
    }
    return libs::VoidResult();
}

}  // namespace business::services
