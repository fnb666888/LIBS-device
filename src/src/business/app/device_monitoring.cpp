#include "libs/business/app/runtime.hpp"

#include <algorithm>
#include <chrono>

#include "libs/business/readiness/lamp.hpp"
#include "libs/business/shared/time_util.hpp"

namespace business::app {
namespace {

using readiness::DeviceCondition;
using readiness::MachineState;
using SessionDevice = session::DeviceSession::Device;

constexpr unsigned kAuxiliaryPollMs = 1000;
constexpr unsigned kSpectrometerPollMs = 2000;
constexpr unsigned kIdleLaserPollMs = 2000;

std::string auxiliary_state_error(const std::string& message) {
    return message.empty() ? "通信失败" : "通信失败（" + message + "）";
}

json::Value spectrum_channels(const std::vector<libs::devices::SpectrometerInfo>& infos) {
    json::Array channels;
    for (const auto& info : infos) {
        auto channel = json::object();
        channel.set("serial", json::Value(info.serial));
        channel.set("name", json::Value(info.friendly_name));
        if (info.pixels_known) {
            channel.set("pixels", json::Value(static_cast<double>(info.pixels)));
            channel.set("wavelength_min_nm", json::Value(info.wavelength_min_nm));
            channel.set("wavelength_max_nm", json::Value(info.wavelength_max_nm));
        }
        channels.emplace_back(std::move(channel));
    }
    return json::Value(std::move(channels));
}

bool air_fault(std::uint16_t address) {
    using namespace libs::air_conditioner::reg;
    return address == kIndoorSensorFault || address == kEvaporatorSensorFault ||
           address == kCondenserSensorFault || address == kHumiditySensorFault ||
           address == kCoolingAlarm;
}

}  // namespace

void Runtime::start_sampler(std::function<void()> sample, unsigned interval_ms) {
    samplers_.emplace_back([this, sample = std::move(sample), interval_ms] {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(sampler_mutex_);
                if (samplers_stopping_) return;
            }
            sample();
            std::unique_lock<std::mutex> lock(sampler_mutex_);
            if (sampler_wake_.wait_for(lock, std::chrono::milliseconds(interval_ms),
                                       [this] { return samplers_stopping_; })) return;
        }
    });
}

void Runtime::start_device_monitoring() {
    samplers_stopping_ = false;
    laser_sample_requested_ = false;
    laser_safety_ = {};
    laser_opened_ = false;
    readings_ = json::object();
    readings_.set("laser", json::object());
    for (const char* name : {"rangefinder", "air_conditioner"}) {
        auto device = json::object();
        device.set("opened", json::Value(false));
        readings_.set(name, std::move(device));
    }
    readings_.set("spectrometer_channels", spectrum_channels(devices_.spectrometer_channels()));
    indicator_status_ = json::object();
    applied_lamp_state_.reset();
    auxiliary_ = {};
    auxiliary_fresh_until_ms_ = {};
    if (config_.indicator.enabled) auxiliary_[0].name = "三色灯";
    if (config_.rangefinder.serial.enabled) auxiliary_[1].name = "测距仪";
    if (config_.air_conditioner.serial.enabled) auxiliary_[2].name = "空调";

    monitor_->arm();
    measurement_->refresh_gate();
    if (options_.open_devices) {
        if (config_.indicator.enabled) start_sampler([this] { sample_indicator(); }, kAuxiliaryPollMs);
        if (config_.rangefinder.serial.enabled) start_sampler([this] { sample_rangefinder(); }, kAuxiliaryPollMs);
        if (config_.air_conditioner.serial.enabled) start_sampler([this] { sample_air_conditioner(); }, kAuxiliaryPollMs);
        if (config_.spectrometer.enabled) start_sampler([this] { sample_spectrometer(); }, kSpectrometerPollMs);
    }
    samplers_.emplace_back([this] { run_laser_sampler(); });
}

void Runtime::run_laser_sampler() {
    for (;;) {
        std::unique_lock<std::mutex> lock(sampler_mutex_);
        const auto period = measurement_->running()
            ? measurement::kGateRecheckIntervalMs : kIdleLaserPollMs;
        sampler_wake_.wait_for(lock, std::chrono::milliseconds(period), [this] {
            return samplers_stopping_ || laser_sample_requested_;
        });
        if (samplers_stopping_) return;
        laser_sample_requested_ = false;
        lock.unlock();
        measurement_->refresh_gate();
    }
}

void Runtime::request_laser_sample() {
    {
        std::lock_guard<std::mutex> lock(sampler_mutex_);
        laser_sample_requested_ = true;
    }
    sampler_wake_.notify_all();
}

void Runtime::stop_device_monitoring() {
    {
        std::lock_guard<std::mutex> lock(sampler_mutex_);
        samplers_stopping_ = true;
    }
    sampler_wake_.notify_all();
    for (auto& sampler : samplers_) sampler.join();
    samplers_.clear();
    if (auto* indicator = devices_.indicator()) readiness::apply_lamp(MachineState::offline, indicator);
}

json::Value Runtime::device_readings() const {
    std::lock_guard<std::mutex> lock(readings_mutex_);
    return readings_;
}

json::Value Runtime::indicator_status() const {
    std::lock_guard<std::mutex> lock(readings_mutex_);
    return indicator_status_;
}

void Runtime::sample_laser() {
    auto data = json::object();
    readiness::LaserSafetyFacts safety;
    if (config_.laser.enabled) devices_.retry_open(SessionDevice::laser);
    auto* laser = devices_.laser();
    if (laser) {
        const auto actual = laser->readActual();
        if (!actual) {
            data.set("error", json::Value(actual.error().message));
        } else {
            const auto& state = actual.value();
            safety = readiness::parse_actual(state);
            data.set("work_state", json::Value(static_cast<double>(state.work_state)));
            data.set("q_gate_open", json::Value(!safety.q_gate));
            data.set("interlock_1", json::Value(safety.interlock_1));
            data.set("interlock_2", json::Value(safety.interlock_2));
            data.set("emergency_stop", json::Value(!safety.emergency_stop));
            data.set("overtemperature", json::Value(!safety.over_temperature));
            data.set("fault_bits", json::Value(static_cast<double>(state.fault_bits)));
            data.set("discharge_counter", json::Value(static_cast<double>(state.discharge_counter)));
        }
    }
    std::lock_guard<std::mutex> lock(readings_mutex_);
    readings_.set("laser", std::move(data));
    laser_opened_ = laser != nullptr;
    laser_safety_ = safety;
}

void Runtime::collect_gate_facts(readiness::ReadinessFacts& facts) {
    facts.stop_unconfirmed = measurement_ && measurement_->stop_unconfirmed();
    std::lock_guard<std::mutex> lock(readings_mutex_);
    facts.devices_opened = laser_opened_;
    facts.laser = laser_safety_;
    const auto now = static_cast<std::uint64_t>(monotonic_ms());
    for (std::size_t i = 0; i < auxiliary_.size(); ++i) {
        const auto& device = auxiliary_[i];
        if (device.name.empty()) continue;
        auto observed = device;
        if (observed.condition == DeviceCondition::normal && now >= auxiliary_fresh_until_ms_[i]) {
            observed.condition = DeviceCondition::unknown;
            observed.detail = "未收到新的设备状态";
            observed.warnings.clear();
        }
        facts.auxiliary.push_back(std::move(observed));
    }
}

void Runtime::publish_auxiliary(AuxiliaryDevice device, DeviceCondition condition,
                                std::string detail, std::vector<std::string> warnings) {
    std::unique_lock<std::mutex> lock(readings_mutex_);
    const auto index = static_cast<std::size_t>(device);
    auto& reading = auxiliary_[index];
    const auto now = static_cast<std::uint64_t>(monotonic_ms());
    auto& fresh_until = auxiliary_fresh_until_ms_[index];
    // Include the device's I/O budget plus one polling interval and scheduling margin.
    const std::array<std::uint64_t, 3> poll_budget = {
        7ULL * (config_.indicator.timeout_ms + config_.indicator.idle_ms + 60),
        config_.rangefinder.serial.timeout_ms,
        2ULL * config_.air_conditioner.serial.timeout_ms,
    };
    // A lost reply cannot clear a previously confirmed hardware fault.
    if (reading.condition == DeviceCondition::fault && condition == DeviceCondition::communication_failed) return;
    reading.condition = condition;
    reading.detail = condition == DeviceCondition::communication_failed
        ? auxiliary_state_error(detail) : std::move(detail);
    reading.warnings = std::move(warnings);
    if (condition == DeviceCondition::normal) fresh_until = now + poll_budget[index] + kAuxiliaryPollMs + 500;
    lock.unlock();
    measurement_->on_device_status_changed();
}

void Runtime::sample_indicator() {
    devices_.retry_open(SessionDevice::indicator);
    auto* indicator = devices_.indicator();
    const auto target = monitor_->latest_state();
    libs::Error error;
    if (!indicator) {
        error = libs::Error::make(libs::ErrorKind::transport, "indicator_not_open", "状态灯未打开");
    } else if (applied_lamp_state_ != target) {
        const auto applied = readiness::apply_lamp(target, indicator);
        if (applied) applied_lamp_state_ = target;
        else error = applied.error();
    } else {
        const auto actual = indicator->queryAll();
        if (!actual) {
            error = actual.error();
        } else {
            const auto expected = readiness::lamp_states(target);
            for (const auto& [channel, response] : actual.value()) {
                if (expected.at(channel) != response.state) {
                    error = libs::Error::make(libs::ErrorKind::device, "indicator_state_mismatch", "灯态与回读不一致");
                    applied_lamp_state_.reset();
                    break;
                }
            }
        }
    }
    const auto condition = !error ? DeviceCondition::normal :
        error.code == "indicator_state_mismatch" ? DeviceCondition::fault : DeviceCondition::communication_failed;
    publish_auxiliary(AuxiliaryDevice::indicator, condition, error.message);
    auto lamp = json::object();
    lamp.set("intent", json::Value(readiness::lamp_intent_name(target)));
    lamp.set("applied", json::Value(!error));
    if (error) lamp.set("last_error", json::Value(error.message));
    std::lock_guard<std::mutex> lock(readings_mutex_);
    indicator_status_ = std::move(lamp);
}

void Runtime::sample_rangefinder() {
    auto data = json::object();
    devices_.retry_open(SessionDevice::rangefinder);
    auto* range = devices_.rangefinder();
    data.set("opened", json::Value(range != nullptr));
    if (!range) {
        publish_auxiliary(AuxiliaryDevice::rangefinder, DeviceCondition::unknown, "测距仪未打开");
    } else {
        const auto observed = range->readObservation();
        if (!observed) {
            publish_auxiliary(AuxiliaryDevice::rangefinder, DeviceCondition::communication_failed,
                              observed.error().message);
            data.set("error", json::Value(observed.error().message));
        } else {
            const auto& sample = observed.value();
            std::vector<std::string> warnings;
            if (sample.distance_mm) data.set("distance_mm", json::Value(static_cast<double>(*sample.distance_mm)));
            else warnings.emplace_back("当前距离读数不可用");
            if (sample.confidence_percent)
                data.set("confidence_percent", json::Value(static_cast<double>(*sample.confidence_percent)));
            data.set("sample_counter", json::Value(static_cast<double>(sample.sample_counter)));
            publish_auxiliary(AuxiliaryDevice::rangefinder, DeviceCondition::normal, {}, std::move(warnings));
        }
    }
    std::lock_guard<std::mutex> lock(readings_mutex_);
    readings_.set("rangefinder", std::move(data));
}

void Runtime::sample_air_conditioner() {
    auto data = json::object();
    devices_.retry_open(SessionDevice::air_conditioner);
    auto* air = devices_.air_conditioner();
    data.set("opened", json::Value(air != nullptr));
    if (!air) {
        publish_auxiliary(AuxiliaryDevice::air_conditioner, DeviceCondition::unknown, "空调未打开");
    } else {
        std::lock_guard<std::mutex> device_lock(auxiliary_device_mutex_);
        const auto status = air->readStatus();
        if (!status) {
            publish_auxiliary(AuxiliaryDevice::air_conditioner, DeviceCondition::communication_failed,
                              status.error().message);
            data.set("status_error", json::Value(status.error().message));
        } else {
            using namespace libs::air_conditioner::reg;
            json::Array alarms;
            std::string fault;
            std::vector<std::string> warnings;
            for (const auto& bit : status.value().bits) {
                if (bit.address == kPowerState) data.set("power_on", json::Value(bit.active));
                if (bit.address == kCoolingOn) data.set("cooling", json::Value(bit.active));
                if (bit.address == kHeaterOn) data.set("heating", json::Value(bit.active));
                if (!bit.is_alarm || !bit.active) continue;
                alarms.emplace_back(bit.name);
                if (air_fault(bit.address)) {
                    if (!fault.empty()) fault += "、";
                    fault += bit.name;
                } else {
                    warnings.push_back(bit.name);
                }
            }
            data.set("alarms", json::Value(std::move(alarms)));
            const auto condition = fault.empty() ? DeviceCondition::normal : DeviceCondition::fault;
            data.set("fault", json::Value(condition == DeviceCondition::fault));
            publish_auxiliary(AuxiliaryDevice::air_conditioner, condition, std::move(fault), std::move(warnings));
            const auto parameters = air->readParams();
            if (!parameters) {
                data.set("parameters_error", json::Value(parameters.error().message));
            } else {
                for (const auto& field : parameters.value().registers) {
                    if (field.address == kIndoorTemp) data.set("temperature_c", json::Value(field.value));
                    if (field.address == kHumidity) data.set("humidity_percent", json::Value(field.value));
                    if (field.address == kMode) data.set("mode_code", json::Value(static_cast<double>(field.raw)));
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(readings_mutex_);
    readings_.set("air_conditioner", std::move(data));
}

void Runtime::sample_spectrometer() {
    std::unique_lock<std::mutex> poll(spectrometer_poll_mutex_, std::try_to_lock);
    if (!poll.owns_lock()) return;
    std::unique_lock<std::mutex> access(acquisition_mutex_, std::try_to_lock);
    if (!access.owns_lock() || measurement_->running()) return;
    auto* spectrometer = devices_.spectrometer();
    if (!spectrometer) return;
    if (measurement_->take_spectrometer_reset_request()) spectrometer->resetChannels();
    const auto channels = spectrometer->probeChannels();
    bool ready = false;
    if (channels) {
        const auto& infos = channels.value();
        const auto& required = config_.spectrometer.serials;
        ready = std::any_of(infos.begin(), infos.end(), [](const auto& info) { return info.pixels_known; }) &&
            std::all_of(required.begin(), required.end(), [&](const auto& serial) {
                return std::any_of(infos.begin(), infos.end(), [&](const auto& info) {
                    return info.serial == serial && info.pixels_known;
                });
            });
    }
    monitor_->set_spectrometer_ready(ready);
    measurement_->on_device_status_changed();
    if (!ready) spectrometer->resetChannels();
    devices_.update_spectrometer_status(ready, ready ? "" : "光谱仪通道不可用，正在重试");
    std::lock_guard<std::mutex> lock(readings_mutex_);
    readings_.set("spectrometer_channels", channels ? spectrum_channels(channels.value()) : json::array());
}

}  // namespace business::app
