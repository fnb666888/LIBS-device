#pragma once

// 设备独占会话与生命周期（能力 2）。
//
// 它是整个业务层的**地基**——把"设备独占"这条物理约束变成一条可测试的代码
// 约束：serial.cpp share=0 独占打开；AvaSpec 命名互斥体；全库同步阻塞。
// 因此本类是**进程内唯一**持有设备对象的地方，业务层其他地方只拿引用。
//
// 生命周期：open() -> 使用 -> close()。析构只是兜底，正常路径必须显式 close()。

#include <memory>
#include <string>

#include "libs/business/shared/config/site_config.hpp"
#include "libs/devices/indicator.hpp"
#include "libs/devices/laser.hpp"
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
    bool all_required_open = true;
    std::string summary;
    DeviceState laser;
    DeviceState indicator;
    DeviceState spectrometer;
};

// 进程内唯一的设备持有者。不可拷贝、不可移动。
//
// 本项目只持有三类硬件：**激光器 / 状态灯 / 光谱仪**。
// 测距仪与空调曾在这里被打开并写进 health 回报，但全库没有任何业务读过它们
// （空调联锁阈值始终未标定、门禁侧的判定条件已一并删除），属于纯开销，已移除。
class DeviceSession {
public:
    DeviceSession() = default;
    ~DeviceSession();
    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;

    // 按配置打开全部 enabled 的设备。
    //
    // 部分成功是允许的且**如实报告**：某台设备没接不应该让整台机器起不来。
    // 但 required 的设备（配置里 enabled=true 的串口设备）没打开时，
    // all_required_open 为 false，由调用方决定是否继续。
    // 绝不做做的事：静默回落、换端口重试、把失败吞掉。
    libs::VoidResult open(const config::SiteConfig& config);

    // 优雅关闭。顺序与打开相反：先关光谱仪（SDK 需要显式清理），
    // 再关串口。可重复调用。
    libs::VoidResult close();

    bool is_open() const noexcept { return open_; }

    HealthReport health() const;

    // 设备访问器。未打开时返回 nullptr——调用方必须自己判空，
    // 不提供"自动打开"的隐式行为。
    libs::devices::Laser* laser() noexcept { return laser_.get(); }
    libs::devices::Indicator* indicator() noexcept { return indicator_.get(); }
    libs::devices::Spectrometer* spectrometer() noexcept { return spectrometer_.get(); }

    const config::SiteConfig& config() const noexcept { return config_; }

private:
    void reset();

    bool open_ = false;
    config::SiteConfig config_;

    // 传输层句柄。驱动对象引用它们，所以链接对象必须比驱动活得久：
    // 声明顺序即析构逆序，links_ 在最后声明 -> 最先析构，这是想要的。
    std::vector<std::unique_ptr<libs::transport::Link>> links_;
    std::unique_ptr<libs::devices::Laser> laser_;
    std::unique_ptr<libs::devices::Indicator> indicator_;
    std::unique_ptr<libs::devices::Spectrometer> spectrometer_;

    DeviceState laser_state_;
    DeviceState indicator_state_;
    DeviceState spectrometer_state_;
};

}  // namespace business::session