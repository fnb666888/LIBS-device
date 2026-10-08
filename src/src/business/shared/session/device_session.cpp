#include "libs/business/shared/session/device_session.hpp"

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

// 打开一个串口并把设备包起来。失败时只记录，不抛出——部分成功是允许的。
template <class Make>
bool open_serial_device(const config::SerialPortConfig& cfg, const char* name,
                        std::vector<std::unique_ptr<libs::transport::Link>>& links,
                        Make make, DeviceState& state) {
    if (!cfg.enabled) {
        state = DeviceState{false, false, "", "", std::string("配置未启用（enabled=false）")};
        return false;
    }

    // configured 的含义是"配置里 enabled=true"，与打开成败无关。
    // 漏掉这一行会让**打开成功**的设备在 health 里被报成"未启用"。
    state.configured = true;

    Result<std::unique_ptr<libs::transport::Link>> link =
        libs::transport::open_serial(cfg.port, cfg.baud, cfg.parity, cfg.data_bits);
    if (!link) {
        // 设备打不开是现场事实，不是编程错误：记下来，如实上报。
        state = DeviceState{true, false, cfg.port, "", std::string(name) + " 打开失败：" + describe(link.error())};
        return false;
    }

    state.endpoint = link.value()->endpoint();
    links.push_back(std::move(link.value()));
    make(links.back().get());
    state.opened = true;
    return true;
}

}  // namespace

DeviceSession::~DeviceSession() {
    // 兜底。正常路径是显式 close()；这里只保证不泄漏句柄。
    if (open_) {
        const VoidResult r = close();
        (void)r;   // 析构里不能报错，错误在显式 close() 时已经报过了
    }
}

void DeviceSession::reset() {
    laser_.reset();
    indicator_.reset();
    spectrometer_.reset();
    links_.clear();
    open_ = false;
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

    open_serial_device(config_.laser, "激光器", links_,
                       [&](libs::transport::Link* link) {
                           laser_ = std::make_unique<libs::devices::Laser>(*link, config_.laser.timeout_ms);
                       },
                       laser_state_);

    open_serial_device(config_.indicator, "状态灯", links_,
                       [&](libs::transport::Link* link) {
                           // 写命令需要相邻命令间隔；用配置里的 idle_ms 作为下限。
                           const unsigned gap = config_.indicator.idle_ms < 50 ? 50 : config_.indicator.idle_ms;
                           indicator_ = std::make_unique<libs::devices::Indicator>(
                               *link, config_.indicator_address, config_.indicator_model,
                               config_.indicator.timeout_ms, gap);
                       },
                       indicator_state_);

    // 光谱仪走厂商 DLL，不走串口。命名互斥体由驱动自己管。
    if (!config_.spectrometer.enabled) {
        spectrometer_state_ = DeviceState{false, false, "", "", "配置未启用（enabled=false）"};
    } else {
        Result<std::unique_ptr<libs::devices::Spectrometer>> spec =
            libs::devices::Spectrometer::open(config_.spectrometer.sdk_dll);
        if (!spec) {
            spectrometer_state_ =
                DeviceState{true, false, config_.spectrometer.sdk_dll, "",
                            "光谱仪打开失败：" + describe(spec.error())};
        } else {
            spectrometer_ = std::move(spec.value());
            spectrometer_state_ = DeviceState{true, true, config_.spectrometer.sdk_dll, "", ""};

            // 枚举通道。这是只读操作，不 prepare、不 measure、不改持久参数。
            const Result<std::vector<libs::devices::SpectrometerInfo>> devices =
                spectrometer_->probeChannels();
            if (!devices) {
                spectrometer_state_.opened = false;
                spectrometer_state_.note = "光谱仪已打开但枚举失败：" + describe(devices.error());
                spectrometer_.reset();
            } else {
                for (const libs::devices::SpectrometerInfo& d : devices.value()) {
                    if (spectrometer_state_.model.empty()) spectrometer_state_.model = d.friendly_name;
                }
            }
        }
    }

    // 会话已建立（哪怕部分设备没开）。是否可继续工作由 health() 告诉调用方。
    open_ = true;
    return libs::VoidResult();
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
    links_.clear();

    open_ = false;

    if (!first_error.empty()) {
        return Error::make(ErrorKind::device, "shutdown_incomplete", first_error);
    }
    return libs::VoidResult();
}

HealthReport DeviceSession::health() const {
    HealthReport report;
    report.laser = laser_state_;
    report.indicator = indicator_state_;
    report.spectrometer = spectrometer_state_;

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

    // required 的判定：配置里 enabled=true 就是 required。
    // 这就是 all_required_open 的含义——"你让我接的，我有没有接上"。
    account("激光器", laser_state_, true);
    account("状态灯", indicator_state_, true);
    account("光谱仪", spectrometer_state_, true);

    report.all_required_open = all;
    report.summary = os.str();
    return report;
}

}  // namespace business::session