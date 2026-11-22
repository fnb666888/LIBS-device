#pragma once

// 进程生命周期与依赖装配（`business/app/`）。
//
// ── Runtime 只做四件事，**不含任何业务逻辑** ─────────────────────────────
//
//   1. 按顺序构造/析构 `shared -> readiness -> measurement`
//   2. 注入依赖
//   3. 提供生命周期（`start()` / `stop()`）
//   4. 转发命令（`facade.cpp`）
//
// **本文件里不允许出现任何判定或出光决策。**
//
// 具体到代码上，这条纪律长这样：
//
//   * 判定只有一处：`readiness::ReadinessMonitor::evaluate_now()`。
//     Runtime 构造它并注入事实源（读激光 B2），但**从不自己解释结论**。
//   * 出光只有一处：`measurement::MeasurementJob`。
//     Runtime 不碰激光器写指令，只在 stop() 里调 job->stop() 收光。
//
// 依赖方向：`app -> {readiness, measurement} -> {shared, devices} -> libs`。
// 反向没有任何引用。设备对象只由 session 持有，别处拿指针。

#include <memory>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "libs/business/app/parameter_service.hpp"
#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/job.hpp"
#include "libs/business/measurement/prediction/service.hpp"
#include "libs/business/measurement/writer.hpp"
#include "libs/business/readiness/monitor.hpp"
#include "libs/business/shared/config/parameter_store.hpp"
#include "libs/business/shared/config/site_config.hpp"
#include "libs/business/shared/data_delivery.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/business/shared/logging/run_log.hpp"
#include "libs/business/shared/session/device_session.hpp"
#include "libs/error.hpp"

namespace business::app {

// HealthReport -> JSON。放在 Runtime 里是为了让 IPC 层与日志用同一份表达。
json::Value to_json(const session::HealthReport& report);

// 一次运行的全部依赖。持有设备会话与所有业务对象。
class Runtime {
public:
    struct Options {
        std::string data_dir = "data";            // 原始光谱、日志、恢复快照的根目录
        std::string log_file;                      // 空则用 data_dir/run.jsonl
        std::string parameters_file;               // 空则用 data_dir/parameters.json
        std::string captures_dir;                  // 空则用 data_dir/captures
        std::string recovery_dir;                  // 空则用 data_dir/recovery
        bool open_devices = true;                  // 离线演示/测试可关掉，只跑纯软件能力
        std::string site_config_file;
    };

    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // 按配置与选项启动。**部分失败允许**：设备没全开也能起，
    // 由 health() 如实报告（能力 2 的纪律）。
    libs::VoidResult start(const config::SiteConfig& config, const Options& options);

    // 优雅关闭。可重复调用。
    libs::VoidResult stop();

    bool running() const noexcept { return running_; }

    // 每根控制线都要串行化：设备是独占的，且驱动是同步阻塞的。
    // 多客户端并发时靠这把锁避免交错事务。
    //
    // **锁留在 Runtime**：串口互斥是**进程级**的，不属于任何一个业务。
    std::mutex& device_mutex() noexcept { return device_mutex_; }
    std::mutex& auxiliary_device_mutex() noexcept { return auxiliary_device_mutex_; }
    std::mutex& acquisition_mutex() noexcept { return acquisition_mutex_; }
    std::mutex& spectrometer_poll_mutex() noexcept { return spectrometer_poll_mutex_; }
    void request_shutdown() noexcept { shutdown_requested_.store(true); }
    bool shutdown_requested() const noexcept { return shutdown_requested_.load(); }

    const config::SiteConfig& config() const noexcept { return config_; }
    const std::string& site_config_file() const noexcept { return options_.site_config_file; }
    session::DeviceSession& devices() noexcept { return devices_; }
    session::HealthReport health() const { return devices_.health(); }
    json::Value health_json() const;
    json::Value device_readings() const;
    json::Value indicator_status() const;
    libs::VoidResult test_indicator(const std::map<libs::indicator::Channel, libs::indicator::State>& states,
                                    unsigned duration_ms);
    json::Value motor_status() const;
    libs::Result<json::Value> motor_jog_profile();
    libs::Result<json::Value> set_motor_jog_frequency(unsigned tenths_hz);
    libs::Result<json::Value> motor_command(const std::string& action, std::optional<unsigned> duration_ms = std::nullopt);
    json::Value sample_rangefinder();
    libs::Result<measurement::MeasurementStartRecord> start_measurement(const measurement::FireParameters& params);
    config::ParameterStore* parameters() noexcept { return parameters_.get(); }

    // 业务一：就绪判定。**未构造（start 未跑）时返回 nullptr**，调用方必须判空。
    //
    // UI 只读 `monitor()->latest()`，**不自己判**。UI 收到"开始检测"的成功
    // 就当成"已开光"，失败原因看返回码；它绝不能"先问门禁、拿到绿灯
    // 再自己决定要不要开始" —— 那会让判定出现第二个执行点。
    readiness::ReadinessMonitor* monitor() noexcept { return monitor_.get(); }
    const readiness::ReadinessMonitor* monitor() const noexcept { return monitor_.get(); }

    // 业务二：测量作业。**唯一持有激光器 + 光谱仪的业务对象**。
    //
    // 用 unique_ptr 且**延迟到 start() 构造**，原因很实在：它的依赖
    // （激光器句柄、光谱仪句柄、落盘 writer、门禁）全都是 start() 里
    // 才拿到的东西。在成员初始化列表里构造它，就只能传一堆 nullptr，
    // 然后在 start() 里补绑 —— 与其那样，不如等齐了再一次性造好。
    measurement::MeasurementJob* measurement() noexcept { return measurement_.get(); }
    const measurement::MeasurementJob* measurement() const noexcept { return measurement_.get(); }

    // 落盘子模块。测量作业调用它，Runtime 只是持有。
    measurement::Writer* writer() noexcept { return writer_.get(); }
    measurement::prediction::Service* prediction() noexcept { return prediction_.get(); }
    const measurement::prediction::Service* prediction() const noexcept { return prediction_.get(); }

    // 帧出口。**纯内存通道，不碰设备、不起线程**，
    // 所以它可以当普通成员直接构造，不需要延迟到 start()。
    //
    // 所有权在 Runtime（不是 MeasurementJob）：UI 必须在 measurement_start
    // 之前就能订阅，作业起不来的时候 UI 也要能看到"通道是空的"这件事。
    measurement::FrameQueue& frame_bus() noexcept { return frame_bus_; }

    // 出光参数管理服务：**测量参数的唯一来源**。
    // 界面改参数走 ParameterStore，出光读参数走 fire_parameters()，
    // 两条路径共用同一个 store —— 所以不存在"改了不生效"。
    ParameterService& parameter_service() noexcept { return parameters_service_; }
    logging::RunLog& log() noexcept { return *log_; }
    logging::RecoveryContextStore& recovery() noexcept { return *recovery_; }

    // 查询统一测量作业状态。独立采集也遵守同一个占用状态。
    bool laser_session_active() const { return measurement_ && measurement_->running(); }
    // 兼容字段：UI 的 status 面板读 laser_running。语义 = "是否有出光会话在进行"。
    bool laser_running() const { return laser_session_active(); }

    ::business::data_delivery::DeliverySink& local_sink() noexcept { return *local_sink_; }

    // 最近一次采集的交付状态（能力 6.3.1 双字段）。
    const std::optional<data_delivery::DeliveryStatus>& last_delivery() const noexcept { return last_delivery_; }
    void set_last_delivery(data_delivery::DeliveryStatus status);

private:
    enum class AuxiliaryDevice { indicator, rangefinder, air_conditioner };
    void start_device_monitoring();
    void stop_device_monitoring();
    void start_sampler(std::function<void()> sample, unsigned interval_ms);
    void run_laser_sampler();
    void request_laser_sample();
    void sample_laser();
    void collect_gate_facts(readiness::ReadinessFacts& facts);
    void sample_indicator();
    void sample_air_conditioner();
    void sample_spectrometer();
    void sample_motor();
    void sample_motor_locked();
    void publish_motor(const libs::Result<libs::devices::Su300Status>& status);
    void log_motor_jog(const std::string& message, const libs::Result<libs::devices::Su300Status>& status,
                       unsigned duration_ms = 0);
    libs::VoidResult arm_motor_watchdog(libs::devices::Su300& drive);
    libs::VoidResult restore_motor_watchdog(libs::devices::Su300& drive);
    libs::VoidResult stop_motor_locked(libs::devices::Su300& drive);
    libs::VoidResult stop_motor_on_exit();
    bool motor_limit(const libs::devices::Su300Status& status, libs::devices::Su300Direction direction) const;
    void publish_auxiliary(AuxiliaryDevice device, readiness::DeviceCondition condition,
                           std::string detail, std::vector<std::string> warnings = {});

    bool running_ = false;
    config::SiteConfig config_;
    Options options_;
    std::mutex device_mutex_;
    std::mutex auxiliary_device_mutex_;
    std::mutex indicator_sample_mutex_;
    std::mutex rangefinder_sample_mutex_;
    std::mutex acquisition_mutex_;
    std::mutex spectrometer_poll_mutex_;
    std::mutex motor_mutex_;
    bool measurement_starting_ = false;
    std::optional<libs::devices::Su300Direction> motor_jog_;
    std::chrono::steady_clock::time_point motor_deadline_{};
    std::chrono::steady_clock::time_point motor_feedback_deadline_{};
    bool motor_jog_confirmed_ = false;
    std::string motor_jog_error_;
    std::chrono::steady_clock::time_point motor_watchdog_quiet_until_{};
    std::optional<std::uint16_t> motor_watchdog_restore_;
    bool motor_watchdog_armed_ = false;
    bool motor_frequency_unverified_ = false;
    bool motor_stop_pending_ = false;
    bool motor_stop_retry_ = false;
    std::atomic<bool> head_stopped_{false};
    std::atomic<bool> shutdown_requested_{false};

    // ── shared：共同底座 ────────────────────────────────────────────────
    session::DeviceSession devices_;
    std::unique_ptr<config::ParameterStore> parameters_;
    std::unique_ptr<logging::RunLog> log_;
    // 说明：日志/恢复仓库用 unique_ptr 持有，因为它们不可拷贝（内部有互斥量/文件句柄），
    // 而 Runtime 需要在 start() 里按实际路径重新构造。
    std::unique_ptr<logging::RecoveryContextStore> recovery_;
    std::unique_ptr<data_delivery::DeliverySink> local_sink_;

    // ── app：参数搬运 ───────────────────────────────────────────────────
    // 测量参数的唯一来源。必须**声明在 measurement_ 之前**：
    // measurement_ 每帧都要向它要参数，顺序反了拿到的还是一个空壳。
    ParameterService parameters_service_{*this};

    // ── readiness：业务一 ───────────────────────────────────────────────
    //
    std::unique_ptr<readiness::ReadinessMonitor> monitor_;
    mutable std::mutex readings_mutex_;
    readiness::LaserSafetyFacts laser_safety_;
    bool laser_opened_ = false;
    json::Value readings_ = json::object();
    json::Value indicator_status_;
    std::optional<readiness::MachineState> applied_lamp_state_;
    std::array<readiness::AuxiliaryDeviceFacts, 3> auxiliary_;
    std::array<std::uint64_t, 3> auxiliary_fresh_until_ms_{};
    std::mutex sampler_mutex_;
    std::condition_variable sampler_wake_;
    bool samplers_stopping_ = false;
    bool laser_sample_requested_ = false;
    std::vector<std::thread> samplers_;

    // ── measurement：业务二 ─────────────────────────────────────────────
    //
    // 帧出口当普通成员构造（零设备依赖，UI 要能在作业起之前就订阅）。
    measurement::FrameQueue frame_bus_;

    // **析构顺序**：成员按声明逆序析构，所以 measurement_ 会先于
    // monitor_ / writer_ / local_sink_ 析构 —— 它停线程时要往 writer 里
    // 写最后几帧，而 writer 还要往 local_sink_ 写。顺序反了 =
    // 作业还在 push 帧，下游已经被销毁。
    std::unique_ptr<measurement::Writer> writer_;
    std::unique_ptr<measurement::MeasurementJob> measurement_;
    std::unique_ptr<measurement::prediction::Service> prediction_;

    std::optional<data_delivery::DeliveryStatus> last_delivery_;

};

}  // namespace business::app
