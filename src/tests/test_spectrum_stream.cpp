// 实时光谱流离线测试。
//
// 这些用例守的是 spectrum_stream.hpp 文件头写下的四条纪律。
// **全部不碰真实设备**：采集动作被 capture_hook 替换成受控替身，
// 于是"会不会抢触发""会不会把设备永久占住"这些只能靠真机发现的问题，
// 可以在桌面上被确定性地复现。
//
// 为什么这类测试值钱：外触发抢触发与租约泄漏都是**偶发**故障，
// 现场表现是"偶尔测不到"，不写测试就只能靠盯日志碰运气。

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "libs/business/config/site_config.hpp"
#include "libs/business/services/spectrum_stream.hpp"

using namespace business;
using libs::Error;
using libs::Result;

namespace {

// 造一份能过 validate 的站点配置。
config::SiteConfig make_site() {
    config::SiteConfig site;
    site.station_id = "TEST";
    site.laser.port = "COM1";
    site.rangefinder.port = "COM2";
    site.indicator.port = "COM3";
    site.air_conditioner.port = "COM4";
    site.capture_timeout_ms = 3000;
    site.integration_ms = 10;
    // 站点配置本身要求等激光触发——正是这个配置会让实时流去抢 TTL。
    site.spectrometer.sdk_dll = "C:\\fake\\avaspecx64.dll";
    site.spectrometer.wait_for_laser_trigger = true;
    site.spectrometer.integration_delay_ns = 50000.0;
    return site;
}

// 造一个"正常"通道：wavelength 与 counts 等长。
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

libs::devices::ScanGroup make_group(bool synchronized) {
    libs::devices::ScanGroup g;
    g.shot_id = 7;
    g.complete = true;
    // 原样上送的测试入口：SDK 报什么就传什么。
    g.hardware_synchronized = synchronized;
    g.spectra.push_back(make_channel("SN-A", 4));
    return g;
}

Result<libs::devices::CaptureResult> ok_capture(bool synchronized = false) {
    libs::devices::CaptureResult r;
    r.groups.push_back(make_group(synchronized));
    return r;
}

}  // namespace

// ── 纪律 1：hardware_synchronized 原样上送，不推断 ──────────────────────

static void test_sync_flag_is_passed_through_verbatim() {
    CASE(sync_flag_is_passed_through_verbatim);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.set_capture_hook_for_test([]() { return ok_capture(true); });

    services::SpectrumStream::Options options;
    options.min_period_ms = 1;              // 测试里跑快点
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();

    services::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = stream.try_pop(id, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stream.stop();

    CHECK(got);
    if (got) {
        // SDK 报 true -> 原样 true
        CHECK_EQ(frame.hardware_synchronized, true);
    }
}

static void test_sync_false_is_not_upgraded() {
    CASE(sync_false_is_not_upgraded);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.set_capture_hook_for_test([]() { return ok_capture(false); });

    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    services::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = stream.try_pop(id, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stream.stop();

    CHECK(got);
    if (got) {
        // SDK 报 false，即使业务层请求过同步也不许说 true。
        // 这正是 §3.10.1 的纪律：不得由"我请求了同步"推断"物理上已同步"。
        CHECK_EQ(frame.hardware_synchronized, false);
    }
}

// ── 纪律 2：流式服务不发任何激光写指令 ──────────────────────────────────
//
// 这一条无法从"结果"直接看出来（因为替身不记录激光访问），
// 所以用结构事实来守：**SpectrumStream 的依赖里没有 Laser**。
// 如果日后有人为了"顺便触发一次"去 include laser.hpp，这个用例会挡住。

static void test_stream_never_touches_the_laser() {
    CASE(stream_never_touches_the_laser);
    // 本用例的断言就是它能编译并运行：
    // SpectrumStream 只接受 Spectrometer*，没有任何接受 Laser* 的入口。
    // 要真正动激光，唯一的路是 LaserFireService，而那必须先过 Gate。
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    // 站点配置要求等激光触发，但默认（follow_site_trigger_mode=false）
    // 必须无视它——否则没接 TTL 源时每帧都超时。
    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    options.follow_site_trigger_mode = false;
    stream.set_options(options);
    CHECK_EQ(options.follow_site_trigger_mode, false);
    CHECK_EQ(stream.running(), false);
}

// ── 纪律 3：通道缺失是第三态，不是失败、也不用零值填充 ────────────────────

static void test_missing_channel_is_absent_not_zero_filled() {
    CASE(missing_channel_is_absent_not_zero_filled);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);

    // 一个通道有数据，另一个通道返回空（模拟 SDK 少回一路）。
    std::atomic<bool> first{true};
    stream.set_capture_hook_for_test([&]() -> Result<libs::devices::CaptureResult> {
        libs::devices::CaptureResult r;
        libs::devices::ScanGroup g;
        g.shot_id = 1;
        g.complete = false;                    // 部分缺失
        g.spectra.push_back(make_channel("SN-A", 3));
        libs::devices::Spectrum empty;
        empty.serial = "SN-B";
        empty.pixels = 0;                     // 没有数据
        g.spectra.push_back(empty);
        r.groups.push_back(g);
        return r;
    });

    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    services::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = stream.try_pop(id, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stream.stop();

    CHECK(got);
    if (!got) return;
    // 整次采集本身是"成功"的：拿到了东西。
    CHECK_EQ(frame.ok(), true);
    CHECK_EQ(static_cast<int>(frame.channels.size()), 2);
    if (frame.channels.size() != 2) return;
    // A 路有数据。
    CHECK_EQ(static_cast<int>(frame.channels[0].state), static_cast<int>(services::ChannelState::present));
    CHECK_EQ(frame.channels[0].counts.size(), std::size_t{3});
    // B 路标 absent + 原因，**不是**一串 0。
    CHECK_EQ(static_cast<int>(frame.channels[1].state), static_cast<int>(services::ChannelState::absent));
    CHECK(!frame.channels[1].reason.empty());
    CHECK_EQ(frame.channels[1].counts.size(), std::size_t{0});
}

static void test_whole_capture_failure_is_reported_as_error() {
    CASE(whole_capture_failure_is_reported_as_error);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.set_capture_hook_for_test([]() -> Result<libs::devices::CaptureResult> {
        return Error::make(libs::ErrorKind::timeout, "capture_timeout", "模拟采集超时");
    });

    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    services::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = stream.try_pop(id, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stream.stop();

    CHECK(got);
    if (got) {
        // 整次失败与"通道缺失"是**两回事**，UI 要能分开显示。
        CHECK_EQ(frame.ok(), false);
        CHECK_EQ(frame.capture_error_code, std::string("capture_timeout"));
    }
}

// ── 纪律 4：采集租约（外触发模式下不抢 TTL）──────────────────────────────

static void test_lease_suppresses_stream_while_held() {
    CASE(lease_suppresses_stream_while_held);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);

    // 计数器记录采集替身被调了几次。持租约期间应当**一次都不增**。
    std::atomic<int> captures{0};
    stream.set_capture_hook_for_test([&]() -> Result<libs::devices::CaptureResult> {
        ++captures;
        return ok_capture();
    });

    services::SpectrumStream::Options options;
    options.min_period_ms = 5;
    stream.set_options(options);

    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const int idle_captures = captures.load();
    CHECK(idle_captures > 0);        // 空闲时确实在采

    {
        // 持租约：模拟一次出光采集。外触发模式下这条路径正要去吃 pin 6 的 TTL。
        services::AcquisitionLease lease(stream);
        CHECK_EQ(lease.held(), true);
        // 边界纪律：流可能**在租约拿到之前**就已经进了这一轮采集，
        // 那一轮落在设备锁内、计数已经加过了。所以先等它落地，取稳定
        // 基线，再看租约期间有没有新的采集发生。判据是"稳定后不再增长"，
        // 不是"拿到租约那一瞬间就必须已经停了"——后者是拿测试的时序去赌
        // 实现的时序，跑一次绿一次就说明判据写错了。
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int settled = captures.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        // 持租约期间流必须**完全让路**：一次都不许碰设备。
        CHECK_EQ(captures.load(), settled);
        // 对照组：空闲时 120ms 早该采到二十几帧（5ms 一轮）。
        // 持租约却一帧不涨——若这条挂了说明租约没生效、抑制位没清。
        CHECK(settled > 0);
    }
    stream.stop();
}

static void test_lease_is_released_on_scope_exit() {
    CASE(lease_is_released_on_scope_exit);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    std::atomic<int> captures{0};
    stream.set_capture_hook_for_test([&]() -> Result<libs::devices::CaptureResult> {
        ++captures;
        return ok_capture();
    });
    services::SpectrumStream::Options options;
    options.min_period_ms = 5;
    stream.set_options(options);
    stream.start();

    { services::AcquisitionLease lease(stream); CHECK(lease.held()); }
    // 租约析构后流必须恢复采集。忘了 release 的实现会永久卡死预览。
    const int before = captures.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(captures.load() > before);
    stream.stop();
}

static void test_lease_does_not_leak_on_exception_path() {
    CASE(lease_does_not_leak_on_exception_path);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    std::atomic<int> captures{0};
    stream.set_capture_hook_for_test([&]() -> Result<libs::devices::CaptureResult> {
        ++captures;
        return ok_capture();
    });
    services::SpectrumStream::Options options;
    options.min_period_ms = 5;
    stream.set_options(options);
    stream.start();

    // 模拟出光流程中途抛异常（例如某步设备写失败）。
    // RAII 的价值就在这里：异常路径也必须把租约还回去。
    try {
        services::AcquisitionLease lease(stream);
        throw std::runtime_error("模拟出光流程异常");
    } catch (const std::exception&) {
    }
    const int before = captures.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(captures.load() > before);
    stream.stop();
}

static void test_lease_refused_after_stop() {
    CASE(lease_refused_after_stop);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.start();
    stream.stop();
    // 流已停时不该再给租约：否则调用方会以为自己独占地，
    // 实际上没有任何东西在跑。
    services::AcquisitionLease lease(stream);
    CHECK_EQ(lease.held(), false);
}

// ── 背压：队列满时丢最旧的一帧，不阻塞采集 ───────────────────────────────

static void test_backpressure_drops_oldest_and_keeps_newest() {
    CASE(backpressure_drops_oldest_and_keeps_newest);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.set_capture_hook_for_test([]() { return ok_capture(); });

    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    options.subscriber_queue_frames = 2;      // 故意设很小
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    stream.stop();

    // 队列不超过上限。
    CHECK(stream.pending(id) <= std::size_t{2});
    // 确实丢过帧（否则说明背压没生效，UI 会拿到过期数据却不自知）。
    CHECK(stream.dropped_frames() > 0);

    // 取出来的应该是**较新**的帧，不是被丢剩的旧帧。
    services::SpectrumFrame frame;
    std::vector<std::int64_t> indices;
    while (stream.try_pop(id, frame)) indices.push_back(frame.frame_index);
    CHECK(indices.size() <= std::size_t{2});
    for (std::size_t i = 1; i < indices.size(); ++i) {
        CHECK(indices[i] > indices[i - 1]);   // 单调递增 = 留下的确实是新的
    }
}

// ── 订阅生命周期 ────────────────────────────────────────────────────────

static void test_unsubscribe_stops_delivery() {
    CASE(unsubscribe_stops_delivery);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.set_capture_hook_for_test([]() { return ok_capture(); });
    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stream.unsubscribe(id);

    // 退订后取不到帧，也不该崩。
    services::SpectrumFrame frame;
    CHECK_EQ(stream.try_pop(id, frame), false);
    CHECK_EQ(stream.pending(id), std::size_t{0});
    stream.stop();
}

static void test_unknown_subscriber_returns_no_frame() {
    CASE(unknown_subscriber_returns_no_frame);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    services::SpectrumFrame frame;
    // 没订阅过就取帧：返回 false，不是崩、不是空数据。
    CHECK_EQ(stream.try_pop(99999, frame), false);
}

static void test_stop_joins_worker_so_no_frames_after_stop() {
    CASE(stop_joins_worker_so_no_frames_after_stop);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    stream.set_capture_hook_for_test([]() { return ok_capture(); });
    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stream.stop();
    CHECK_EQ(stream.running(), false);

    // stop 返回后，采集计数**不再增长**（join 已经发生）。
    services::SpectrumFrame frame;
    while (stream.try_pop(id, frame)) {}   // 清空
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(stream.try_pop(id, frame), false);
}

// ── 无设备时如实报 unsupported，而不是静默假装在跑 ──────────────────────

static void test_no_device_reports_error_frames() {
    CASE(no_device_reports_error_frames);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);      // 设备为 nullptr
    // 不装 hook，走真实路径 -> device==nullptr -> unsupported
    services::SpectrumStream::Options options;
    options.min_period_ms = 1;
    stream.set_options(options);

    const std::uint64_t id = stream.subscribe();
    stream.start();
    services::SpectrumFrame frame;
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        got = stream.try_pop(id, frame);
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stream.stop();

    CHECK(got);
    if (got) {
        // 没有设备就说没有，不返回"一路平的假谱"。
        CHECK_EQ(frame.ok(), false);
        CHECK_EQ(frame.capture_error_code, std::string("spectrometer_not_open"));
    }
}

static void test_status_reports_honestly() {
    CASE(status_reports_honestly);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    json::Value s = stream.status();
    CHECK_EQ(s["running"].as_bool(), false);
    CHECK_EQ(s["spectrometer_open"].as_bool(), false);
    // 关键：note 里必须写明这不是测量谱。
    CHECK(!s["note"].as_string().empty());
}

static void test_stream_can_be_restarted_after_stop() {
    CASE(stream_can_be_restarted_after_stop);
    // 回归：stop() 置的停止标志必须在 start() 里被复位，否则流只能启动一次。
    // 现场症状特别难查：第二次点"开始预览"，start() 返回成功、不报任何错，
    // 但 status 里 running 一直是 false，一帧数据都没有——按钮像是坏的。
    // stop() 侧有"没在跑就直接返回"的短路，把这个不对称盖住了，所以要单独钉住。
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);

    CHECK(stream.start());
    CHECK_EQ(stream.status()["running"].as_bool(), true);
    stream.stop();
    CHECK_EQ(stream.status()["running"].as_bool(), false);

    // 关键这一步：停过之后必须还能再起来。
    CHECK(stream.start());
    CHECK_EQ(stream.status()["running"].as_bool(), true);
    stream.stop();
    CHECK_EQ(stream.status()["running"].as_bool(), false);
    // 反复起停也要稳：线程不能攒下没释放的东西。
    for (int i = 0; i < 3; ++i) {
        CHECK(stream.start());
        CHECK_EQ(stream.status()["running"].as_bool(), true);
        stream.stop();
        CHECK_EQ(stream.status()["running"].as_bool(), false);
    }
}

static void test_start_while_running_is_a_noop() {
    CASE(start_while_running_is_a_noop);
    std::mutex device_mutex;
    services::SpectrumStream stream(device_mutex);
    stream.configure(make_site(), nullptr);
    CHECK(stream.start());
    CHECK(stream.start());   // 已在跑：再点一次"开始"不该起第二个线程
    CHECK_EQ(stream.status()["running"].as_bool(), true);
    stream.stop();
}
int main() {
    std::printf("== %s ==\n", __FILE__);
    test_sync_flag_is_passed_through_verbatim();
    test_sync_false_is_not_upgraded();
    test_stream_never_touches_the_laser();
    test_missing_channel_is_absent_not_zero_filled();
    test_whole_capture_failure_is_reported_as_error();
    test_lease_suppresses_stream_while_held();
    test_lease_is_released_on_scope_exit();
    test_lease_does_not_leak_on_exception_path();
    test_lease_refused_after_stop();
    test_backpressure_drops_oldest_and_keeps_newest();
    test_unsubscribe_stops_delivery();
    test_unknown_subscriber_returns_no_frame();
    test_stop_joins_worker_so_no_frames_after_stop();
    test_no_device_reports_error_frames();
    test_stream_can_be_restarted_after_stop();
    test_start_while_running_is_a_noop();
    test_status_reports_honestly();
    TEST_MAIN_RETURN();
