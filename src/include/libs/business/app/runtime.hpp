#pragma once

// 进程生命周期与依赖装配（`business/app/`）。
//
// 职责（范围报告 §5.1）：
//   * 打开设备会话（能力 2，独占打开）
//   * 构造各服务模块（能力 3-12）
//   * 串起门禁事实源（激光 B2 轮询 -> Gate::update_laser_facts）
//   * 退出时优雅关闭设备（先光谱仪，后串口）
//
// **依赖装配方向**：app -> services -> policy/session/config -> libs。
// 反向没有任何引用。设备对象只由 session 持有，别处拿引用。

#include <memory>
#include <optional>
#include <mutex>
#include <atomic>
#include <string>

#include "libs/business/config/site_config.hpp"
#include "libs/business/json/json.hpp"
#include "libs/business/logging/run_log.hpp"
#include "libs/business/policy/detection_config.hpp"
#include "libs/business/policy/gate.hpp"
#include "libs/business/services/ac_interlock.hpp"
#include "libs/business/services/business_event.hpp"
#include "libs/business/services/capture.hpp"
#include "libs/business/services/composition.hpp"
#include "libs/business/services/data_delivery.hpp"
#include "libs/business/services/environment.hpp"
#include "libs/business/services/laser_fire.hpp"
#include "libs/business/services/motion.hpp"
#include "libs/business/services/ore_presence.hpp"
#include "libs/business/services/spectrum_stream.hpp"
#include "libs/business/session/device_session.hpp"

namespace business::app {

// HealthReport -> JSON。放在 Runtime 里是为了让 IPC 层与日志用同一份表达。
json::Value to_json(const session::HealthReport& report);

// 一次运行的全部依赖。持有设备会话与所有服务对象。
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

    // 轮询一次激光 B2，把事实喂给门禁。**这是门禁唯一的激光事实来源。**
    // 读失败时把 available=false 喂进去——门禁据此拦截（读不到 != 安全）。
    libs::VoidResult poll_laser_facts();

    // 同上，但事实从**指定设备**读。默认等价于 poll_laser_facts()。
    // 分开是因为出光流程可能操作一个与默认句柄不同的 Laser 实例
    // （离线测试注入替身时就是如此）。事实必须读将要被写入的**同一个**设备，
    // 否则会出现“读 A 的安全状态、往 B 发出光指令”这种最危险的错配。
    libs::VoidResult poll_laser_facts(libs::devices::Laser* laser);

    // 每根控制线都要串行化：设备是独占的，且驱动是同步阻塞的。
    // 多客户端并发时靠这把锁避免交错事务。
    std::mutex& device_mutex() noexcept { return device_mutex_; }

    const config::SiteConfig& config() const noexcept { return config_; }
    session::DeviceSession& devices() noexcept { return devices_; }
    session::HealthReport health() const { return devices_.health(); }
    json::Value health_json() const;
    policy::Gate& gate() noexcept { return gate_; }
    policy::ParameterStore* parameters() noexcept { return parameters_.get(); }
    logging::RunLog& log() noexcept { return *log_; }
    logging::RecoveryContextStore& recovery() noexcept { return *recovery_; }
    services::OrePresenceJudge& ore_judge() noexcept { return ore_judge_; }
    services::AirConditionerMonitor& ac_monitor() noexcept { return ac_monitor_; }

    // 连续出光是否在跑。
    //
    // **必须由 Runtime 记着**：守护进程退出时要负责收光。关串口不会让激光
    // 停下来 —— 设备自己保持着"工作"状态，软件崩了、窗口关了，光还在打。
    void set_laser_running(bool running) noexcept { laser_running_ = running; }
    bool laser_running() const noexcept { return laser_running_; }

    // ── 优雅停机请求（管道命令 shutdown）──────────────────────────────────────
    //
    // **为什么必须有这条路**：stop() 要负责收光 —— 连续出光模式下激光正在出光时，
    // 直接杀掉进程不会让它停下来，设备自己保持着"工作"状态。
    // 而一旦守护进程从控制台窗口里解放出来（隐藏窗口启动，见 Fix E），
    // "关窗口触发 CTRL_CLOSE_EVENT"这条老路就没了，剩下的 `schtasks /End`
    // 是硬杀 TerminateProcess，**会跳过 stop()、把激光留在出光状态**。
    // 所以补一条协议内的停机请求：客户端说"请退出"，守护进程照常走完整收光流程。
    void request_shutdown() noexcept { shutdown_requested_.store(true); }
    bool shutdown_requested() const noexcept { return shutdown_requested_.load(); }

    // 辅助监测（空调 / 测距）上一次**真的去读设备**的时刻（单调钟毫秒）。
    //
    // 为什么需要节流：status 每调用一次就去读空调（2 个 Modbus 事务）+ 测距
    // （1 个），各带串口超时，实测把 status 从 8 ms 拖到 ~2 秒；而 UI 每 800 ms
    // 就轮询一次 status —— 于是 status 常年占着 device_mutex，把光谱采集和
    // 取帧一起饿死（现场表现：有帧号、但界面一直空）。
    //
    // 所以按节奏读：status 多数时候直接回缓存，读数的新鲜度用 aux_read_age_ms
    // 如实上报，让调用方自己判断"这条读数有多旧"。
    Milliseconds last_aux_read_ms() const noexcept { return last_aux_read_ms_; }
    void mark_aux_read(Milliseconds now_ms) noexcept { last_aux_read_ms_ = now_ms; }
    services::AirConditionerInterlock& ac_interlock() noexcept { return ac_interlock_; }
    services::CaptureCoordinator& capture() noexcept { return capture_; }
    // 实时光谱流（展示侧）。与其它服务一样由 Runtime 独占持有。
    services::SpectrumStream& spectrum_stream() noexcept { return spectrum_stream_; }
    // 出光服务。它反过来持有 Runtime 的引用，是有意为之：
    // 出光必须拿到设备句柄、门禁、采集协调三样，绕开它们就会重复实现判定。
    services::LaserFireService& laser_fire() noexcept { return laser_fire_; }
    services::CompositionAnalyzer& composition() noexcept { return composition_; }
    services::MotionTiming& motion_timing() noexcept { return motion_timing_; }
    services::PositionSwitch& position_switch() noexcept { return *position_switch_; }
    ::business::data_delivery::DeliverySink& local_sink() noexcept { return *local_sink_; }
    ::business::data_delivery::DeliverySink& gateway_sink() noexcept { return *gateway_sink_; }
    events::GatewayAdapter& gateway_adapter() noexcept { return gateway_adapter_; }
    events::FeedSegmentRules& feed_rules() noexcept { return feed_rules_; }
    const services::WorkFrequency& work_frequency() const noexcept { return work_frequency_; }
    void set_work_frequency(services::WorkFrequency f) { work_frequency_ = std::move(f); }

    // 有界电机动作的可中断标志。运动命令释放 device_mutex 等待时，
    // 另一个 IPC 请求可以先置位该标志并下发停机，不必等完整时长结束。
    void begin_motor_motion() noexcept { motor_stop_requested_.store(false); }
    void request_motor_stop() noexcept { motor_stop_requested_.store(true); }
    bool motor_stop_requested() const noexcept { return motor_stop_requested_.load(); }

    // 当前检测阶段（业务进度，不与设备状态绑定）。
    events::DetectionStage stage() const noexcept { return stage_; }
    void set_stage(events::DetectionStage stage);
    std::int64_t belt_sequence() const noexcept { return belt_sequence_; }
    void set_belt_sequence(std::int64_t seq);
    const std::string& target_belt() const noexcept { return target_belt_; }
    void set_target_belt(std::string belt);

    // 最近一次采集的交付状态（能力 6.3.1 双字段）。
    const std::optional<data_delivery::DeliveryStatus>& last_delivery() const noexcept { return last_delivery_; }
    void set_last_delivery(data_delivery::DeliveryStatus status);

private:
    void log_stage_change(events::DetectionStage from, events::DetectionStage to);

    bool running_ = false;
    std::atomic<bool> shutdown_requested_{false};
    config::SiteConfig config_;
    Options options_;
    std::mutex device_mutex_;

    session::DeviceSession devices_;
    policy::Gate gate_;
    std::unique_ptr<policy::ParameterStore> parameters_;
    std::unique_ptr<logging::RunLog> log_;
    // 说明：日志/恢复仓库用 unique_ptr 持有，因为它们不可拷贝（内部有互斥量/文件句柄），
    // 而 Runtime 需要在 start() 里按实际路径重新构造。
    std::unique_ptr<logging::RecoveryContextStore> recovery_;
    services::CaptureCoordinator capture_;
    // **device_mutex_ 必须声明在 spectrum_stream_ 之前**：C++ 成员按声明
    // 逆序析构，所以先析构 stream（停掉它的采集线程），再析构互斥量。
    // 顺序反了 = 采集线程还在跑，它再去 lock 一把已经没了的 mutex。
    services::SpectrumStream spectrum_stream_{device_mutex_};
    services::LaserFireService laser_fire_;
    services::CompositionAnalyzer composition_;
    services::OrePresenceJudge ore_judge_;
    services::AirConditionerMonitor ac_monitor_;
    bool laser_running_ = false;
    Milliseconds last_aux_read_ms_ = -100000;   // 负值 = 还没读过
    services::AirConditionerInterlock ac_interlock_;
    services::MotionTiming motion_timing_;
    std::unique_ptr<services::PositionSwitch> position_switch_;
    std::unique_ptr<::business::data_delivery::DeliverySink> local_sink_;
    std::unique_ptr<::business::data_delivery::DeliverySink> gateway_sink_;
    events::UnavailableGatewayAdapter gateway_adapter_;
    events::UnspecifiedFeedSegmentRules feed_rules_;
    services::WorkFrequency work_frequency_;
    events::DetectionStage stage_ = events::DetectionStage::idle;
    std::int64_t belt_sequence_ = -1;
    std::string target_belt_;
    std::optional<data_delivery::DeliveryStatus> last_delivery_;
    std::atomic<bool> motor_stop_requested_{false};
};

}  // namespace business::app




