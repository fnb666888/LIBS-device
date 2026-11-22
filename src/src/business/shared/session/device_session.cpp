#include "libs/business/shared/session/device_session.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace business::session {
namespace {

using libs::Error;
using libs::ErrorKind;
using libs::Result;
using libs::VoidResult;

std::string describe(const libs::Error& e) {
    std::ostringstream os;
    os << libs::to_string(e.kind) << "/" << e.code << ": " << e.message;
    return os.str();
}

}  // namespace

template <class T, class Make>
void DeviceSession::open_serial_device(const config::SerialPortConfig& cfg, const char* name,
                                       std::unique_ptr<T>& device, DeviceState& state, Make make) {
    DeviceState next;
    if (!cfg.enabled) {
        next.note = "配置未启用（enabled=false）";
    } else {
        next.configured = true;
        next.endpoint = cfg.port;
        auto link = libs::transport::open_serial(cfg.port, cfg.baud, cfg.parity, cfg.data_bits);
        if (link) {
            auto device_to_publish = make(*link.value());
            next.opened = true;
            next.endpoint = link.value()->endpoint();
            std::lock_guard<std::mutex> lock(devices_mutex_);
            links_.push_back(std::move(link.value()));
            device = std::move(device_to_publish);
            state = std::move(next);
            return;
        }
        next.note = std::string(name) + " 打开失败：" + describe(link.error());
    }
    std::lock_guard<std::mutex> lock(devices_mutex_);
    state = std::move(next);
}

DeviceSession::~DeviceSession() {
    // 兜底。正常路径是显式 close()；这里只保证不泄漏句柄。
    if (open_) {
        const VoidResult r = close();
        (void)r;   // 析构里不能报错，错误在显式 close() 时已经报过了
    }
}

VoidResult DeviceSession::open(const config::SiteConfig& config) {
    if (open_) {
        return Error::make(ErrorKind::invalid_argument, "already_open",
                           "this session already holds the devices; call close() first");
    }

    // 配置先校验：打开串口之前就知道这份配置是不是自洽的。
    const VoidResult valid = config::validate(config);
    if (!valid) return valid.error();

    config_ = config;

    open_ = true;
    for (auto device : {Device::laser, Device::indicator, Device::rangefinder,
                        Device::air_conditioner, Device::spectrometer, Device::motor}) {
        retry_open(device);
    }
    return libs::VoidResult();
}

void DeviceSession::retry_open(Device device) {
    if (!open_) return;
    switch (device) {
        case Device::laser:
            if (!laser()) open_serial_device(config_.laser, "激光器", laser_, laser_state_, [&](auto& link) {
                return std::make_unique<libs::devices::Laser>(link, config_.laser.timeout_ms);
            });
            break;
        case Device::indicator:
            if (!indicator()) open_serial_device(config_.indicator, "状态灯", indicator_, indicator_state_, [&](auto& link) {
                const unsigned gap = std::max(config_.indicator.idle_ms, 50u);
                return std::make_unique<libs::devices::Indicator>(
                    link, config_.indicator_address, config_.indicator_model, config_.indicator.timeout_ms, gap);
            });
            break;
        case Device::rangefinder:
            if (!rangefinder()) {
                const auto& range = config_.rangefinder;
                open_serial_device(range.serial, "测距仪", rangefinder_, rangefinder_state_, [&](auto& link) {
                    return std::make_unique<libs::devices::Rangefinder>(link, range.address, range.serial.timeout_ms);
                });
            }
            break;
        case Device::air_conditioner:
            if (!air_conditioner()) {
                const auto& air = config_.air_conditioner;
                open_serial_device(air.serial, "空调", air_conditioner_, air_conditioner_state_, [&](auto& link) {
                    return std::make_unique<libs::devices::AirConditioner>(link, air.address, air.serial.timeout_ms);
                });
            }
            break;
        case Device::spectrometer:
            if (!spectrometer()) open_spectrometer();
            break;
        case Device::motor:
            if (!motor()) {
                const auto& motor_config = config_.motor.connection;
                open_serial_device(motor_config.serial, "SU300", motor_, motor_state_, [&](auto& link) {
                    return std::make_unique<libs::devices::Su300>(link, motor_config.address, motor_config.serial.timeout_ms);
                });
            }
            break;
    }
}

void DeviceSession::open_spectrometer() {
    DeviceState state;
    std::unique_ptr<libs::devices::Spectrometer> device;
    std::vector<libs::devices::SpectrometerInfo> channels;
    if (!config_.spectrometer.enabled) {
        state.note = "配置未启用（enabled=false）";
    } else {
        state.configured = true;
        state.endpoint = config_.spectrometer.sdk_dll;
        auto opened = libs::devices::Spectrometer::open(state.endpoint);
        if (!opened) {
            state.note = "光谱仪打开失败：" + describe(opened.error());
        } else {
            auto probed = opened.value()->probeChannels();
            if (!probed) {
                state.note = "光谱仪已打开但枚举失败：" + describe(probed.error());
            } else {
                channels = std::move(probed.value());
                const auto usable = [](const auto& channel) { return channel.pixels_known; };
                const auto first = std::find_if(channels.begin(), channels.end(), usable);
                const auto& selected = config_.spectrometer.serials;
                const bool selected_ready = std::all_of(selected.begin(), selected.end(), [&](const auto& serial) {
                    return std::any_of(channels.begin(), channels.end(), [&](const auto& channel) {
                        return channel.serial == serial && usable(channel);
                    });
                });
                if (first == channels.end() || !selected_ready) {
                    state.note = "没有可用或缺少已配置的光谱仪通道";
                } else {
                    state.opened = true;
                    state.model = first->friendly_name;
                }
            }
            device = std::move(opened.value());
        }
    }
    std::lock_guard<std::mutex> lock(devices_mutex_);
    spectrometer_channels_ = std::move(channels);
    spectrometer_state_ = std::move(state);
    spectrometer_ = std::move(device);
}

libs::devices::Laser* DeviceSession::laser() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return laser_.get();
}

libs::devices::Indicator* DeviceSession::indicator() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return indicator_.get();
}

libs::devices::Spectrometer* DeviceSession::spectrometer() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return spectrometer_.get();
}

void DeviceSession::update_spectrometer_status(bool opened, std::string detail) {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    spectrometer_state_.opened = opened;
    spectrometer_state_.note = std::move(detail);
}

libs::devices::Rangefinder* DeviceSession::rangefinder() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return rangefinder_.get();
}

libs::devices::AirConditioner* DeviceSession::air_conditioner() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return air_conditioner_.get();
}

libs::devices::Su300* DeviceSession::motor() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return motor_.get();
}

VoidResult DeviceSession::close() {
    std::string first_error;

    // 光谱仪必须最先关：SDK 需要显式停止测量并清空脉冲配置。
    if (spectrometer_) {
        const VoidResult r = spectrometer_->close();
        if (!r && first_error.empty()) first_error = describe(r.error());
        spectrometer_.reset();
    }

    // 串口设备：驱动对象先析构（它们持有 Link 的引用），再关 Link。
    laser_.reset();
    indicator_.reset();
    rangefinder_.reset();
    air_conditioner_.reset();
    motor_.reset();
    spectrometer_channels_.clear();
    links_.clear();
    for (auto* state : {&laser_state_, &indicator_state_, &spectrometer_state_,
                        &rangefinder_state_, &air_conditioner_state_, &motor_state_}) {
        if (state->opened) {
            state->opened = false;
            state->note = "会话已关闭";
        }
    }

    open_ = false;

    if (!first_error.empty()) {
        return Error::make(ErrorKind::device, "shutdown_incomplete", first_error);
    }
    return libs::VoidResult();
}

HealthReport DeviceSession::health() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    HealthReport report;
    report.laser = laser_state_;
    report.indicator = indicator_state_;
    report.spectrometer = spectrometer_state_;
    report.rangefinder = rangefinder_state_;
    report.air_conditioner = air_conditioner_state_;
    report.motor = motor_state_;

    std::ostringstream os;
    bool all = true;
    const auto account = [&](const char* name, const DeviceState& s, bool required) {
        std::ostringstream line;
        if (!s.configured) {
            line << name << ": 未启用";
        } else if (s.opened) {
            line << name << ": 已打开 " << s.endpoint;
        } else {
            line << name << ": 失败 " << s.note;
            if (required) all = false;
        }
        if (!line.str().empty()) {
            if (os.tellp() > 0) os << "; ";
            os << line.str();
        }
    };

    account("激光器", laser_state_, true);
    account("状态灯", indicator_state_, false);
    account("光谱仪", spectrometer_state_, true);
    account("测距仪", rangefinder_state_, false);
    account("空调", air_conditioner_state_, false);
    account("检测头移位", motor_state_, false);

    report.all_required_open = all;
    report.summary = os.str();
    return report;
}

}  // namespace business::session
