// 连续出光测量作业离线测试。
//
// 守的是 measurement/job.hpp 文件头写下的七条纪律里离线可验的那些：
//   1. 判定只在业务二这一条调用栈里发生（不放行时一条写指令都不发）
//   2. 运行期重判：中途失锁自己收光
//   3. wait_for_laser_trigger 恒为 true
//   4. 失败不伪造、也不因偶发失败终止循环
//   5. 落盘异步且有界，绝不反压采集
//   6. 停止随时可调、无条件执行（关串口不会让激光停下来）
//
// **不碰真实串口，也不加载 SDK**：激光走 FakeLink 顶替 transport::Link，
// 于是 startContinuous/stopContinuous 的**真实时序**都被覆盖到了 ——
// 出光问题 90% 是"发出去的顺序不对"，只测替身等于没测。
// 光谱仪侧没有注入点（AvaSpec SDK 是 open() 出来的），所以采集动作由
// set_capture_hook_for_test 换成受控替身。
//
// 这样"会不会抢触发""磁盘卡住会不会拖慢出光""停止会不会白等一个 TTL"
// 这些只能靠真机发现的问题，可以在桌面上被确定性地复现。

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <windows.h>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"

#include "libs/business/shared/config/site_config.hpp"
#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/job.hpp"
#include "libs/business/measurement/writer.hpp"
#include "libs/business/readiness/monitor.hpp"
#include "libs/business/readiness/verdict.hpp"
#include "libs/protocols/laser.hpp"

using namespace business;
using libs::Bytes;
using libs::Error;
using libs::ErrorKind;
// 交付层的两个类型名字很长，测试里到处要用，直接起个短别名。
using data_delivery::CaptureReference;
using data_delivery::DeliveryStatus;
using libs::Result;

namespace {
template<class T> T fake_export(HMODULE module, const char* name) {
    T result = nullptr;
    const FARPROC raw = GetProcAddress(module, name);
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
}

// 假串口：**有状态的**激光器模拟器。
//
// 为什么不是"预置一串回包按序发"：那种写法把测试绑死成"驱动恰好读了几次
// 0xB2"。而读取次数取决于轮询节奏与超时，改一行 settle 参数就全线崩 ——
// 曾经就因为预排多了一条"安全态"回包，导致放电那一帧永远轮不到，
// 报"设备没有进入放电"，而这与被测逻辑无关。
//
// 这里的做法是：写 kWorkState/kQGate 就改自己的状态，读 kLdActual 就报
// 当前状态。测试因此表达的是**意图**（"光闸已开"、"正在放电"），
// 读取多少次都对。测出光顺序仍然是真实的 —— 发出去的每一帧都记在 sent。
class FakeLink final : public libs::transport::Link {
public:
    std::vector<Bytes> sent;
    std::deque<libs::Result<libs::transport::Reply>> replies;
    bool fail_writes = false;

    // 模拟器自己的状态。测试可以直接改，也可以让写指令去改。
    unsigned counter = 100;
    bool work_state = false;
    std::function<void()> on_work_on;
    bool q_gate_open = false;
    std::function<void()> on_gate_open;
    bool interlock_ok = true;
    bool estop_ok = true;
    bool ot_ok = true;
    std::uint8_t fault_bits = 0;
    // 工作使能之后**多少毫秒**才进入放电。真机上这是 5~6 秒，
    // 测试里默认 0（立刻），需要复现"settle 等待"时才调大。
    unsigned discharge_latency_ms = 0;

    libs::Result<libs::transport::Reply> exchange(const Bytes& request, unsigned timeout_ms = 650,
                                                 unsigned idle_ms = 80) override {
        (void)timeout_ms;
        (void)idle_ms;
        sent.push_back(request);
        if (fail_writes) {
            return libs::Error::make(libs::ErrorKind::transport, "cable_unplugged", "fake write failure");
        }
        if (request.size() >= 5 && request[0] == 0x5A && request[1] == 0xA5) {
            const std::uint8_t command = request[4];
            const Bytes payload(request.begin() + 5, request.end() - 2);   // 去掉尾 CRC
            switch (command) {
                case libs::laser::command::kWorkState:
                    // 工作使能/停机。停机立刻断，放电计数也停。
                    work_state = !payload.empty() && payload[0] != 0x00;
                    if (work_state && on_work_on) on_work_on();
                    if (!work_state) counter = counter;   // 停机不重置计数，只是记录
                    break;
                case libs::laser::command::kQGate:
                    q_gate_open = !payload.empty() && payload[0] != 0x00;
                    if (q_gate_open && on_gate_open) on_gate_open();
                    break;
                case libs::laser::command::kLdActual:
                    return reply(actual_frame());
                default:
                    break;
            }
        }
        // 只有读请求才消费预置回包。写指令按协议原数回显，不该"吃掉"下一条 0xB2。
        const bool is_read = request.size() >= 5 &&
                             (request[4] == libs::laser::command::kLdSettings ||
                              request[4] == libs::laser::command::kLdActual ||
                              request[4] == libs::laser::command::kTec);
        if (is_read && !replies.empty()) {
            auto r = replies.front();
            replies.pop_front();
            return r;
        }
        libs::transport::Reply reply;
        reply.bytes = request;          // 协议要求原数发回
        reply.elapsed_ms = 1;
        return libs::Result<libs::transport::Reply>::success(reply);
    }

    std::string endpoint() const override { return "FAKE12"; }

    void queue(const Bytes& bytes) {
        libs::transport::Reply r;
        r.bytes = bytes;
        r.elapsed_ms = 2;
        replies.push_back(libs::Result<libs::transport::Reply>::success(r));
    }

    // 组一帧 0xB2。极性按协议 §3.5：0=安全，1=危险。
    Bytes actual_frame() const {
        // 使能之后经过 discharge_latency_ms 才真的在放电。
        const bool discharging = work_state && discharge_latency_ms == 0;
        Bytes d(22, 0);
        d[0]  = discharging ? 1 : 0;
        d[1]  = 0;
        d[2]  = fault_bits;
        d[3]  = interlock_ok ? 0 : 1;
        d[4]  = interlock_ok ? 0 : 1;
        d[5]  = estop_ok ? 0 : 1;
        d[6]  = ot_ok ? 0 : 1;
        // 4 字节大端（协议 §3.5，高位在前）。曾用错字节序，
        // 恰好与 16 位截断的 bug 相互抵消，害得截断一直没被发现。
        d[13] = static_cast<std::uint8_t>((counter >> 24) & 0xFF);
        d[14] = static_cast<std::uint8_t>((counter >> 16) & 0xFF);
        d[15] = static_cast<std::uint8_t>((counter >> 8) & 0xFF);
        d[16] = static_cast<std::uint8_t>(counter & 0xFF);
        d[19] = q_gate_open ? 1 : 0;
        d[20] = discharging ? 1 : 0;
        d[21] = 0;
        return libs::laser::make_frame(libs::laser::command::kLdActual, d);
    }

    // 排一次 0xB2 实际状态读。保留给需要**逐次脚本化**读回的场景；
    // 大多数用例应该直接改上面的状态字段。
    void queue_actual(unsigned c, bool work, bool gate,
                      bool ilock = true, bool estop = true, bool ot = true,
                      std::uint8_t fault = 0) {
        counter = c;
        work_state = work;
        q_gate_open = gate;
        interlock_ok = ilock;
        estop_ok = estop;
        ot_ok = ot;
        fault_bits = fault;
        queue(actual_frame());
    }

    libs::Result<libs::transport::Reply> reply(const Bytes& bytes) const {
        libs::transport::Reply r;
        r.bytes = bytes;
        r.elapsed_ms = 2;
        return libs::Result<libs::transport::Reply>::success(r);
    }

    // 写指令码序列（按发送顺序）。只统计**写帧**。
    //
    // 判据是"指令码是不是读指令"，不能看帧长：0x34 单次触发和 0x31 保存参数
    // 本来就没有载荷，帧长与读请求一样是 4 字节；按长度过滤会漏掉出光最关键
    // 的那两帧。而漏算读请求同样是错的——"门禁拦截时零写帧"是本测试最硬的断言。
    std::vector<std::uint8_t> write_commands() const {
        std::vector<std::uint8_t> out;
        for (const Bytes& f : sent) {
            if (f.size() < 5 || f[0] != 0x5A || f[1] != 0xA5) continue;
            if (f[4] == libs::laser::command::kLdSettings ||
                f[4] == libs::laser::command::kLdActual ||
                f[4] == libs::laser::command::kTec) continue;
            out.push_back(f[4]);
        }
        return out;
    }
};

config::SiteConfig make_site() {
    config::SiteConfig site = config::default_template().value();
    site.station_id = "TEST";
    // 已标定的出光参数。时间参数压到最小，测试才跑得动。
    site.laser_fire.commissioned = true;
    site.laser_fire.discharge_current_a = 80;
    site.laser_fire.pulse_width_us = 200;
    site.laser_fire.frequency_hz = 1;
    site.laser_fire.discharge_delay_us = 0;
    site.laser_fire.q_delay_us = 210;
    site.laser_fire.q_warmup_s = 0;
    site.laser_fire.settle_timeout_ms = 600;
    site.spectrometer.serials = {"SN-A"};
    site.spectrometer.master_serial = "SN-A";
    site.spectrometer.synchronized = true;
    return site;
}

measurement::SpectrumMeasurement make_measurement() {
    measurement::SpectrumMeasurement m;
    m.integration_ms = 10.0;
    m.timeout_ms = 300;              // 测试里别等 3 秒
    m.integration_delay_ns = 50000.0;
    m.scans = 1;
    return m;
}

measurement::FireParameters make_params() {
    measurement::FireParameters p;
    p.laser.discharge_current_a = 80;
    p.laser.pulse_width_us = 200;
    p.laser.frequency_hz = 1;
    p.laser.q_delay_us = 210;
    p.laser.q_warmup_s = 0;
    p.laser.settle_timeout_ms = 600;
    p.integration_ms = 10.0;
    p.capture_timeout_ms = 300;
    return p;
}

// 一个永不报错的 monitor。用于"激光器根本没连上"这类用例：
// 那条路径必须在碰判定之前就返回 laser_not_open，所以 monitor 说什么都不该有影响。
std::unique_ptr<readiness::ReadinessMonitor> make_quiet_monitor() {
    return std::make_unique<readiness::ReadinessMonitor>(
        [](readiness::ReadinessFacts&) { return false; });   // 读不到 = 按不安全
}

// 构造一个"门禁放行"的判定事实。
readiness::ReadinessFacts safe_readiness_facts() {
    readiness::LaserSafetyFacts laser;
    laser.available = true;
    laser.work_state = false;
    laser.interlock_1 = true;
    laser.interlock_2 = true;
    laser.emergency_stop = true;
    laser.q_gate = true;
    laser.over_temperature = true;
    laser.fault_bits = 0;

    readiness::ReadinessFacts f;
    f.laser = laser;
    f.devices_opened = true;
    f.spectrometer_ready = true;
    f.parameters_complete = true;
    f.laser_enabled = true;
    f.laser_commissioned = true;
    return f;
}

// 让判定放行。**事实源是可变的**（`mutable facts`），这样运行期重判用例
// 可以在作业跑起来之后把联锁弄断，验证它自己收光。
std::unique_ptr<readiness::ReadinessMonitor> make_allowing_monitor(
        readiness::ReadinessFacts* facts_ptr) {
    auto monitor = std::make_unique<readiness::ReadinessMonitor>(
        [facts_ptr](readiness::ReadinessFacts& out) {
            out = *facts_ptr;
            return true;          // 读到了
        });
    monitor->set_parameters_ready(true);
    monitor->set_laser_enabled(true);
    monitor->set_laser_commissioned(true);
    monitor->set_devices_opened(true);
    monitor->set_spectrometer_ready(true);
    (void)monitor->arm();        // 内部会先 evaluate 一次
    return monitor;
}


libs::devices::Spectrum make_channel(const std::string& serial, std::size_t pixels) {
    libs::devices::Spectrum s;
    s.serial = serial;
    s.pixels = static_cast<std::uint16_t>(pixels);
    s.wavelength_nm.reserve(pixels);
    s.counts.reserve(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        s.wavelength_nm.push_back(200.0 + static_cast<double>(i));
        s.counts.push_back(100.0 + static_cast<double>(i));
    }
    return s;
}

libs::devices::ScanGroup make_group(bool synchronized, std::uint32_t shot_id = 7) {
    libs::devices::ScanGroup g;
    g.shot_id = shot_id;
    g.complete = true;
    // 原样上送：SDK 报什么就传什么，本类不推断"物理上同步了没有"。
    g.hardware_synchronized = synchronized;
    g.spectra.push_back(make_channel("SN-A", 4));
    return g;
}

Result<libs::devices::CaptureResult> ok_capture(bool synchronized = false) {
    libs::devices::CaptureResult r;
    r.groups.push_back(make_group(synchronized));
    return r;
}

// 采集替身：可计次、可按周期失败、可挂住。
class CaptureStub {
public:
    using CaptureResult = libs::devices::CaptureResult;

    std::function<libs::Result<CaptureResult>()> hook() {
        return [this]() {
            std::lock_guard<std::mutex> lock(mutex);
            ++calls;
            const std::size_t n = calls;
            if (hold_ms > 0) {
                // 模拟"在等一个 TTL"：阻塞一小段时间。
                std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
            }
            if (fail_every > 0 && n % fail_every == 0) {
                return libs::Result<libs::devices::CaptureResult>(
                    libs::Error::make(libs::ErrorKind::device, "capture_timeout",
                                      "等待激光 TTL 超时"));
            }
            return libs::Result<libs::devices::CaptureResult>(ok_capture(synchronized));
        };
    }
    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    mutable std::mutex mutex;
    std::size_t calls = 0;
    std::size_t fail_every = 0;
    unsigned hold_ms = 0;
    bool synchronized = false;
};

// 落盘替身：记录调用，并可控制写盘有多慢。不写真文件 ——
// 落盘正确性属于 LocalFileSink 自己的测试，这里只管编排顺序。
class RecordingSink final : public data_delivery::DeliverySink {
public:
    struct Call {
        data_delivery::CaptureReference ref;
        std::string blob;
    };

    std::string name() const override { return "recording"; }

    libs::Result<DeliveryStatus> deliver(const CaptureReference& capture,
                                        const std::string& local_blob) override {
        // **先记账再睡**：这样"排队中"也可见，调用次数立刻反映积压。
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        if (fail_next) {
            fail_next = false;
            DeliveryStatus s;
            s.local_saved = false;
            s.local_error = "disk_full";
            return s;
        }
        recorded.push_back(Call{capture, local_blob});
        DeliveryStatus s;
        s.local_saved = true;
        s.local_path = "memory://frame";
        s.gateway_error = "unsupported";
        return s;
    }

    std::size_t call_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    std::size_t recorded_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return recorded.size();
    }
    std::vector<Call> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(recorded);
    }
    void fail_once() {
        std::lock_guard<std::mutex> lock(mutex);
        fail_next = true;
    }

    mutable std::mutex mutex;
    std::size_t calls = 0;
    std::size_t delay_ms = 0;
    bool fail_next = false;
    std::vector<Call> recorded;
};

// 整套依赖：假串口 + 真 Laser + 帧队列 + 落盘替身。
// 这样 start/stop 的真实指令时序能被验到。
struct Fixture {
    config::SiteConfig site = make_site();
    FakeLink link;
    std::unique_ptr<libs::devices::Laser> laser;
    measurement::FrameQueue queue;
    RecordingSink sink;
    // 落盘是 Writer 的职责（子模块），作业通过它把帧送出去。
    // 所以这里必须显式起 Writer —— 它的线程生命周期由作业驱动。
    std::unique_ptr<measurement::Writer> writer;
    readiness::ReadinessFacts facts = safe_readiness_facts();
    std::unique_ptr<readiness::ReadinessMonitor> monitor;
    CaptureStub capture;
    std::unique_ptr<measurement::MeasurementJob> job;

    explicit Fixture(bool allow = true) {
        laser = std::make_unique<libs::devices::Laser>(link);
        writer = std::make_unique<measurement::Writer>(sink, site.station_id);
        monitor = allow ? make_allowing_monitor(&facts)
                        : std::make_unique<readiness::ReadinessMonitor>(
                              [](readiness::ReadinessFacts& out) {
                                  out = readiness::ReadinessFacts();
                                  return true;
                              });
        job = std::make_unique<measurement::MeasurementJob>(
            laser.get(), nullptr, *monitor, queue, *writer);
        job->configure(site, [] { return make_measurement(); });
        job->set_capture_hook_for_test(capture.hook());
        // **不预排任何回包**：FakeLink 是有状态的模拟器，0xB2 读多少次都有回。
        // 曾经这里预排 64 条"安全态"，结果放电那一帧排在第 67 条，
        // start() 永远等不到放电而报 laser_not_discharging —— 与被测逻辑无关。
    }

    ~Fixture() {
        // 析构顺序有意义：作业先停（不再往队列里推帧），
        // 再停 Writer（drain 完最后几帧），最后帧队列/替身自己析构。
        if (job) job->stop();
        if (writer) writer->stop();
    }
};

// 一直等到谓词成立或超时。返回是否成立。
template <class Pred>
bool wait_for(Pred pred, int timeout_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

}  // namespace

TEST_MAIN_BEGIN()

// ── 纪律 1：判定只在业务二这一条调用栈里发生 ────────────────────────────

// 下面三个用例改的都是 `f.facts` —— 它是 monitor 的事实源所引用的那份数据，
// 所以 job->start() 里的 evaluate_now() 每次都会重新读到它。
// **这正是"判定只在 MeasurementJob 里执行"的可测形态**：改事实 → 判定结果变，
// 中间没有任何缓存、没有第二个执行点。

CASE(gate_blocked_sends_no_write_frames); {
    // 这是整个流程最硬的一条断言：不放行时**一条写指令都不发**。
    // 判据用"写帧数 == 0"，不是"函数返回了错误" —— 返回错误是行为，
    // 零写帧才是安全保证。现场"以为没出光其实出了一下"就发生在这个缝隙里。
    Fixture f(/*allow=*/false);
    const libs::Result<measurement::MeasurementStartRecord> r = f.job->start(make_params());
    CHECK(!r);
    if (!r) {
        CHECK_EQ(r.error().code, std::string("gate_blocked"));
        // 判定拦下时**必须**说清"没发出任何写指令"，
        // 否则操作员会去查激光器，而问题在联锁上。
        CHECK(r.error().message.find("未发出任何写指令") != std::string::npos);
    }
    CHECK(f.link.write_commands().empty());
    CHECK(!f.job->running());
    CHECK_EQ(f.sink.call_count(), std::size_t(0));
}

CASE(gate_blocked_when_laser_facts_are_unreadable); {
    // 关键：**available=false**。读不到 B2 != 安全。
    // 曾经这里默认当成"没问题"，于是串口掉了照样出光。
    //
    // 这一条同时验了 monitor 的事实源：**返回 false = 读不到**，与
    // "返回 true 但位全坏"是两条不同的失败路径，前者报 laser_unavailable。
    Fixture f(/*allow=*/false);
    // 事实源改成读不到：既 available=false，也返回 false。
    auto monitor = std::make_unique<readiness::ReadinessMonitor>(
        [](readiness::ReadinessFacts& out) {
            out = safe_readiness_facts();
            out.laser.available = false;   // 位全是好的，但"没读到"
            return false;                   // 读不到
        });
    monitor->set_parameters_ready(true);
    monitor->set_laser_enabled(true);
    monitor->set_laser_commissioned(true);
    monitor->set_devices_opened(true);
    monitor->set_spectrometer_ready(true);
    f.job = std::make_unique<measurement::MeasurementJob>(
        f.laser.get(), nullptr, *monitor, f.queue, *f.writer);
    f.job->configure(f.site, [] { return make_measurement(); });
    f.job->set_capture_hook_for_test(f.capture.hook());
    // 清掉 Fixture 刚才生成时拆掉旧 job 导致的收光帧：
    // ~MeasurementJob 无条件收光，所以即使从未 start 也会发出 stop 帧。
    // 本用例断言的是 **start 这一次调用没发出任何写指令**，
    // 而不是“这个设备从头到尾没发过帧”。
    f.link.sent.clear();
    const auto r = f.job->start(make_params());
    CHECK(!r);
    if (!r) CHECK_EQ(r.error().code, std::string("gate_blocked"));
    CHECK(f.link.write_commands().empty());
}

CASE(gate_blocked_when_interlock_is_open); {
    // 联锁断开是最典型的现场阻断。
    Fixture f(/*allow=*/true);
    f.facts.laser.interlock_1 = false;   // 联锁1 断开 = 危险
    const auto r = f.job->start(make_params());
    CHECK(!r);
    if (!r) CHECK_EQ(r.error().code, std::string("gate_blocked"));
    CHECK(f.link.write_commands().empty());
}

CASE(gate_blocked_when_emergency_stop_is_pressed); {
    Fixture f(/*allow=*/true);
    f.facts.laser.emergency_stop = false;
    const auto r = f.job->start(make_params());
    CHECK(!r);
    CHECK(f.link.write_commands().empty());
}

CASE(gate_blocked_when_laser_is_not_commissioned); {
    // 未标定 = 不知道该发什么参数 = 不能出光。默认就是未标定，
    // 所以这条锁住的是"默认值本身就不安全"。
    Fixture f(/*allow=*/true);
    f.facts.laser_commissioned = false;
    const auto r = f.job->start(make_params());
    CHECK(!r);
    CHECK(f.link.write_commands().empty());
}

CASE(gate_blocks_when_parameters_are_incomplete); {
    Fixture f(/*allow=*/true);
    f.facts.parameters_complete = false;
    const auto r = f.job->start(make_params());
    CHECK(!r);
    CHECK(f.link.write_commands().empty());
}

// 放行路径必须真的出光 —— 否则上面五条就只是"永远不放行也能过"。
CASE(gate_allows_and_the_laser_actually_fires); {
    Fixture f(/*allow=*/true);
    const auto r = f.job->start(make_params());
    CHECK(r);
    CHECK(f.job->running());
    const auto cmds = f.link.write_commands();
    CHECK(!cmds.empty());
    (void)f.job->stop();
}

CASE(laser_not_open_fails_before_gate_is_consulted); {
    // 设备没开时**不该**报"门禁不放行"—— 那是误导：
    // 现场会去查联锁，而真实原因是这台机器根本没连上。
    measurement::FrameQueue queue;
    RecordingSink sink;
    measurement::Writer writer(sink, "TEST");
    auto monitor = make_quiet_monitor();
    measurement::MeasurementJob job(nullptr, nullptr, *monitor, queue, writer);
    job.configure(make_site(), [] { return make_measurement(); });
    const auto r = job.start(make_params());
    CHECK(!r);
    if (!r) CHECK_EQ(r.error().code, std::string("laser_not_open"));
}

// ── 纪律 2：失败不伪造 ──────────────────────────────────────────────────

CASE(capture_failure_is_reported_not_zero_filled); {
    // 采集失败**不终止循环**：激光还在打，光谱仪可能只是偶发丢一帧。
    // 一次失败就停，等于把偶发抖动放大成整条流程中断。
    //
    // 但失败也**绝不能**变成一条"平的谱"冒充数据：帧必须带错误码，
    // 通道数组为空（不是零值填充），UI 才能显示"这次没采到"。
    Fixture f;
    f.capture.fail_every = 1;              // 每次都失败
    f.job->set_capture_hook_for_test(f.capture.hook());
    const std::uint64_t sub = f.queue.subscribe();
    const libs::Result<measurement::MeasurementStartRecord> started = f.job->start(make_params());
    CHECK(started);
    // 等到第一帧（哪怕是失败帧）出现。
    measurement::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 100 && !got; ++i) {
        got = f.queue.try_pop(sub, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got);
    if (got) {
        CHECK(!frame.ok());
        CHECK(!frame.capture_error_code.empty());
        // 关键：不伪造谱。空的通道数组 = "没采到"，
        // 一条平的谱 = 谎报"采到了但是平的"。
        CHECK(frame.channels.empty());
    }
    (void)f.job->stop();
}

CASE(capture_failure_does_not_stop_the_loop); {
    // 每三帧失败一次：循环必须继续跑，失败帧与成功帧交替出现在订阅队列。
    Fixture f;
    f.capture.fail_every = 3;
    const std::uint64_t sub = f.queue.subscribe();
    const auto started = f.job->start(make_params());
    CHECK(started);

    int ok_frames = 0;
    int failed_frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < deadline && ok_frames < 2) {
        measurement::SpectrumFrame frame;
        if (f.queue.try_pop(sub, frame)) {
            if (frame.ok()) ++ok_frames; else ++failed_frames;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    // 采到过成功帧 = 循环没有被失败打断。
    CHECK(ok_frames >= 2);
    CHECK(f.job->running());
    (void)f.job->stop();
}

CASE(missing_measurement_provider_is_reported_honestly); {
    // 没有参数来源时**如实报错**，而不是用默认值凑一帧 ——
    // 默认积分时间采出来的谱，操作员会当成正确参数下的测量值。
    measurement::FrameQueue queue;
    RecordingSink sink;
    measurement::Writer writer(sink, "TEST");
    auto monitor = make_quiet_monitor();
    measurement::MeasurementJob job(nullptr, nullptr, *monitor, queue, writer);
    job.configure(make_site(), nullptr);   // provider 为空
    const auto r = job.start(make_params());
    CHECK(!r);
    if (!r) CHECK_EQ(r.error().code, std::string("laser_not_open"));
}

// ── 纪律 1b：运行期重判，中途失锁自行收光 ────────────────────────────────

// start() 里那次判定只是**快照**。连续出光可能一直打到手动停止，
// 中间有人打开了安全门 —— 事实变了，但没人再问，光就一直在打。
// 所以 loop() 周期性重判，且判定的执行方与收光的执行方必须是同一个对象：
// 不能靠 UI（可能崩/被关/卡住）。
CASE(gate_lost_mid_run_shuts_the_laser_off_by_itself); {
    // 采集代质放慢一点（每帧 60 ms）。不然的话一秒内就会堆满落盘队列（64 帧），
    // 落盘拥堵会把 last_error_code 覆盖成 disk_backlog_dropped，
    // 屏蔽了“运行期失锁”这个本测试真正要验的事。

    // 这里改的是 f.facts（monitor 事实源所引用的那份数据），
    // 所以下一个重判周期会真的看到联锁断开。
    Fixture f(/*allow=*/true);
    f.capture.hold_ms = 60;
    const auto started = f.job->start(make_params());
    CHECK(started);
    if (!started) {
        // 没出光就没话说“中途收了光”，直接返回让用例失败。
        return 0;
    }
    CHECK(f.job->running());

    // 先确认它确实在打光 —— 否则下面的“收了光”没意义。
    // 连续出光时光闸是**开**u7684，所以这里断言的是 q_gate_open == true。
    CHECK(f.link.q_gate_open);

    // 运行中拉开联锁。没人来调 stop，但事实变了。
    f.facts.laser.interlock_1 = false;

    // 重判周期是 kGateRecheckIntervalMs（500 ms），给四倍宽限。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(4 * static_cast<int>(measurement::kGateRecheckIntervalMs));
    while (std::chrono::steady_clock::now() < deadline && f.job->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 这里很明确：UI 不知道这件事，所以 running 必须自己变 false。
    CHECK(!f.job->running());
    // 进程自己把光关了——这是整条链路里最重要的一条。
    const auto cmds = f.link.write_commands();
    CHECK(!cmds.empty());
    bool saw_work_off = false;
    bool saw_gate_off = false;
    for (const std::uint8_t c : cmds) {
        if (c == libs::laser::command::kWorkState) saw_work_off = true;
        if (c == libs::laser::command::kQGate) saw_gate_off = true;
    }
    // 关键区分：start 时开一次光闸、收光时关一次。
    // 只看“发过过 kQGate”会误把开光闸当成收光，所以这里必须区分开/关。
    std::size_t gate_on = 0;
    std::size_t gate_off = 0;
    std::size_t work_on = 0;
    std::size_t work_off = 0;
    // FakeLink 把每一帧完整记在 sent，直接按帧内容做差分。
    for (const Bytes& frame : f.link.sent) {
        if (frame.size() < 5) continue;
        if (frame[0] != 0x5A || frame[1] != 0xA5) continue;
        const std::uint8_t cmd = frame[4];
        const Bytes payload(frame.begin() + 5, frame.end() - 2);
        if (cmd == libs::laser::command::kQGate) {
            if (!payload.empty() && payload[0] != 0x00) ++gate_on; else ++gate_off;
        } else if (cmd == libs::laser::command::kWorkState) {
            if (!payload.empty() && payload[0] != 0x00) ++work_on; else ++work_off;
        }
    }
    CHECK(gate_on >= 1);
    CHECK(gate_off >= 1);
    CHECK(work_on >= 1);
    CHECK(work_off >= 1);
    CHECK(saw_work_off);
    CHECK(saw_gate_off);

    // 运行期失锁必须留下可诊断的错误码，否则面守人只会看到“停了”
    // 而不知道是人停的还是联锁自己张的。
    const measurement::MeasurementStatus s = f.job->status();
    CHECK_EQ(s.last_error_code, std::string("gate_lost_mid_run"));
    (void)f.job->stop();
}

// 收光优先于任何其他事: 设备上的联锁断开也必须自己收光。
// 把失锁原因换成急停（联锁开了但急停没按）—— 现场很常见：
// 安全门一打开，操作员顺手拉了急停。两条路径都必须自己收光。
CASE(gate_lost_on_emergency_stop_also_shuts_the_laser_off); {
    Fixture f(/*allow=*/true);
    f.capture.hold_ms = 60;
    const auto started = f.job->start(make_params());
    CHECK(started);
    if (!started) return 0;   // 没出光就不能谈收光
    CHECK(f.link.q_gate_open);

    f.facts.laser.emergency_stop = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(4 * static_cast<int>(measurement::kGateRecheckIntervalMs));
    while (std::chrono::steady_clock::now() < deadline && f.job->running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!f.job->running());
    CHECK(!f.link.q_gate_open);   // 模拟器状态也已经是“关”
    const measurement::MeasurementStatus s = f.job->status();
    CHECK_EQ(s.last_error_code, std::string("gate_lost_mid_run"));
    (void)f.job->stop();
}

// ── 纪律 3：落盘异步、不反压 ────────────────────────────────────────────

CASE(frames_are_published_to_the_queue_and_delivered_to_disk); {
    // 一次成功采集 = 一个订阅者拿到一帧 + 落盘一次。
    // 这条锁住了"连续出光 → 每发被采到 → 落盘 → 显示"这条主链路的接缝。
    Fixture f;
    const std::uint64_t sub = f.queue.subscribe();
    const auto started = f.job->start(make_params());
    CHECK(started);

    measurement::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        got = f.queue.try_pop(sub, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got);
    if (got) {
        // 本流程每一帧都是激光触发的测量谱：laser_shot 恒为 true。
        // 这是把测量谱与背景谱在数据上分开的关键字段。
        CHECK(frame.laser_shot);
        CHECK(frame.ok());
        CHECK_EQ(frame.channels.size(), std::size_t(1));
    }
    // 落盘线程异步但最终会把这一帧写出去。
    CHECK(wait_for([&] { return f.sink.recorded_count() >= 1; }));
    const auto delivered = f.sink.take();
    CHECK(!delivered.empty());
    if (!delivered.empty()) {
        // 载荷必须含完整 wavelength_nm + counts。只存 shot_id / 通道名
        // 一类元数据等于存了个空壳 —— 那是"数据只活在管道响应里"的成因。
        CHECK(delivered.front().blob.find("wavelength_nm") != std::string::npos);
        CHECK(delivered.front().blob.find("counts") != std::string::npos);
        CHECK_EQ(delivered.front().ref.station_id, std::string("TEST"));
    }
    (void)f.job->stop();
}

CASE(slow_disk_does_not_backpressure_the_capture_loop); {
    // 纪律 3 的核心：写盘慢 **不能** 拖慢出光节奏。
    // 判据：把落盘故意拖成 200 ms 一帧，采集仍能在 800 ms 内采到多帧。
    // 如果落盘是同步的，这个断言必挂 —— 采集会被磁盘一块儿拖死。
    Fixture f;
    f.sink.delay_ms = 200;
    const std::uint64_t sub = f.queue.subscribe();
    const auto started = f.job->start(make_params());
    CHECK(started);

    int frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < deadline && frames < 5) {
        measurement::SpectrumFrame frame;
        if (f.queue.try_pop(sub, frame)) ++frames;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // 200 ms 一帧的磁盘，800 ms 里至少还能采到 3 帧 = 采集没被磁盘反压。
    CHECK(frames >= 3);
    (void)f.job->stop();
}

CASE(disk_failure_is_counted_not_hidden); {
    // 写盘失败**如实计数**并暴露在 status，绝不假装"每发都存了"。
    // 现场"以为存了其实没存"是丢数据的头号成因。
    Fixture f;
    f.sink.fail_once();
    const auto started = f.job->start(make_params());
    CHECK(started);
    // 等到落盘跑过一次。
    CHECK(wait_for([&] { return f.sink.call_count() >= 1; }));
    const measurement::MeasurementStatus s = f.job->status();
    CHECK(s.disk_writes_failed >= 1);
    (void)f.job->stop();
}

// ── 纪律 4：停止无条件、可重复 ─────────────────────────────────────────

CASE(stop_without_start_is_safe); {
    // 从没 start 过就 stop：必须**安全地**返回，而不是崩或者挂死。
    measurement::FrameQueue queue;
    RecordingSink sink;
    measurement::Writer writer(sink, "TEST");
    auto monitor = make_quiet_monitor();
    measurement::MeasurementJob job(nullptr, nullptr, *monitor, queue, writer);
    job.configure(make_site(), [] { return make_measurement(); });
    const auto r = job.stop();
    CHECK(!r);
    if (!r) CHECK_EQ(r.error().code, std::string("laser_not_open"));
}

CASE(stop_is_idempotent); {
    // 重复 stop 必须安全：守护进程退出、UI 点停止、作业自己收尾，
    // 这三处都会调 stop。任何一处抛异常或死锁就是现场事故。
    Fixture f;
    f.job->stop();
    f.job->stop();
    f.job->stop();
    CHECK(!f.job->running());
}

CASE(stop_confirms_the_laser_is_shut_down); {
    // 停止后读回确认：光闸关、已停机。收光没确认 = 最严重的失败，
    // 因为设备自己保持"工作"状态，软件关了光还在打。
    Fixture f;
    const auto started = f.job->start(make_params());
    CHECK(started);
    const auto stopped = f.job->stop();
    CHECK(stopped);
    CHECK(!f.job->running());
    // 写指令里必须有停机与关光闸。
    const auto cmds = f.link.write_commands();
    bool saw_work_off = false;
    bool saw_gate_off = false;
    for (const std::uint8_t c : cmds) {
        if (c == libs::laser::command::kWorkState) saw_work_off = true;
        if (c == libs::laser::command::kQGate) saw_gate_off = true;
    }
    CHECK(saw_work_off);
    CHECK(saw_gate_off);
}

CASE(stop_clears_pending_frames_for_subscribers); {
    // 停流后清空订阅者队列：留着这些帧没意义（作业已收光，
    // 继续显示"最后一帧"会让人以为还在测）。
    Fixture f;
    const std::uint64_t sub = f.queue.subscribe();
    const auto started = f.job->start(make_params());
    CHECK(started);
    // 让它先产几帧。
    measurement::SpectrumFrame tmp;
    for (int i = 0; i < 50; ++i) {
        if (f.queue.try_pop(sub, tmp)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)f.job->stop();
    CHECK_EQ(f.queue.pending(sub), std::size_t(0));
}

CASE(stop_does_not_block_on_a_capture_waiting_for_ttl); {
    // stop() 时，采集线程可能正卡在 capture 上等一个 TTL（最长 timeout_ms）。
    // stop 必须**先收光**让循环自然退出，而不是卡在 join 上等满超时。
    // 这里 capture 替身 hold 400 ms（模拟在等 TTL），stop 必须在
    // 明显少于 hold+timeout 的时间内返回。
    Fixture f;
    f.capture.hold_ms = 400;
    const auto started = f.job->start(make_params());
    CHECK(started);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));   // 让它进 capture
    const auto t0 = std::chrono::steady_clock::now();
    (void)f.job->stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // stop 应该只是收光 + 等最后一次 capture 自然超时返回，
    // 总时长量级就是 hold 本身，不该叠加成好几倍。
    CHECK(elapsed < std::chrono::milliseconds(2000));
}

CASE(destructor_shuts_down_the_laser); {
    // 析构必须先收光再停线程。光留在外面是最严重的后果。
    FakeLink link;
    std::unique_ptr<libs::devices::Laser> laser =
        std::make_unique<libs::devices::Laser>(link);
    measurement::FrameQueue queue;
    RecordingSink sink;
    measurement::Writer writer(sink, "TEST");
    readiness::ReadinessFacts facts = safe_readiness_facts();
    auto monitor = make_allowing_monitor(&facts);
    {
        measurement::MeasurementJob job(laser.get(), nullptr, *monitor, queue, writer);
        job.configure(make_site(), [] { return make_measurement(); });
        // 从没 start，析构也不该崩 —— 它 stop() 一次，找不到设备就报错返回。
    }
    CHECK(true);   // 没崩就是通过；真收光的覆盖见 stop_confirms_the_laser_is_shut_down
}

// ── status / to_json ───────────────────────────────────────────────────

CASE(status_reports_all_counters); {
    // status 必须能答"累计采了多少、落盘积压多少" ——
    // 这是操作员判断"是不是磁盘跟不上"的唯一依据。
    Fixture f;
    const measurement::MeasurementStatus s = f.job->status();
    CHECK(!s.running);
    CHECK_EQ(s.shots_captured, std::uint64_t(0));
    CHECK_EQ(s.shots_failed, std::uint64_t(0));
    CHECK_EQ(s.disk_writes_ok, std::uint64_t(0));
    CHECK_EQ(s.disk_writes_failed, std::uint64_t(0));
    CHECK_EQ(s.disk_backlog, std::uint64_t(0));
}

CASE(status_json_carries_every_field_the_ui_reads); {
    // JSON 字段名是 UI 契约。漏一个字段，UI 就少一个数，
    // 而"少显示积压"是那种没人会发现的问题。
    Fixture f;
    const json::Value v = measurement::to_json(f.job->status());
    CHECK(v.has("running"));
    CHECK(v.has("shots_captured"));
    CHECK(v.has("shots_failed"));
    CHECK(v.has("disk_writes_ok"));
    CHECK(v.has("disk_writes_failed"));
    CHECK(v.has("disk_backlog"));
    CHECK(v.has("last_error_code"));
}

CASE(status_is_callable_before_anything_is_configured); {
    // 没有设备、没有配置、什么都没装配时 status 必须能答 ——
    // UI 的状态面板一上来就问它。
    measurement::FrameQueue queue;
    RecordingSink sink;
    measurement::Writer writer(sink, "TEST");
    auto monitor = make_quiet_monitor();
    measurement::MeasurementJob job(nullptr, nullptr, *monitor, queue, writer);
    (void)job.status();
    (void)job.running();
}

CASE(sdk_channels_are_armed_before_laser_gate_opens); {
    const char* path = std::getenv("LIBS_FAKE_AVASPEC_DLL");
    CHECK(path != nullptr);
    if (path) {
        HMODULE module = LoadLibraryA(path);
        CHECK(module != nullptr);
        if (module) {
            const auto reset = fake_export<void (*)(int)>(module, "FakeReset");
            const auto counter = fake_export<int (*)(int)>(module, "FakeCounter");
            CHECK(reset && counter);
            if (reset && counter) {
                reset(0);
                {
                    Fixture f;
                    auto spec = libs::devices::Spectrometer::open(path);
                    CHECK(spec);
                    if (spec) {
                        f.site.spectrometer.serials = {"MASTER", "SLAVE1", "SLAVE2"};
                        f.site.spectrometer.master_serial = "MASTER";
                        f.job->configure(f.site, [] { return make_measurement(); });
                        f.job->bind_devices(f.laser.get(), spec.value().get());
                        f.job->set_capture_hook_for_test({});
                        bool opened_after_arming = false;
                        f.link.on_gate_open = [&] { opened_after_arming = counter(1) == 3; };
                        CHECK(f.job->start(make_params()));
                        CHECK(opened_after_arming);
                        CHECK(f.job->stop());
                        CHECK(spec.value()->close());
                        f.job->bind_devices(f.laser.get(), nullptr);
                    }
                }
                reset(1);
                {
                    Fixture f;
                    auto spec = libs::devices::Spectrometer::open(path);
                    CHECK(spec);
                    if (spec) {
                        f.site.spectrometer.serials = {"MASTER", "SLAVE1", "SLAVE2"};
                        f.site.spectrometer.master_serial = "MASTER";
                        f.job->configure(f.site, [] { return make_measurement(); });
                        f.job->bind_devices(f.laser.get(), spec.value().get());
                        f.job->set_capture_hook_for_test({});
                        bool gate_opened = false;
                        f.link.on_gate_open = [&] { gate_opened = true; };
                        CHECK(!f.job->start(make_params()));
                        CHECK(!gate_opened);
                        CHECK(!f.link.work_state); CHECK(!f.link.q_gate_open);
                        CHECK(!f.job->running());
                        CHECK(counter(2) >= 3);
                        reset(0); // Disable the persistent injected prepare failure before quiet close.
                        CHECK(spec.value()->close());
                        f.job->bind_devices(f.laser.get(), nullptr);
                    }
                }
                reset(0);
            }
            FreeLibrary(module);
        }
    }
}
CASE(stop_cancels_pending_sdk_capture_without_waiting_for_ttl_timeout); {
    const char* path = std::getenv("LIBS_FAKE_AVASPEC_DLL");
    if (path) {
        HMODULE module = LoadLibraryA(path);
        const auto reset = fake_export<void (*)(int)>(module, "FakeReset");
        CHECK(reset);
        if (reset) {
            reset(4);
            {
                Fixture f;
                auto spec = libs::devices::Spectrometer::open(path);
                CHECK(spec);
                if (spec) {
                    f.site.spectrometer.serials = {"MASTER", "SLAVE1", "SLAVE2"};
                    f.site.spectrometer.master_serial = "MASTER";
                    f.job->configure(f.site, [] { return make_measurement(); });
                    f.job->bind_devices(f.laser.get(), spec.value().get());
                    f.job->set_capture_hook_for_test({});
                    auto params = make_params(); params.capture_timeout_ms = 1500;
                    CHECK(f.job->start(params));
                    const auto begin = std::chrono::steady_clock::now();
                    CHECK(f.job->stop());
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - begin).count();
                    CHECK(elapsed < 500);
                    CHECK(!f.link.q_gate_open); CHECK(!f.link.work_state);
                    CHECK(spec.value()->close());
                    f.job->bind_devices(f.laser.get(), nullptr);
                }
            }
            reset(0);
        }
        if (module) FreeLibrary(module);
    }
}
CASE(interlock_changed_during_preparation_never_opens_gate); {
    Fixture f;
    f.link.on_work_on = [&] { f.facts.laser.interlock_1 = false; };
    bool opened = false;
    f.link.on_gate_open = [&] { opened = true; };
    const auto started = f.job->start(make_params());
    CHECK(!started);
    if (!started) CHECK_EQ(started.error().code, std::string("gate_lost_during_startup"));
    CHECK(!opened); CHECK(!f.link.work_state); CHECK(!f.job->running());
}
CASE(invalid_rate_is_rejected_before_device_writes); {
    Fixture f;
    auto params = make_params();
    params.laser.frequency_hz = 10;
    params.integration_ms = 100;
    const auto started = f.job->start(params);
    CHECK(!started);
    if (!started) CHECK_EQ(started.error().code, std::string("laser_spectrometer_rate_mismatch"));
    CHECK(f.link.write_commands().empty());
}
CASE(worker_exception_closes_gate_and_allows_stop); {
    Fixture f;
    f.job->set_capture_hook_for_test([]() -> libs::Result<libs::devices::CaptureResult> {
        throw std::runtime_error("test capture exception");
    });
    (void)f.job->start(make_params());
    CHECK(f.job->stop());
    CHECK(!f.link.q_gate_open); CHECK(!f.link.work_state);
}
CASE(spectral_request_override_applies_to_following_frames); {
    Fixture f;
    const auto subscriber = f.queue.subscribe();
    auto params = make_params();
    params.capture_timeout_ms = 777;
    params.override_timeout = true;
    CHECK(f.job->start(params));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    measurement::SpectrumFrame frame;
    CHECK(f.queue.try_pop(subscriber, frame));
    CHECK_EQ(frame.timeout_ms, 777u);
    CHECK(f.queue.try_pop(subscriber, frame));
    CHECK_EQ(frame.timeout_ms, 777u);
    CHECK(f.job->stop());
}
CASE(measurement_can_stop_and_start_again); {
    Fixture f;
    CHECK(f.job->start(make_params())); CHECK(f.job->stop());
    CHECK(f.job->start(make_params())); CHECK(f.job->stop());
}

TEST_MAIN_RETURN()
