#pragma once

// 实时光谱流（能力 10 的展示侧，本次新增）。
//
// 判定依据：范围报告 §3.10 采集协调 + 任务书"实时显示光谱图"的展示需求。
// 既有方案里 capture 是一次性的（有界 scans 循环，拿到结果就返回），
// 现场要"边测边看"就需要一条**持续产出**的链路。本文件补的就是这一段。
//
// ── 三条纪律（与既有 services/ 保持一致，不是新发明的规矩）───────────────
//
//  1. **hardware_synchronized 原样上送**（§3.10.1）。
//     与 cmd_capture 同一个规矩：本服务不判断"物理上同步了没有"，
//     只把 SDK 的事实搬到帧里，业务层视角的 requested_sync 单列一个字段。
//
//  2. **流式服务不发任何激光写指令**。
//     它只调用 CaptureCoordinator::capture()，那是能力 10 的采集侧，
//     不含出光。出光是 LaserFireService 的事，且必须先过门禁。
//     换句话说：实时预览在默认（自由触发）下拍到的是背景谱 / 杂散光，
//     那是**诚实的观测值**，不是 LIBS 测量值。本文件在 note 里写明这一点。
//
//  3. **通道缺失是第三态，不是"采集失败"**（§7 纪律 2）。
//     某个通道没回来，帧里那一路就是 absent + 原因，**不**整帧判失败，
//     **不**用零值填充。UI 侧据此把那条曲线标成"缺"而不是画一条假的平线。
//
// ── 第四条：与一次性采集互斥（外触发模式下的物理约束）──────────────────
//
// 这是本文件最容易被忽略、但会直接毁掉测量的一点：
// 站点配置里 wait_for_laser_trigger=true（激光触发光谱仪，§4.9 接法二）时，
// 光谱仪在**外触发模式**下等 pin 6 的 TTL。此时**任何**一次 capture 都会
// 去"吃掉"那个 TTL。如果实时流也在跑，它可能抢在真正那次出光之前
// 把触发脉冲消耗掉——结果是测量那次永远等不到触发，一直超时。
//
// 因此本服务引入**采集租约（acquisition lease）**：
// 一次性操作（laser_fire / capture）在开始前 acquire()，流循环在采集前
// 持有租约，于是物理上一次只有一条采集路径在动设备。
// 租约本身不带任何"能不能出光"的判断——那仍然只由 Gate 负责。
//
// ── 背压策略：丢旧帧，不阻塞采集 ────────────────────────────────────────
//
// 订阅者（UI 客户端）各有自己的队列。队列满时**丢最旧的一帧**，
// 绝不阻塞采集线程：光谱仪的节奏由硬件决定，为了迁就一个慢客户端
// 而拖慢采集，代价是测量本身。丢帧会如实记 dropped_frames，
// 实时预览丢几帧无所谓，但不能假装"一帧没丢"。

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libs/business/config/site_config.hpp"
#include "libs/business/json/json.hpp"
#include "libs/business/services/capture.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"

namespace business::services {

// 一个通道在某一帧里的状态。三态，不与"数值 0"混。
enum class ChannelState {
    present,     // 这一帧采到了
    absent,      // 这次没采到；reason 里说明原因
};
const char* to_string(ChannelState state) noexcept;

// 单个通道的一帧数据。
struct ChannelFrame {
    std::string serial;
    ChannelState state = ChannelState::absent;
    std::string reason;            // state=absent 时的中文原因
    std::uint16_t pixels = 0;
    // 本帧这一路被抽稀的倍数（1 = 全分辨率）。
    // pixels 报的始终是**真实像素数**，counts.size() 是实际发来的点数，
    // 两者一比就能看出抽稀了多少 —— 界面据此标注，不让人把抽稀波形当全分辨率。
    unsigned decimate = 1;
    unsigned device_time_ticks = 0;
    std::vector<double> wavelength_nm;   // 长度 == pixels
    std::vector<double> counts;          // 长度 == pixels
};

// 一次采集产出的**一帧**。UI 每收到一帧就重画一次。
//
// 帧里**不含**波长轴之外的任何推断量：没有归一化、没有基线扣除、
// 没有累加平均。UI 想显示归一化曲线，自己算——
// 业务层替 UI 预先"处理"过光谱，等于把处理口径写死在库里。
struct SpectrumFrame {
    std::int64_t frame_index = 0;      // 进程内单调递增；UI 靠它发现丢帧
    std::uint32_t shot_id = 0;         // SDK 报的 shot id，原样
    Milliseconds wall_clock_ms = 0;
    Milliseconds captured_monotonic_ms = 0;

    // 同步事实（§3.10.1 纪律）：原样上送，不合并、不推断。
    bool hardware_synchronized = false;
    bool requested_sync = false;

    // 采集节奏，UI 可据此显示"当前多少 ms 一帧"。
    double period_ms = 0.0;
    unsigned timeout_ms = 0;

    std::vector<ChannelFrame> channels;

    // 采集层面的失败（如整次 capture 超时）。**空 = 这次采集本身成功**。
    // 通道缺失不走这里，走上面各 channel 的 state。
    std::string capture_error_code;
    std::string capture_error_message;
    bool ok() const noexcept { return capture_error_code.empty(); }
};

json::Value to_json(const ChannelFrame& channel);
json::Value to_json(const SpectrumFrame& frame);

// ── 采集租约（见文件头"第四条"）────────────────────────────────────────
//
// 极简的互斥：一次性采集持租约期间，流循环**跳过这一轮**（不排队、不阻塞）。
// 为什么是"跳过"而不是"等"：等会让流循环的节奏被一次性采集拖住，
// 而流的价值就是节奏稳定。跳过则只丢几帧预览，对 UI 无感。
//
// 租约是 RAII：构造即 acquire，析构即 release。异常或提前 return
// 都不会把设备永久留在"被别人占着"的状态。
// 前向声明：租约要引用流，流的方法又返回租约（不加就过不了编译）。
class SpectrumStream;

class AcquisitionLease {
public:
    explicit AcquisitionLease(SpectrumStream& stream);
    ~AcquisitionLease();
    AcquisitionLease(const AcquisitionLease&) = delete;
    AcquisitionLease& operator=(const AcquisitionLease&) = delete;

    bool held() const noexcept { return held_; }
    // 没能拿到租约（例如流已经停止）时不抛异常，只是 held()==false，
    // 调用方**必须**自己判——不要因为没判就以为自己在独占地操作设备。
    explicit operator bool() const noexcept { return held_; }

private:
    SpectrumStream& stream_;
    bool held_ = false;
};

// ── 实时光谱流服务 ──────────────────────────────────────────────────────
//
// 生命周期：Runtime 持有它（唯一实例），start() 起后台线程，stop() 收。
// 线程模型：一个采集线程 + 若干订阅者（各自在自己的线程里消费队列）。
// 订阅者的队列**不属于**任何 GUI 线程，因此 UI 崩了/卡了都不会影响采集。
class SpectrumStream {
public:
    struct Options {
        // 每帧间隔下限，单位毫秒。0 = 不额外限速。
        // **下限是保护措施**：默认 200ms（约 5fps），
        // 免得有人把 period 设成 1ms 把光谱仪跑满并写爆日志。
        unsigned min_period_ms = 200;
        // 订阅者队列最多保留几帧。满了丢最旧的。
        std::size_t subscriber_queue_frames = 4;
        // 是否跟随站点配置的 wait_for_laser_trigger。
        // **默认 false**：预览不让光谱仪进外触发模式，
        // 否则在没有 TTL 源时每帧都会超时。
        // 需要看真实 LIBS 谱时才置 true，且必须与激光器串好。
        bool follow_site_trigger_mode = false;

        // 预览帧抽稀倍数（1 = 不抽稀，原样发全部像素）。
        //
        // 为什么需要：一帧四通道 2048 点的 JSON 序列化 + 过管道实测约 800 ms
        // （见 ui/src/main_window.cpp 的注释）。预览要跟得上激光的重复频率，
        // 就必须把单帧体积降下来 —— 抽稀是唯一能真正提吞吐的一步。
        //
        // 纪律：**抽稀只作用于预览**（人眼看的波形），不作用于采集落盘 ——
        // 逐发的原始谱由「出光测量」按全像素保存，预览绝不代替它。
        // 抽稀倍数如实上报（frame.decimate），界面据此标注"显示已抽稀"，
        // 不让人把抽稀后的波形当成全分辨率数据。
        unsigned decimate = 1;
    };

    SpectrumStream() = delete;
    explicit SpectrumStream(std::mutex& device_mutex);
    ~SpectrumStream();

    SpectrumStream(const SpectrumStream&) = delete;
    SpectrumStream& operator=(const SpectrumStream&) = delete;

    // 按站点配置绑定设备与节奏。**不启动线程**。
    // 与其它服务一样：设备句柄来自 Runtime，本类只拿裸指针，不拥有。
    //
    // 设备锁在**构造函数**注入（与 Runtime 共享同一把），不在这里传：
    // 两把锁等于没有锁，而重复传入正是制造两把锁的入口。
    void configure(const config::SiteConfig& site, libs::devices::Spectrometer* device);

    // 覆盖选项（限速、队列深度、是否跟随站点触发模式）。必须在 start() 之前调用。
    void set_options(Options options);

    // 只读回显当前生效的选项：界面据此标注"本帧已抽稀 N 倍"与周期，
    // 不让人把抽稀后的波形当成全分辨率数据。
    Options options() const;

    // 启动 / 停止。重复调用是安全的。
    // 停止会等采集线程 join 完再返回：返回后不会再有新帧。
    libs::VoidResult start();
    void stop();

    bool running() const noexcept;

    // 运行状态快照。给 `spectrum_stream status` 用。
    json::Value status() const;

    // ── 订阅 ────────────────────────────────────────────────────────────
    //
    // 订阅句柄就是一个数字 id，不持有任何设备或线程资源。
    // 退订（unsubscribe）可重复调用。disconnect 之前建议先 unsubscribe，
    // 否则退订后残留的帧会在客户端断开时被白白丢掉。
    std::uint64_t subscribe();
    void unsubscribe(std::uint64_t id);

    // 取出一帧。队列空返回 false，**不阻塞**。
    //
    // **注意**：这是唯一的取帧入口，且**不**加 device_mutex——
    // 采集线程早就不在设备上了，取帧不该和设备访问互相排队。
    bool try_pop(std::uint64_t id, SpectrumFrame& out);

    std::size_t pending(std::uint64_t id) const;

    // 累计丢帧数（因为订阅者队列满而丢的）。UI 可用它提示"显示跟不上采集"。
    std::uint64_t dropped_frames() const noexcept;

    // ── 采集租约的内部接口 ───────────────────────────────────────────
    // 私有 + friend：只有 AcquisitionLease 该碰。公开了就会有人
    // 手工 acquire/release，绕过 RAII 把设备留在"一直被占着"的状态。
    friend class AcquisitionLease;
    bool try_acquire_lease();
    void release_lease();

    // 供测试注入：把一次"采集"替换成受控行为。
    // 存在的原因与 LaserFireService::set_laser_for_test 相同：
    // DeviceSession 按配置独占打开，没有注入点就没有离线测试。
    void set_capture_hook_for_test(std::function<libs::Result<libs::devices::CaptureResult>()> hook) {
        capture_hook_ = std::move(hook);
    }

private:
    // 采集线程主循环。
    void loop();
    // 做一次采集并组装成帧。设备为 null / 节奏未配置都走这里如实报错。
    SpectrumFrame acquire_frame();
    // 把一帧投递给所有订阅者（各自丢最旧的一帧）。
    void publish(const SpectrumFrame& frame);

    mutable std::mutex mutex_;          // 保护下面所有共享状态
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;

    config::SiteConfig site_;
    libs::devices::Spectrometer* device_ = nullptr;
    // 与 Runtime 共享的**同一把**设备锁。引用成员，生命周期由 Runtime 保证
    // （Runtime 成员声明顺序保证 stream_ 比 mutex_ 先析构）。
    std::mutex& device_mutex_;
    Options options_;

    // 租约持有计数。>0 表示一次性采集在动设备，流循环必须让路。
    int lease_count_ = 0;

    std::uint64_t next_frame_index_ = 0;
    std::uint64_t next_subscriber_id_ = 1;
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::uint64_t delivered_frames_ = 0;

    struct Subscriber {
        std::deque<SpectrumFrame> queue;
    };
    std::map<std::uint64_t, Subscriber> subscribers_;

    std::string last_error_code;
    std::string last_error_message;
    Milliseconds last_frame_monotonic_ms = 0;

    std::function<libs::Result<libs::devices::CaptureResult>()> capture_hook_;
};

}  // namespace business::services