#include "libs/business/app/runtime.hpp"
#include "libs/measurement_limits.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace business::app {
namespace {

using libs::devices::Su300Direction;
using libs::devices::Su300Status;
using Clock = std::chrono::steady_clock;
constexpr std::uint16_t kJogWatchdogDeciseconds = 10;
constexpr auto kJogStartupConfirmation = std::chrono::milliseconds(500);

libs::Error motor_error(const char* code, const char* message) {
    return libs::Error::make(libs::ErrorKind::device, code, message);
}

const char* direction_name(Su300Direction direction) {
    switch (direction) {
        case Su300Direction::forward: return "forward";
        case Su300Direction::reverse: return "reverse";
        case Su300Direction::stopped: return "stopped";
        default: return "unknown";
    }
}

}  // namespace

json::Value Runtime::motor_status() const {
    std::lock_guard<std::mutex> lock(readings_mutex_);
    return readings_["motor"];
}

libs::Result<json::Value> Runtime::motor_jog_profile() {
    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (motor_jog_ || motor_stop_pending_)
        return motor_error("motor_profile_busy", "请等待点动结束并确认停机后读取变频器参数");
    auto* drive = devices_.motor();
    if (!drive) return motor_error("su300_not_open", "SU300 连接不可用");
    const auto profile = drive->read_jog_profile();
    if (!profile) return profile.error();
    motor_frequency_unverified_ = false;

    const auto& value = profile.value();
    auto result = json::object();
    result.set("forward_hz", json::Value(value.forward_hz));
    result.set("reverse_hz", json::Value(value.reverse_hz));
    result.set("deceleration_ms", json::Value(static_cast<double>(value.deceleration_ms)));
    result.set("communication_timeout_ms", json::Value(static_cast<double>(value.communication_timeout_ms)));
    result.set("communication_loss_word", json::Value(static_cast<double>(value.communication_loss_word)));
    return result;
}

libs::Result<json::Value> Runtime::set_motor_jog_frequency(unsigned tenths_hz) {
    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (!config_.motor.connection.serial.enabled || !config_.motor.jog_ms)
        return motor_error("motor_control_disabled", "站点尚未启用手动移位");
    if (motor_jog_ || motor_stop_pending_ || measurement_starting_ || laser_session_active() ||
            (measurement_ && measurement_->stop_unconfirmed()))
        return motor_error("motor_frequency_busy", "请停止检测和点动后再设置点动频率");
    auto* drive = devices_.motor();
    if (!drive) return motor_error("su300_not_open", "SU300 连接不可用");
    const auto saved = drive->set_jog_frequency(tenths_hz);
    if (!saved) {
        if (saved.error().code == "su300_jog_frequency_restore_failed") motor_frequency_unverified_ = true;
        return saved.error();
    }
    motor_frequency_unverified_ = false;
    auto result = json::object();
    result.set("forward_hz", json::Value(tenths_hz / 10.0));
    result.set("reverse_hz", json::Value(tenths_hz / 10.0));
    return result;
}

bool Runtime::motor_limit(const Su300Status& status, Su300Direction direction) const {
    const auto& config = config_.motor;
    const auto mask = direction == Su300Direction::forward
        ? config.forward_limit_mask : config.reverse_limit_mask;
    return mask != 0 && ((status.inputs & mask) != 0) != config.limits_active_low;
}

void Runtime::publish_motor(const libs::Result<Su300Status>& status) {
    auto data = json::object();
    data.set("configured", json::Value(config_.motor.connection.serial.enabled));
    data.set("opened", json::Value(devices_.motor() != nullptr));
    data.set("control_enabled", json::Value(config_.motor.jog_ms != 0));
    data.set("jog_ms", json::Value(static_cast<double>(config_.motor.jog_ms)));
    data.set("jog_requested", json::Value(motor_jog_.has_value()));
    data.set("stop_pending", json::Value(motor_stop_pending_));
    if (!motor_jog_error_.empty()) data.set("jog_error", json::Value(motor_jog_error_));
    const bool stopped = status && status.value().stopped() && !motor_jog_ && !motor_stop_pending_;
    data.set("stopped", json::Value(stopped));
    if (status) {
        const auto& value = status.value();
        data.set("direction", json::Value(direction_name(value.direction)));
        data.set("jogging", json::Value(value.jogging));
        data.set("frequency_hz", json::Value(value.output_centihz / 100.0));
        data.set("reference_hz", json::Value(value.reference_centihz / 100.0));
        data.set("fault_code", json::Value(static_cast<double>(value.fault_code)));
        data.set("input_word", json::Value(static_cast<double>(value.inputs)));
        if (config_.motor.forward_limit_mask)
            data.set("forward_limit", json::Value(motor_limit(value, Su300Direction::forward)));
        if (config_.motor.reverse_limit_mask)
            data.set("reverse_limit", json::Value(motor_limit(value, Su300Direction::reverse)));
        data.set("communication_control", json::Value(value.communication_control));
    } else {
        data.set("error", json::Value(status.error().message));
    }
    {
        std::lock_guard<std::mutex> lock(readings_mutex_);
        readings_.set("motor", std::move(data));
    }
    if (head_stopped_.exchange(stopped) != stopped && measurement_)
        measurement_->on_device_status_changed();
}

void Runtime::log_motor_jog(const std::string& message, const libs::Result<Su300Status>& status,
                            unsigned duration_ms) {
    if (!log_) return;
    auto fields = json::object();
    if (duration_ms) fields.set("duration_ms", json::Value(static_cast<double>(duration_ms)));
    if (motor_jog_) fields.set("requested_direction", json::Value(direction_name(*motor_jog_)));
    if (!motor_jog_error_.empty()) fields.set("jog_error", json::Value(motor_jog_error_));
    if (status) {
        const auto& value = status.value();
        fields.set("run_word_2101", json::Value(static_cast<double>(value.run_word)));
        fields.set("flags_word_2116", json::Value(static_cast<double>(value.flags_word)));
        fields.set("direction_word_2117", json::Value(static_cast<double>(value.direction_word)));
        fields.set("output_hz", json::Value(value.output_centihz / 100.0));
        fields.set("fault_code", json::Value(static_cast<double>(value.fault_code)));
    } else {
        fields.set("error_code", json::Value(status.error().code));
        fields.set("error", json::Value(status.error().message));
    }
    log_->write(logging::RecordKind::general, message, std::move(fields));
}

libs::VoidResult Runtime::stop_motor_locked(libs::devices::Su300& drive) {
    motor_jog_.reset();
    motor_stop_pending_ = true;
    const auto result = drive.stop();
    motor_stop_retry_ = !result.ok();
    motor_watchdog_quiet_until_ = motor_stop_retry_ && motor_watchdog_armed_
        ? Clock::now() + std::chrono::milliseconds(1100) : Clock::time_point{};
    return result;
}

libs::VoidResult Runtime::arm_motor_watchdog(libs::devices::Su300& drive) {
    const auto original = drive.read_stop_watchdog();
    if (!original) return original.error();
    if (original.value() != 0 && original.value() < 4)
        return motor_error("su300_watchdog_too_short", "现有通讯超时短于 400 ms，无法可靠完成状态轮询");

    motor_watchdog_armed_ = true;
    if (original.value() > 0 && original.value() <= kJogWatchdogDeciseconds) return libs::VoidResult();

    // A lost reply can leave the write applied. Remember the original before sending it.
    motor_watchdog_restore_ = original.value();
    const auto written = drive.set_communication_watchdog(kJogWatchdogDeciseconds);
    if (written) return libs::VoidResult();

    const auto restored = restore_motor_watchdog(drive);
    if (!restored) {
        motor_stop_pending_ = true;
        publish_motor(restored.error());
        return restored;
    }
    return written;
}

libs::VoidResult Runtime::restore_motor_watchdog(libs::devices::Su300& drive) {
    if (motor_watchdog_restore_) {
        const auto restored = drive.set_communication_watchdog(*motor_watchdog_restore_);
        if (!restored) return restored;
        motor_watchdog_restore_.reset();
    }
    motor_watchdog_armed_ = false;
    return libs::VoidResult();
}

void Runtime::sample_motor() {
    std::lock_guard<std::mutex> lock(motor_mutex_);
    sample_motor_locked();
}

void Runtime::sample_motor_locked() {
    // Let the drive's watchdog act if a stop command cannot be confirmed.
    if (Clock::now() < motor_watchdog_quiet_until_) return;
    devices_.retry_open(session::DeviceSession::Device::motor);
    auto* drive = devices_.motor();
    if (!drive) {
        publish_motor(motor_error("su300_not_open", "SU300 连接不可用"));
        return;
    }
    const bool duration_reached = motor_jog_ && Clock::now() >= motor_deadline_;
    if (duration_reached || motor_stop_retry_) {
        if (duration_reached && !motor_jog_confirmed_)
            motor_jog_error_ = "设定时长已到，期间未读到预期方向和非零输出";
        const auto stopped = stop_motor_locked(*drive);
        if (!stopped) {
            log_motor_jog("电机停机指令未确认", stopped.error());
            publish_motor(stopped.error());
            return;
        }
    }
    const auto status = drive->read_status();
    if (motor_jog_) {
        const auto requested = *motor_jog_;
        if (!status) {
            motor_jog_error_ = "点动中通讯回读失败：" + status.error().message;
        } else {
            const auto& value = status.value();
            const bool confirmed = value.confirms_jog(requested);
            const auto now = Clock::now();
            if (value.fault_code) motor_jog_error_ = "变频器报告故障 " + std::to_string(value.fault_code);
            else if (!value.communication_control) motor_jog_error_ = "变频器已退出通信控制";
            else if (motor_limit(value, requested)) motor_jog_error_ = "点动方向的限位已触发";
            else if ((motor_jog_confirmed_ || value.jog_direction_from_run_word) &&
                    value.confirms_jog(requested == Su300Direction::forward ? Su300Direction::reverse : Su300Direction::forward))
                motor_jog_error_ = "变频器回读了相反的点动方向";
            else if (!confirmed && motor_jog_confirmed_) motor_jog_error_ = "点动中断，未读到预期方向和非零输出";
            else if (!confirmed && now >= motor_feedback_deadline_) motor_jog_error_ = "启动后未读到预期方向和非零输出";
            if (confirmed && !motor_jog_confirmed_ && motor_jog_error_.empty()) {
                motor_jog_confirmed_ = true;
                log_motor_jog("已回读电机点动方向及非零输出", status);
            }
        }
        const bool duration_elapsed = Clock::now() >= motor_deadline_;
        if (duration_elapsed && !motor_jog_confirmed_ && motor_jog_error_.empty())
            motor_jog_error_ = "设定时长已到，期间未读到预期方向和非零输出";
        if (!motor_jog_error_.empty() || duration_elapsed) {
            const auto stopped = stop_motor_locked(*drive);
            if (!motor_jog_error_.empty()) log_motor_jog("电机点动提前结束", status);
            if (!stopped) {
                log_motor_jog("电机停机指令未确认", stopped.error());
                publish_motor(stopped.error());
            } else {
                // The status above predates the stop command.
                publish_motor(motor_error("su300_stop_pending", "已请求停止，等待变频器回读"));
            }
            return;
        }
    }
    if (motor_stop_pending_ && status && status.value().stopped()) {
        const auto restored = restore_motor_watchdog(*drive);
        if (!restored) {
            publish_motor(restored.error());
            return;
        }
        motor_stop_pending_ = false;
        motor_stop_retry_ = false;
        log_motor_jog("电机停机已回读", status);
    }
    publish_motor(status);
}

libs::Result<json::Value> Runtime::motor_command(const std::string& action, std::optional<unsigned> duration_ms) {
    if (action == "status") return motor_status();
    if (action != "forward" && action != "reverse" && action != "stop")
        return libs::Error::make(libs::ErrorKind::invalid_argument, "motor_action_invalid", "不支持的电机操作");
    if (duration_ms && (action == "stop" || *duration_ms < libs::measurement_limits::kMotorJogMinMs ||
            *duration_ms > libs::measurement_limits::kMotorJogMaxMs))
        return libs::Error::make(libs::ErrorKind::invalid_argument, "motor_duration_invalid", "点动时长须为 100–50000 ms");

    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (!config_.motor.connection.serial.enabled || !config_.motor.jog_ms)
        return motor_error("motor_control_disabled", "站点尚未启用手动移位");
    auto* drive = devices_.motor();
    if (!drive) return motor_error("su300_not_open", "SU300 连接不可用");
    if (action == "stop") {
        const auto stopped = stop_motor_locked(*drive);
        if (!stopped) {
            publish_motor(stopped.error());
            return stopped.error();
        }
        sample_motor_locked();
        return motor_status();
    }

    if (measurement_starting_ || laser_session_active() || measurement_->stop_unconfirmed())
        return motor_error("motor_measurement_active", "检测未结束或停机未确认，暂不能移位");
    if (motor_jog_ || motor_stop_pending_)
        return motor_error("motor_stop_pending", "上一条移位尚未结束");
    if (motor_frequency_unverified_)
        return motor_error("motor_frequency_unverified", "上次点动频率设置未确认，请先重新读取变频器");
    if (config_.laser.enabled) {
        measurement_->refresh_gate();
        std::lock_guard<std::mutex> readings_lock(readings_mutex_);
        if (!laser_safety_.available || !laser_safety_.q_gate || !laser_safety_.emergency_stop ||
            !laser_safety_.interlock_1 || !laser_safety_.interlock_2)
            return motor_error("motor_laser_not_safe", "无法确认光闸关闭、联锁闭合且急停解除");
    }
    const auto status = drive->read_status();
    publish_motor(status);
    if (!status) return status.error();
    if (!status.value().communication_control)
        return motor_error("su300_not_remote", "SU300 当前未选择通信运行命令通道");
    if (!status.value().stopped() || status.value().fault_code)
        return motor_error("su300_not_ready", "SU300 有故障或尚未确认停机");
    const auto direction = action == "forward" ? Su300Direction::forward : Su300Direction::reverse;
    if (motor_limit(status.value(), direction))
        return motor_error("motor_limit_active", "该方向限位已触发");

    motor_jog_error_.clear();
    const auto watchdog = arm_motor_watchdog(*drive);
    if (!watchdog) {
        log_motor_jog("电机点动未启动", watchdog.error());
        return watchdog.error();
    }

    motor_jog_ = direction;
    motor_jog_confirmed_ = false;
    const unsigned duration = duration_ms.value_or(config_.motor.jog_ms);
    const auto started = Clock::now();
    motor_deadline_ = started + std::chrono::milliseconds(duration);
    motor_feedback_deadline_ = started + kJogStartupConfirmation;
    publish_motor(status);
    const auto jog = drive->jog(direction);
    if (!jog) {
        motor_jog_error_ = "点动启动回执未确认：" + jog.error().message;
        const auto stopped = stop_motor_locked(*drive);
        log_motor_jog("电机点动启动回执未确认", jog.error(), duration);
        publish_motor(stopped ? jog.error() : stopped.error());
        return jog.error();
    }
    log_motor_jog("电机点动指令已应答", status, duration);
    return motor_status();
}

libs::Result<measurement::MeasurementStartRecord> Runtime::start_measurement(
        const measurement::FireParameters& params) {
    if (!config_.motor.connection.serial.enabled) return measurement_->start(params);
    {
        std::lock_guard<std::mutex> lock(motor_mutex_);
        if (measurement_starting_ || measurement_->running())
            return motor_error("laser_already_running", "检测已启动或正在启动");
        sample_motor_locked();
        if (!head_stopped_.load())
            return motor_error("head_stop_unconfirmed", "检测头仍在移位或尚未确认停机");
        measurement_starting_ = true;
    }
    struct ClearStarting {
        std::mutex& mutex;
        bool& starting;
        ~ClearStarting() {
            std::lock_guard<std::mutex> lock(mutex);
            starting = false;
        }
    } clear{motor_mutex_, measurement_starting_};
    return measurement_->start(params);
}

libs::VoidResult Runtime::stop_motor_on_exit() {
    std::lock_guard<std::mutex> lock(motor_mutex_);
    if (!motor_jog_ && !motor_stop_pending_) return libs::VoidResult();
    auto* drive = devices_.motor();
    if (!drive) return motor_error("motor_stop_unconfirmed", "退出时未能确认移位电机停机");
    stop_motor_locked(*drive);
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        sample_motor_locked();
        if (!motor_stop_pending_) return libs::VoidResult();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (Clock::now() < deadline);
    return motor_error("motor_stop_unconfirmed", "退出时未能确认移位电机停机");
}

}  // namespace business::app
