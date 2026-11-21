#pragma once

// 设备独占会话与生命周期（能力 2）。
//
// 它是整个业务层的**地基**——把"设备独占"这条物理约束变成一条可测试的代码
// 约束：serial.cpp share=0 独占打开；AvaSpec 命名互斥体；全库同步阻塞。
// 因此本类是**进程内唯一**持有设备对象的地方，业务层其他地方只拿引用。
//
// 生命周期：open() -> 使用 -> close()。析构只是兜底，正常路径必须显式 close()。

#include <memory>
#include <mutex>
#include <string>

#include "libs/business/shared/config/site_config.hpp"
#include "libs/devices/air_conditioner.hpp"
#include "libs/devices/indicator.hpp"
#include "libs/devices/laser.hpp"
#include "libs/devices/rangefinder.hpp"
#include "libs/devices/spectrometer.hpp"
#include "libs/error.hpp"
#include "libs/transport/serial.hpp"

namespace business::session {

// 单台设备在本次运行中的状态。enabled=false 的设备是"配置上就没接"，
// 不是"打开失败"——这两个语义必须分开，否则调用方会把配置问题当成故障。
struct DeviceState {
    bool configured = false;   // 配置里 enabled=true
    bool opened = false;       // 真的拿到了句柄
    std::string endpoint;      // 驱动报告的端口/标识
    std::string model;         // 光谱仪 SDK 型号等
    std::string note;          // 失败原因（中文，直接可写日志）
};

// 会话健康检查结果。全部设备汇总，不含任何门禁判断——那是能力 4 的事。
struct HealthReport {
    // 已启用的激光器和光谱仪是否已打开。
    bool all_required_open = true;
    std::string summary;
    DeviceState laser;
    DeviceState indicator;
    DeviceState spectrometer;
    DeviceState rangefinder;
    DeviceState air_conditioner;
};

// 进程内唯一的设备持有者。不可拷贝、不可移动。
class DeviceSession {
public:
    enum class Device { laser, indicator, rangefinder, air_conditioner, spectrometer };
    DeviceSession() = default;
    ~DeviceSession();
    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;

    // 按配置打开全部 enabled 的设备。
    //
    // 部分成功是允许的且**如实报告**：某台设备没接不应该让整台机器起不来。
    // 已启用的主测量设备打开失败时，all_required_open 为 false。
    // 绝不做做的事：静默回落、换端口重试、把失败吞掉。
    libs::VoidResult open(const config::SiteConfig& config);
    // Retry the configured endpoint of a device that has no handle yet.
    // Each endpoint is retried by its own sampler. Successful handles stay stable.
    void retry_open(Device device);
    void update_spectrometer_status(bool opened, std::string detail);

    // 优雅关闭。顺序与打开相反：先关光谱仪（SDK 需要显式清理），
    // 再关串口。可重复调用。
    libs::VoidResult close();

    bool is_open() const noexcept { return open_; }

    HealthReport health() const;

    libs::devices::Laser* laser();
    libs::devices::Indicator* indicator();
    libs::devices::Spectrometer* spectrometer();
    libs::devices::Rangefinder* rangefinder();
    libs::devices::AirConditioner* air_conditioner();
    const std::vector<libs::devices::SpectrometerInfo>& spectrometer_channels() const noexcept {
        return spectrometer_channels_;
    }

    const config::SiteConfig& config() const noexcept { return config_; }

private:
    template <class T, class Make>
    void open_serial_device(const config::SerialPortConfig& config, const char* name,
                            std::unique_ptr<T>& device, DeviceState& state, Make make);
    void open_spectrometer();

    bool open_ = false;
    config::SiteConfig config_;
    mutable std::mutex devices_mutex_;

    // 传输层句柄。驱动对象引用它们，所以链接对象必须比驱动活得久：
    // links_ 先声明，因此析构时驱动先于链路销毁。
    std::vector<std::unique_ptr<libs::transport::Link>> links_;
    std::unique_ptr<libs::devices::Laser> laser_;
    std::unique_ptr<libs::devices::Indicator> indicator_;
    std::unique_ptr<libs::devices::Spectrometer> spectrometer_;
    std::unique_ptr<libs::devices::Rangefinder> rangefinder_;
    std::unique_ptr<libs::devices::AirConditioner> air_conditioner_;
    std::vector<libs::devices::SpectrometerInfo> spectrometer_channels_;

    DeviceState laser_state_;
    DeviceState indicator_state_;
    DeviceState spectrometer_state_;
    DeviceState rangefinder_state_;
    DeviceState air_conditioner_state_;
};

}  // namespace business::session
