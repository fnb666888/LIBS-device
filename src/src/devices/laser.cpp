#include "libs/devices/laser.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include "libs/protocols/laser.hpp"
#include "libs/measurement_limits.hpp"

namespace libs::devices {
namespace {

namespace cmd = libs::laser::command;

std::uint16_t field_u16(const libs::laser::Frame& frame, const char* name) {
    for (const auto& field : frame.fields) {
        if (field.name == name) return field.raw;
    }
    return 0;
}

std::uint32_t field_u32(const libs::laser::Frame& frame, const char* name) {
    for (const auto& field : frame.fields) {
        if (field.name != name) continue;
        return field.wide ? field.raw32 : field.raw;
    }
    return 0;
}

Error op_error(ErrorKind kind, std::string code, std::string message) {
    return Error::make(kind, std::move(code), std::move(message));
}

Result<libs::laser::Frame> decode_response(const Bytes& reply, std::uint8_t command) {
    unsigned complete_frames = 0;
    libs::laser::Frame frame;
    Error last_error = Error::make(ErrorKind::protocol, "no_frame",
                                   "no complete laser frame in the reply");
    for (std::size_t offset = 0; offset + 7 <= reply.size(); ++offset) {
        const auto scan = libs::laser::find_frame(
            Bytes(reply.begin() + static_cast<std::ptrdiff_t>(offset), reply.end()));
        if (scan.status == libs::laser::FrameStatus::invalid) continue;
        Bytes window(reply.begin() + static_cast<std::ptrdiff_t>(offset),
                     reply.begin() + static_cast<std::ptrdiff_t>(offset + scan.length));
        auto decoded = libs::laser::decode_frame(window, command);
        if (!decoded) {
            last_error = decoded.error();
            continue;
        }
        frame = std::move(decoded.value());
        ++complete_frames;
        offset += scan.length - 1;
    }

    if (complete_frames == 0) return last_error;
    if (complete_frames > 1)
        return Error::with_raw(ErrorKind::protocol, "ambiguous_reply",
                               "the reply contains more than one complete laser frame; "
                               "it is not clear which one answers this request",
                               reply);
    return frame;
}

LaserActual decode_actual(const libs::laser::Frame& frame) {
    LaserActual out;
    out.valid = true;
    out.work_state = static_cast<std::uint8_t>(field_u16(frame, "work_state"));
    out.clock_mode = static_cast<std::uint8_t>(field_u16(frame, "clock_mode"));
    out.fault_bits = static_cast<std::uint8_t>(field_u16(frame, "fault_bits"));
    out.interlock_1 = static_cast<std::uint8_t>(field_u16(frame, "interlock_1"));
    out.interlock_2 = static_cast<std::uint8_t>(field_u16(frame, "interlock_2"));
    out.emergency_stop = static_cast<std::uint8_t>(field_u16(frame, "emergency_stop"));
    out.overtemperature = static_cast<std::uint8_t>(field_u16(frame, "overtemperature"));
    out.q_gate = static_cast<std::uint8_t>(field_u16(frame, "q_gate"));
    out.work_enabled = static_cast<std::uint8_t>(field_u16(frame, "work_enabled"));
    out.discharge_clock_enabled =
        static_cast<std::uint8_t>(field_u16(frame, "discharge_clock_enabled"));
    out.ld_temperature_raw = field_u16(frame, "ld_temperature");
    out.actual_current_raw = field_u16(frame, "actual_current");
    out.actual_voltage_raw = field_u16(frame, "actual_voltage");
    out.heatsink_temperature_raw = field_u16(frame, "heatsink_temperature");
    out.discharge_counter = field_u32(frame, "discharge_counter");
    return out;
}

}  // namespace

Result<Laser::LaserTransaction> Laser::readTransaction(std::uint8_t command) {
    if (!libs::laser::is_read_command(command))
        return Error::make(ErrorKind::invalid_argument, "bad_command",
                           "read is limited to B1, B2 and BC");

    Bytes request;
    try {
        request = libs::laser::make_read_request(command);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "request_build_failed", e.what());
    }

    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    LaserTransaction tx;
    tx.request = request;
    tx.response = reply.bytes;
    tx.elapsed_ms = reply.elapsed_ms;
    tx.receive_errors = reply.receive_errors;
    tx.last_receive_tick = reply.last_receive_tick;
    tx.timed_out = reply.timed_out;

    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while reading the laser",
                               reply.bytes);

    const auto decoded = decode_response(reply.bytes, command);
    if (!decoded) return decoded.error();
    return tx;
}

Result<Laser::LaserTransaction> Laser::writeTransaction(std::uint8_t command,
                                                        const Bytes& payload) {
    Bytes request;
    try {
        request = libs::laser::make_write_frame(command, payload);
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::invalid_argument, "request_build_failed", e.what());
    }
    auto exchanged = link_.exchange(request, timeout_ms_);
    if (!exchanged) return exchanged.error();
    const auto& reply = exchanged.value();

    LaserTransaction tx;
    tx.request = request;
    tx.response = reply.bytes;
    tx.elapsed_ms = reply.elapsed_ms;
    tx.receive_errors = reply.receive_errors;
    tx.last_receive_tick = reply.last_receive_tick;
    tx.timed_out = reply.timed_out;
    if (reply.receive_errors)
        return Error::with_raw(ErrorKind::transport, "uart_errors",
                               "serial parity/framing/overrun error while writing to the laser",
                               reply.bytes);
    return tx;
}

Result<Laser::LaserTransaction> Laser::sendTransaction(std::uint8_t command,
                                                       std::uint8_t value) {
    return writeTransaction(command, Bytes{value});
}

Result<LaserActual> Laser::appendReadActual(LaserOperationTrace* trace) {
    LaserOperationTrace local;
    LaserOperationTrace& target = trace != nullptr ? *trace : local;
    const auto tx = readTransaction(cmd::kLdActual);
    if (!tx) {
        target.failed_step = "read_actual";
        return tx.error();
    }
    target.sent_frames.push_back(libs::to_hex(tx.value().request));
    const auto frame = decode_response(tx.value().response, cmd::kLdActual);
    if (!frame) return frame.error();
    return decode_actual(frame.value());
}

Result<LaserActual> Laser::readActual(LaserOperationTrace* trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    return appendReadActual(trace);
}

Result<LaserSettings> Laser::readSettings(LaserOperationTrace* trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    LaserOperationTrace local;
    LaserOperationTrace& target = trace != nullptr ? *trace : local;
    const auto tx = readTransaction(cmd::kLdSettings);
    if (!tx) {
        target.failed_step = "read_settings";
        return tx.error();
    }
    target.sent_frames.push_back(libs::to_hex(tx.value().request));
    const auto frame = decode_response(tx.value().response, cmd::kLdSettings);
    if (!frame) return frame.error();
    LaserSettings out;
    out.discharge_current_raw = field_u16(frame.value(), "discharge_current");
    out.discharge_pulse_width_raw = field_u16(frame.value(), "discharge_pulse_width");
    out.discharge_frequency_raw = field_u16(frame.value(), "discharge_frequency");
    out.discharge_delay_raw = field_u16(frame.value(), "discharge_delay");
    out.q_delay_raw = field_u16(frame.value(), "q_delay");
    out.ld_temperature_limit_raw = field_u16(frame.value(), "ld_temperature_limit");
    out.voltage_limit_raw = field_u16(frame.value(), "voltage_limit");
    out.q_divider_raw = field_u16(frame.value(), "q_divider");
    out.q_warmup_raw = field_u16(frame.value(), "q_warmup");
    return out;
}

Result<LaserTec> Laser::readTec(LaserOperationTrace* trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    LaserOperationTrace local;
    LaserOperationTrace& target = trace != nullptr ? *trace : local;
    const auto tx = readTransaction(cmd::kTec);
    if (!tx) {
        target.failed_step = "read_tec";
        return tx.error();
    }
    target.sent_frames.push_back(libs::to_hex(tx.value().request));
    const auto frame = decode_response(tx.value().response, cmd::kTec);
    if (!frame) return frame.error();
    LaserTec out;
    out.max_temperature_raw = field_u16(frame.value(), "tec_max_temperature");
    out.min_temperature_raw = field_u16(frame.value(), "tec_min_temperature");
    out.b_coefficient_raw = field_u16(frame.value(), "tec_b_coefficient");
    out.thermistor_resistance_raw = field_u16(frame.value(), "thermistor_resistance");
    out.stage_voltage_raw = field_u16(frame.value(), "stage_voltage");
    out.tec_voltage_raw = field_u16(frame.value(), "tec_voltage");
    out.kp_raw = field_u16(frame.value(), "kp");
    out.ki_raw = field_u16(frame.value(), "ki");
    out.kd_raw = field_u16(frame.value(), "kd");
    out.actual_temperature_raw = field_u16(frame.value(), "tec_actual_temperature");
    out.work_intensity_raw = field_u16(frame.value(), "work_intensity");
    return out;
}

VoidResult Laser::writeChecked(std::uint8_t command,
                               const Bytes& payload,
                               const char* step,
                               LaserOperationTrace& trace) {
    const auto tx = writeTransaction(command, payload);
    if (!tx) {
        trace.failed_step = step;
        return tx.error();
    }
    trace.sent_frames.push_back(libs::to_hex(tx.value().request));
    return VoidResult();
}

VoidResult Laser::resetFault(LaserOperationTrace* trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    LaserOperationTrace local;
    LaserOperationTrace& target = trace != nullptr ? *trace : local;
    return writeChecked(cmd::kResetFault, {}, "reset_fault", target);
}

VoidResult Laser::clearCounter(LaserOperationTrace* trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    LaserOperationTrace local;
    LaserOperationTrace& target = trace != nullptr ? *trace : local;
    return writeChecked(cmd::kClearCounter, {}, "clear_counter", target);
}

VoidResult Laser::writeSession(const LaserSessionSettings& settings,
                               LaserOperationTrace& trace) {
    struct Param {
        std::uint8_t command;
        unsigned value;
        const char* name;
    };
    const Param params[] = {
        {cmd::kDischargeCurrent, settings.discharge_current_a, "set_discharge_current"},
        {cmd::kDischargePulseWidth, settings.pulse_width_us, "set_pulse_width"},
        {cmd::kDischargeFrequency, settings.frequency_hz, "set_frequency"},
        {cmd::kDischargeDelay, settings.discharge_delay_us, "set_discharge_delay"},
        {cmd::kQDelay, settings.q_delay_us, "set_q_delay"},
        {cmd::kQWarmup, settings.q_warmup_s, "set_q_warmup"},
    };
    for (const Param& p : params) {
        const Bytes payload{static_cast<std::uint8_t>((p.value >> 8) & 0xFF),
                            static_cast<std::uint8_t>(p.value & 0xFF)};
        const VoidResult written = writeChecked(p.command, payload, p.name, trace);
        if (!written) return written.error();
    }
    return VoidResult();
}

VoidResult Laser::prepareContinuous(const LaserSessionSettings& settings,
                                    unsigned settle_timeout_ms,
                                    LaserOperationTrace& trace,
                                    unsigned poll_interval_ms) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    const std::pair<const char*, unsigned> parameters[] = {
        {"discharge_current_a", settings.discharge_current_a}, {"pulse_width_us", settings.pulse_width_us},
        {"frequency_hz", settings.frequency_hz}, {"discharge_delay_us", settings.discharge_delay_us},
        {"q_delay_us", settings.q_delay_us}, {"q_warmup_s", settings.q_warmup_s},
    };
    for (const auto& parameter : parameters) {
        if (!libs::measurement_limits::accepts("laser", parameter.first, parameter.second)) {
            trace.failed_step = parameter.first;
            return op_error(ErrorKind::invalid_argument, "bad_laser_parameter",
                            std::string(parameter.first) + " 超出允许范围");
        }
    }
    if (!libs::measurement_limits::accepts("laser", "settle_timeout_ms", settle_timeout_ms))
        return op_error(ErrorKind::invalid_argument, "bad_settle_timeout",
                        "settle_timeout_ms must be in 1..600000 ms");
    if (poll_interval_ms == 0 && settle_timeout_ms > 1)
        return op_error(ErrorKind::invalid_argument, "bad_poll_interval",
                        "poll_interval_ms must be positive");

    const auto baseline = readActual(&trace);
    if (!baseline) return baseline.error();
    trace.before = baseline.value();
    trace.has_before = true;
    trace.counter_before = trace.before.discharge_counter;
    if (trace.before.fault_bits != 0) {
        trace.failed_step = "baseline_fault";
        return op_error(ErrorKind::device, "laser_fault",
                        "激光器 0xB2 故障位非零，拒绝写入会话参数");
    }

    const VoidResult params = writeSession(settings, trace);
    if (!params) return params.error();

    const VoidResult clock = writeChecked(cmd::kClockMode, {0x00}, "set_internal_clock_mode", trace);
    if (!clock) return clock.error();

    const VoidResult work_on = writeChecked(cmd::kWorkState, {0x01},
                                             "set_work_state_1", trace);
    if (!work_on) {
        const std::string failed_step = trace.failed_step;
        (void)stopContinuous(trace);
        trace.failed_step = failed_step;
        return work_on.error();
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(settle_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto actual = readActual(&trace);
        if (!actual) {
            trace.failed_step = "settle_read";
            const std::string failed_step = trace.failed_step;
            (void)stopContinuous(trace);
            trace.failed_step = failed_step;
            return actual.error();
        }
        if (actual.value().fault_bits != 0) {
            trace.failed_step = "settle_fault";
            const std::string failed_step = trace.failed_step;
            (void)stopContinuous(trace);
            trace.failed_step = failed_step;
            return op_error(ErrorKind::device, "laser_fault_during_settle",
                            "等待放电期间激光器报了故障位");
        }
        if (actual.value().work_state == 1) {
            trace.settle_ms = static_cast<unsigned>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count());
            return VoidResult();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }

    trace.settle_ms = static_cast<unsigned>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    trace.failed_step = "settle";
    const std::string failed_step = trace.failed_step;
    (void)stopContinuous(trace);
    trace.failed_step = failed_step;
    return op_error(ErrorKind::device, "laser_not_discharging",
                    "工作使能后设备没有进入「正在工作且放电」，拒绝开光闸");
}

VoidResult Laser::openQGate(LaserOperationTrace& trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    const VoidResult opened = writeChecked(cmd::kQGate, {0x01}, "set_q_gate_1", trace);
    if (!opened) {
        const std::string failed_step = trace.failed_step;
        (void)stopContinuous(trace);
        trace.failed_step = failed_step;
        return opened.error();
    }
    running_ = true;
    return VoidResult();
}

VoidResult Laser::startContinuous(const LaserSessionSettings& settings,
                                  unsigned settle_timeout_ms,
                                  LaserOperationTrace& trace,
                                  unsigned poll_interval_ms) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    const VoidResult prepared = prepareContinuous(settings, settle_timeout_ms,
                                                  trace, poll_interval_ms);
    if (!prepared) return prepared.error();
    return openQGate(trace);
}

VoidResult Laser::stopContinuous(LaserOperationTrace& trace) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    std::string first_error;

    const auto gate_off = writeTransaction(cmd::kQGate, {0x00});
    if (!gate_off) {
        first_error = "收光(0x24=0)失败：" + gate_off.error().message;
    } else {
        trace.sent_frames.push_back(libs::to_hex(gate_off.value().request));
    }

    const auto work_off = writeTransaction(cmd::kWorkState, {0x00});
    if (!work_off) {
        if (first_error.empty()) first_error = "停机(0x21=0)失败：" + work_off.error().message;
    } else {
        trace.sent_frames.push_back(libs::to_hex(work_off.value().request));
    }

    const auto confirm = readActual(&trace);
    if (!confirm) {
        if (first_error.empty()) first_error = "读回确认失败：" + confirm.error().message;
    } else {
        trace.after = confirm.value();
        trace.has_after = true;
        trace.counter_after = trace.after.discharge_counter;
        if (trace.counter_after > trace.counter_before)
            trace.discharges = trace.counter_after - trace.counter_before;
        if (trace.after.q_gate != 0 && first_error.empty())
            first_error = "已下发收光，但读回仍显示光闸开着；请现场人工确认";
        if (trace.after.work_state != 0 && first_error.empty())
            first_error = "已下发停机，但读回仍显示正在工作；请现场人工确认";
    }

    running_ = false;
    if (!first_error.empty()) {
        trace.failed_step = "stop_continuous";
        return op_error(ErrorKind::device, "laser_stop_incomplete", std::move(first_error));
    }
    trace.failed_step.clear();
    return VoidResult();
}

VoidResult Laser::fireOnce(const LaserSessionSettings& settings,
                           unsigned settle_timeout_ms,
                           unsigned hold_ms,
                           LaserOperationTrace& trace,
                           unsigned poll_interval_ms) {
    std::lock_guard<std::recursive_mutex> operation(operation_mutex_);
    if (!libs::measurement_limits::accepts("laser", "emission_hold_ms", hold_ms))
        return op_error(ErrorKind::invalid_argument, "bad_emission_hold", "hold_ms 超出允许范围");
    const VoidResult started = startContinuous(settings, settle_timeout_ms,
                                               trace, poll_interval_ms);
    // 入参校验在任何设备事务前完成，此类拒绝不进入出光/收光时序。
    if (!started && started.error().kind == ErrorKind::invalid_argument) return started.error();
    const std::string started_failed_step = trace.failed_step;
    if (started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }
    const VoidResult stopped = stopContinuous(trace);
    if (!started) {
        trace.failed_step = started_failed_step;
        return started.error();
    }
    if (!stopped) return stopped.error();
    return VoidResult();
}

}  // namespace libs::devices
