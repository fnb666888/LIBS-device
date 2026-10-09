#pragma once

// 业务二：激光触发光谱检测（measurement）—— 作业主体。
//
// ── 这个流程是什么 ──────────────────────────────────────────────────────
//
//   连续出光 → 每次出光被光谱仪采到 → 落盘 → 实时显示
//
// 这是现场真正要跑的那一件事。它是**唯一同时碰激光器和光谱仪的对象** ——
// 不是"之一"，是"唯一"。约束 1 成立与否，全看下面 discipline 1 守没守住。
//
// ── 为什么光闸一直开着（方案 A）────────────────────────────────────────
//
// **光闸常开，采集被动跟随 TTL**，而不是软件每发开关一次光闸。原因是物理的：
// `prepareContinuous` 到设备进入「正在工作且放电」要 5~6 秒（实测值），
// 这个自适应过程不可能每发都重走一遍。所以"每发一个完整时序"在连续场景下
// 根本不成立 —— 而循环等 TTL 的方式不需要软件知道频率是多少：
//
//   激光按设定频率自己打 → 每打一发出一个 TTL → 光谱仪在 pin 6 收到就采一次
//
// 于是**每一帧天然对应一次出光**，不会错配、不需要软件数拍子，
// 每次采集等待可用 TTL；速率检查用于发现明显不匹配，逐发不丢仍须真机验证。
//
// ── 七条纪律 ────────────────────────────────────────────────────────────
//
//  1. **门禁是唯一判定入口，不放行时一条写指令都不发**。
//     `start()` 调 `readiness_.evaluate_now()`，任一条原因不满足就**原样返回错误**。
//     注意返回的位置：此时还没碰过 `laser_`。
//
//  2. **运行期重判**。`start()` 那次判定是**快照**，只反映 start() 那一瞬间的事实。
//     连续出光可能打到你手动停止，中间过了几十分钟。所以采集循环里**周期性重判**
//     （`kGateRecheckIntervalMs`），中途失锁就**自己收光**。
//
//     **为什么不能靠别的机制收光**：不能靠 UI（可能崩/被关/卡住），
//     不能靠互锁自己（互锁是"事实变了"，它不负责"去把光关掉"）。
//     所以判定的执行方和收光的执行方必须是同一个对象 —— 这不是设计偏好，
//     是安全性质。
//
//  3. **`wait_for_laser_trigger` 硬编码 true**（在 `capture_plan.cpp` 里）。
//
//  4. **落盘异步，绝不阻塞采集节奏**（见 `writer.hpp`）。
//
//  5. **停止随时可调、无条件执行**。哪怕 start 从没成功过、哪怕中途失败，
//     `stop()` 都要尽力把光闸关掉。**关串口不会让激光停下来** ——
//     设备自己保持着"工作"状态，守护进程崩了光还在打。
//
//  6. **失败不伪造**。采集失败 → 如实报错、帧里带 error code，
//     **绝不用零值填充出一条"平的谱"冒充数据**；且采集失败**不终止循环**。
//
//  7. **出光会话互斥由 Runtime 记账**（一个布尔 + 一把锁），本类不碰。

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libs/business/measurement/capture_plan.hpp"
#include "libs/business/measurement/frame.hpp"
#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/writer.hpp"
#include "libs/business/readiness/monitor.hpp"
#include "libs/business/shared/config/site_config.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/business/shared/logging/run_log.hpp"
#include "libs/devices/laser.hpp"
#include "libs/devices/spectrometer.hpp"
#include "libs/error.hpp"

namespace business::measurement {

// 运行期重判的周期。
//
// **为什么不是 0（每帧都判）**：判定不是免费的，每帧都判会拖慢采集节奏。
// 500 ms 的含义是"操作员打开安全门后，最多半秒光就收掉了"。
// 更短没有额外价值 —— 联锁断开到光真正灭掉还有机械与放电时间。
inline constexpr std::uint64_t kGateRecheckIntervalMs = 500;

// 一次出光作业的入参。
//
// `laser` 是会话参数（激光电流、脉宽、频率、Q 延时…），
// 后三项是光谱侧的测量参数。**两类参数来源不同**：出光参数来自装机配置里
// 已标定的那份（`LaserFireConfig::commissioned`），测量参数来自参数库。
// 调用方负责按协议量程校验好再交进来。
struct FireParameters {
    config::LaserFireConfig laser;       // 会话参数（调用方已按协议量程校验）
    double integration_ms = 10.0;        // 光谱仪积分时间
    unsigned capture_timeout_ms = 3000;  // 等 pin6 触发的超时
    // 收到 pin6 脉冲后再等多久才开始积分。**测量参数**，由参数管理服务供给。
    // 曾经从 SpectrometerConfig 读，与参数库形成两份来源，已归并到参数库。
    double integration_delay_ns = 0.0;
    bool override_integration = false;
    bool override_timeout = false;
    bool override_delay = false;
};

// 一次测量作业的启动记录。**任何失败都要有记录** —— 现场排障靠它，
// 而不是靠"当时屏幕上显示了什么"。
struct MeasurementStartRecord {
    bool ok = false;
    std::string failed_step;
    std::string error_code;
    std::string error_message;

    // 实际下发的指令序列（十六进制帧），逐步记录。
    // 出光问题 90% 是"发出去的顺序不对"，没有这个就无从复盘。
    std::vector<std::string> sent_frames;

    unsigned settle_ms = 0;                 // 从工作使能到"正在工作且放电"
    std::uint32_t discharge_counter_at_start = 0;

    std::string note;                       // 中文小结，可直接写日志
};

// 作业的运行期状态。给 `measurement status` 与 UI 用。
struct MeasurementStatus {
    bool running = false;
    std::uint64_t shots_captured = 0;      // 成功采集并投递的帧数
    std::uint64_t shots_failed = 0;        // 采集失败次数（不终止循环）
    std::uint64_t disk_writes_ok = 0;
    std::uint64_t disk_writes_failed = 0;
    std::uint64_t disk_backlog = 0;        // 落盘线程还没处理完的帧数
    std::string last_error_code;
    std::string last_error_message;
};

// 运行期状态 -> JSON。放这里而不是 facade，是为了 `measurement status` 的
// 字段名只有一个来源 —— 字段散在两处改，必然出现"报出去的和内部记的不一样"。
json::Value to_json(const MeasurementStatus& status);

// 连续出光测量作业。
//
// 依赖注入：**不持有 `Runtime&`**。它只拿自己真正需要的六样东西，
// 换句话说：这六样东西就是"这个流程的全部依赖"，一眼看得完。
// 这也是它能被离线测试的原因 —— 注入替身即可，不需要开串口、不需要 Runtime。
class MeasurementJob {
public:
    // 构造函数依赖很窄，是有意的：
    //   laser/spectrometer 设备句柄（来自 DeviceSession，本类不拥有）
    //   readiness           唯一判定入口（业务一）
    //   frame_bus           帧往哪儿送（纯内存，不碰设备；Runtime 拥有它）
    //   writer              帧往哪儿存（异步落盘子模块；Runtime 拥有它）
    MeasurementJob(libs::devices::Laser* laser,
                   libs::devices::Spectrometer* spectrometer,
                   readiness::ReadinessMonitor& readiness,
                   FrameQueue& frame_bus,
                   Writer& writer, std::mutex* acquisition_mutex = nullptr);

    ~MeasurementJob();
    MeasurementJob(const MeasurementJob&) = delete;
    MeasurementJob& operator=(const MeasurementJob&) = delete;

    // 启动测量。
    //
    // 流程（任一步失败立即中止，**未放行时不发任何写指令**）：
    //   1. 门禁判定（出光参数是否已标定、联锁、急停…）—— 不放行就中止
    //   2. 取测量参数（积分时间 / 超时 / 积分延时）—— 唯一来源是参数库
    //   3. prepareContinuous(): configure laser and wait for discharge, gate closed.
    //   4. Arm all spectrometer channels; SDK completion callback opens the Q gate.
    //      No fixed sleep is used to guess whether the spectrometer is ready.
    //   5. Continue acquiring and fan out full frames to UI and writer.
    //
    // 重复调用是安全的：已在跑则返回失败 `laser_already_running`，
    // 而不是又发一遍 0x21=1/0x24=1。
    libs::Result<MeasurementStartRecord> start(const FireParameters& params);

    // 停止：光闸关 → 停机 → **读回确认**。采集循环随之结束。
    //
    // 必须随时可调用且**无条件执行**。守护进程退出时也靠它收光。
    libs::VoidResult stop();

    MeasurementStatus status() const;

    bool running() const noexcept { return running_.load(); }

    // ── 供 Runtime 装配与测试 ────────────────────────────────────────────

    // 绑定设备句柄。DeviceSession 按配置独占打开句柄，Runtime 在 start() 里
    // 才知道实际拿到了哪些设备，所以这里提供运行期绑定入口。
    void bind_devices(libs::devices::Laser* laser, libs::devices::Spectrometer* spectrometer);

    // 装配期设置：站点接线事实 + 测量参数取值函数。
    //
    // **测量参数用函数而不是值**：参数库可以在作业运行中被操作员改
    // （`param_set`），而"每次采集读一次最新值"才是"改了立刻生效"的含义。
    void configure(const config::SiteConfig& site,
                   std::function<SpectrumMeasurement()> measurement_provider);

    // 日志出口。作业的每一次状态变化都该留痕，但日志不该是它能否运行的
    // 前提 —— 所以它是可选的（nullptr = 不写）。
    void set_log(logging::RunLog* log) noexcept { log_ = log; }

    // 供离线测试注入：把一次"采集"替换成受控行为。
    // 存在的原因：DeviceSession 按配置独占打开，没有注入点就没有离线测试。
    void set_capture_hook_for_test(std::function<libs::Result<libs::devices::CaptureResult>()> hook) {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_hook_ = std::move(hook);
    }

private:
    // 采集循环线程主循环。**唯一碰光谱仪的地方**。
    void loop(std::function<libs::VoidResult()> on_armed,
              std::function<void(const SpectrumFrame&)> on_first,
              SpectrumMeasurement initial);
    // 做一次采集并组装成帧。设备为 null / 参数未配置都如实报错。
    SpectrumFrame capture_once(const std::function<libs::VoidResult()>& on_armed = {},
                               const SpectrumMeasurement* initial = nullptr);
    // 运行期重判：中途失锁就自己收光。返回 false = 已因失锁而中止循环。
    bool recheck_gate();

    // 记录一条运行日志。log_ 为空时什么都不做。
    void log_record(logging::RecordKind kind, const std::string& message,
                    std::function<json::Value()> payload);

    std::mutex lifecycle_mutex_;
    std::mutex* acquisition_mutex_ = nullptr;
    unsigned laser_frequency_hz_ = 0;
    FireParameters active_params_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;        // 采集循环

    libs::devices::Laser* laser_ = nullptr;
    libs::devices::Spectrometer* spectrometer_ = nullptr;

    readiness::ReadinessMonitor& readiness_;
    FrameQueue& frame_bus_;
    Writer& writer_;
    logging::RunLog* log_ = nullptr;

    config::SiteConfig site_;
    std::function<SpectrumMeasurement()> measurement_provider_;

    // 帧号由**采集侧**单调自增，与显示侧的丢帧计数完全无关。
    // 早先把它算成 `dropped + captured + failed`，把 UI 的丢弃数混进了
    // 测量编号，结果就是漏掉一帧显示会让后续所有帧号错位。
    std::uint64_t next_frame_index_ = 0;
    std::atomic<std::uint64_t> shots_captured_{0};
    std::atomic<std::uint64_t> shots_failed_{0};
    std::string last_error_code_;
    std::string last_error_message_;

    // 运行期重判的时刻。0 = 从没判过。
    std::uint64_t last_gate_check_ms_ = 0;

    // 记录最近一次 start() 的实际下发序列，供 status 与日志复盘。
    std::vector<std::string> last_sent_frames_;
    unsigned last_settle_ms_ = 0;
    std::uint32_t last_discharge_counter_ = 0;

    std::function<libs::Result<libs::devices::CaptureResult>()> capture_hook_;
};

}  // namespace business::measurement