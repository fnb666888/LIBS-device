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
//   * 出光会话记账（`begin_laser_session`）是**进程级资源互斥**，
//     不是业务逻辑 —— 它拦的只是"重复出光"，不仲裁设备。
//
// 依赖方向：`app -> {readiness, measurement} -> {shared, devices} -> libs`。
// 反向没有任何引用。设备对象只由 session 持有，别处拿指针。

#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

#include "libs/business/app/parameter_service.hpp"
#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/job.hpp"
#include "libs/business/measurement/prediction/service.hpp"
#include "libs/business/measurement/writer.hpp"
#include "libs/business/readiness/monitor.hpp"
#include "libs/business/readiness/precheck.hpp"
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
    std::mutex& acquisition_mutex() noexcept { return acquisition_mutex_; }
    void request_shutdown() noexcept { shutdown_requested_.store(true); }
    bool shutdown_requested() const noexcept { return shutdown_requested_.load(); }

    const config::SiteConfig& config() const noexcept { return config_; }
    session::DeviceSession& devices() noexcept { return devices_; }
    session::HealthReport health() const { return devices_.health(); }
    json::Value health_json() const;
    config::ParameterStore* parameters() noexcept { return parameters_.get(); }

    // 业务一：就绪判定。**未构造（start 未跑）时返回 nullptr**，调用方必须判空。
    //
    // UI 只读 `monitor()->latest()`，**不自己判**。UI 收到"开始检测"的成功
    // 就当成"已开光"，失败原因看返回码；它绝不能"先问门禁、拿到绿灯
    // 再自己决定要不要开始" —— 那会让判定出现第二个执行点。
    readiness::ReadinessMonitor* monitor() noexcept { return monitor_.get(); }
    const readiness::ReadinessMonitor* monitor() const noexcept { return monitor_.get(); }

    // 门禁结论 -> 状态灯。**全库唯一的灯下发点**。
    readiness::PrecheckService* precheck() noexcept { return precheck_.get(); }

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

    // ── 出光会话互斥（进程级资源记账）────────────────────────────────────
    //
    // **它是什么**：一个布尔 + 一把锁，回答"这台激光器当前有没有出光会话"。
    // 原先需要它是因为有两条能发出光的路径；现在只剩 `measurement_start`
    // 一条，但**记账仍然保留** —— 因为"重复出光"是必须拦的，
    // 而这个拦截点应该独立于具体走哪条命令。
    //
    // **它不是什么**：不是设备仲裁。早年那次要仲裁（`AcquisitionLease`）
    // 是因为同时存在两个采集者；现在物理上只有一个采集者。
    //
    // 调用方约定：**无论是哪条路径**，调用方应当原地拒绝，不要覆盖。
    bool begin_laser_session();
    void end_laser_session() noexcept;
    bool laser_session_active() const;
    // 兼容字段：UI 的 status 面板读 laser_running。语义 = "是否有出光会话在进行"。
    bool laser_running() const { return laser_session_active(); }

    // 按当前判定结论刷新一次状态灯。
    //
    // 灯显示的是判定结论，所以顺序是：先 evaluate 得状态 -> 再映射成灯色下发。
    // 真正刷灯的实现在 PrecheckService —— 全库唯一的灯下发点。
    //
    // 灯写失败**不抛**、不改判定：状态灯是辅助指示，不是判据。
    libs::VoidResult sync_indicator();

    ::business::data_delivery::DeliverySink& local_sink() noexcept { return *local_sink_; }

    // 最近一次采集的交付状态（能力 6.3.1 双字段）。
    const std::optional<data_delivery::DeliveryStatus>& last_delivery() const noexcept { return last_delivery_; }
    void set_last_delivery(data_delivery::DeliveryStatus status);

private:
    bool running_ = false;
    config::SiteConfig config_;
    Options options_;
    std::mutex device_mutex_;
    std::mutex acquisition_mutex_;
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
    // monitor_ 与 precheck_ 延迟构造：它们的事实源要 devices_ 打开之后
    // 才拿得到激光器句柄。事实源由 start() 装成一个闭包，
    // **Runtime 不解释事实、也不解释结论** —— 它只负责把设备递进去。
    std::unique_ptr<readiness::ReadinessMonitor> monitor_;
    std::unique_ptr<readiness::PrecheckService> precheck_;

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

    // 出光会话记账。见上面的 begin_laser_session() 说明。
    mutable std::mutex laser_session_mutex_;
    bool laser_session_active_ = false;
};

}  // namespace business::app
