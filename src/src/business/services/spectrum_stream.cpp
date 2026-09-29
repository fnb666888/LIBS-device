#include "libs/business/services/spectrum_stream.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "libs/business/time_util.hpp"

namespace business::services {

const char* to_string(ChannelState state) noexcept {
    switch (state) {
        case ChannelState::present: return "present";
        case ChannelState::absent:  return "absent";
    }
    return "absent";
}

json::Value to_json(const ChannelFrame& channel) {
    json::Value v = json::object();
    v.set("serial", json::Value(channel.serial));
    // state 是三态里的"状态"那一个，与 counts 的数值**不是一回事**。
    // 缺失时写 "absent"，不是写一个空的 counts 数组——后者会被 UI
    // 当成"这一路是平的"。
    v.set("state", json::Value(to_string(channel.state)));
    if (channel.state == ChannelState::absent) {
        v.set("reason", json::Value(channel.reason));
        return v;
    }
    v.set("pixels", json::Value(static_cast<double>(channel.pixels)));

    // 波长与 counts 是两个等长数组。原样上送，**不做归一化/基线扣除/平均**。
    // UI 要归一化自己算：把显示口径写死在业务层，改口径就得改库。
    json::Array wavelengths;
    json::Array counts;
    wavelengths.reserve(channel.wavelength_nm.size());
    counts.reserve(channel.counts.size());
    for (const double w : channel.wavelength_nm) wavelengths.push_back(json::Value(w));
    for (const double c : channel.counts) counts.push_back(json::Value(c));
    v.set("wavelength_nm", json::Value(std::move(wavelengths)));
    v.set("counts", json::Value(std::move(counts)));
    return v;
}

json::Value to_json(const SpectrumFrame& frame) {
    json::Value v = json::object();
    v.set("frame_index", json::Value(static_cast<double>(frame.frame_index)));
    v.set("shot_id", json::Value(static_cast<double>(frame.shot_id)));
    v.set("wall_clock_ms", json::Value(static_cast<double>(frame.wall_clock_ms)));
    v.set("captured_monotonic_ms", json::Value(static_cast<double>(frame.captured_monotonic_ms)));
    // 同步事实原样上送（§3.10.1），不合并成一个布尔。
    v.set("hardware_synchronized", json::Value(frame.hardware_synchronized));
    v.set("requested_sync", json::Value(frame.requested_sync));
    v.set("period_ms", json::Value(frame.period_ms));
    v.set("timeout_ms", json::Value(static_cast<double>(frame.timeout_ms)));
    v.set("ok", json::Value(frame.ok()));
    if (!frame.ok()) {
        v.set("capture_error_code", json::Value(frame.capture_error_code));
        v.set("capture_error_message", json::Value(frame.capture_error_message));
    }
    json::Array channels;
    for (const auto& channel : frame.channels) channels.push_back(to_json(channel));
    v.set("channels", json::Value(std::move(channels)));
    return v;
}

// ── AcquisitionLease ──────────────────────────────────────────────────────

AcquisitionLease::AcquisitionLease(SpectrumStream& stream) : stream_(stream) {
    held_ = stream_.try_acquire_lease();
}

AcquisitionLease::~AcquisitionLease() {
    if (held_) stream_.release_lease();
}

// ── SpectrumStream ────────────────────────────────────────────────────────

SpectrumStream::SpectrumStream(std::mutex& device_mutex) : device_mutex_(device_mutex) {}

SpectrumStream::~SpectrumStream() { stop(); }

void SpectrumStream::configure(const config::SiteConfig& site, libs::devices::Spectrometer* device) {
    std::lock_guard<std::mutex> lock(mutex_);
    site_ = site;
    device_ = device;
}

void SpectrumStream::set_options(Options options) {
    std::lock_guard<std::mutex> lock(mutex_);
    options_ = std::move(options);
}

libs::VoidResult SpectrumStream::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) return libs::VoidResult();

    // 设备为 nullptr 时**允许 start**（跑起来、如实报 unsupported），
    // 而不是拒绝启动：这样 UI 能在守护进程里看到"流已开但设备没接"，
    // 而不是看到一条起不来的流然后猜为什么。现场排障要的是能看见。
    // 清掉上一次 stop() 留下的停止标志，**这一行是"能重新启动"的全部要害**。
    // 少了它，stop() 置的 stopping_=true 永远留着，这里的 if 之后再也不成立，
    // 流就再也起不来了——而 start() 照样返回成功，现场表现是
    // "点了开始预览，status 说 running=false，也没报错"，最难查的一种坏。
    // stop() 侧靠"没在跑就直接返回"看不出这个不对称，所以必须在这边复位。
    stopping_.store(false);
    running_.store(true);
    worker_ = std::thread([this]() { loop(); });
    return libs::VoidResult();
}

void SpectrumStream::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load() && !worker_.joinable()) return;
        stopping_.store(true);
        running_.store(false);
    }
    // join 必须在锁外：worker 线程自己要取锁（publish/状态更新），
    // 持锁 join 会直接死锁。
    if (worker_.joinable()) worker_.join();
    // 停掉之后把订阅者队列清空：留着这些帧没有意义，
    // 下次 start() 会重新计数 frame_index。
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : subscribers_) entry.second.queue.clear();
}

bool SpectrumStream::running() const noexcept { return running_.load(); }

std::uint64_t SpectrumStream::dropped_frames() const noexcept { return dropped_frames_.load(); }

std::uint64_t SpectrumStream::subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t id = next_subscriber_id_++;
    subscribers_[id] = Subscriber{};
    return id;
}

void SpectrumStream::unsubscribe(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(id);
}

bool SpectrumStream::try_pop(std::uint64_t id, SpectrumFrame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(id);
    if (it == subscribers_.end() || it->second.queue.empty()) return false;
    out = std::move(it->second.queue.front());
    it->second.queue.pop_front();
    return true;
}

std::size_t SpectrumStream::pending(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(id);
    return it == subscribers_.end() ? 0 : it->second.queue.size();
}

json::Value SpectrumStream::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value v = json::object();
    v.set("running", json::Value(running_.load()));
    v.set("subscribers", json::Value(static_cast<double>(subscribers_.size())));
    v.set("frame_index", json::Value(static_cast<double>(next_frame_index_)));
    v.set("delivered_frames", json::Value(static_cast<double>(delivered_frames_)));
    v.set("dropped_frames", json::Value(static_cast<double>(dropped_frames_)));
    v.set("lease_held", json::Value(lease_count_ > 0));
    v.set("spectrometer_open", json::Value(device_ != nullptr));
    v.set("follow_site_trigger_mode", json::Value(options_.follow_site_trigger_mode));
    v.set("min_period_ms", json::Value(static_cast<double>(options_.min_period_ms)));
    if (!last_error_code.empty()) {
        v.set("last_error_code", json::Value(last_error_code));
        v.set("last_error_message", json::Value(last_error_message));
    } else {
        // 没有错误就明确写"无"，而不是省略字段让调用方猜。
        v.set("last_error_code", json::Value());   // null
    }
    v.set("note", json::Value("流式服务不发任何激光写指令；默认自由触发，拍到的是背景谱，"
                              "不是 LIBS 测量谱（真实谱需 laser_fire 走门禁后出光采集）"));
    return v;
}

bool SpectrumStream::try_acquire_lease() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load()) return false;
    ++lease_count_;
    return true;
}

void SpectrumStream::release_lease() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lease_count_ > 0) --lease_count_;
}

void SpectrumStream::loop() {
    while (!stopping_.load()) {
        // 租约被一次性采集（出光/单次 capture）持有时，**跳过这一轮**。
        // 不排队、不等待：见文件头背压说明。流的价值是节奏稳定。
        bool skip = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            skip = lease_count_ > 0;
        }
        if (!skip) {
            const SpectrumFrame frame = acquire_frame();
            publish(frame);
        }

        // 睡到下一帧。sleep 期间不持锁：stop() 要能立刻把标志位改掉。
        double sleep_ms = 200.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.min_period_ms > 0) sleep_ms = static_cast<double>(options_.min_period_ms);
        }
        // 分片睡眠，让 stop() 的响应更快（最坏延迟 = 一个小片段）。
        constexpr int kSlices = 10;
        for (int i = 0; i < kSlices && !stopping_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(sleep_ms / kSlices)));
        }
    }
    running_.store(false);
}

SpectrumFrame SpectrumStream::acquire_frame() {
    SpectrumFrame frame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame.frame_index = next_frame_index_++;
        frame.period_ms = 0.0;
        frame.requested_sync = site_.spectrometer.synchronized;
        // 节奏与触发方式：**由配置决定，不猜**。
        // follow_site_trigger_mode=false 时强制自由触发，
        // 免得在没接 TTL 源的情况下每帧都超时。
        const CapturePace pace = CaptureCoordinator::pace_from_config(site_);
        if (options_.follow_site_trigger_mode) {
            frame.period_ms = pace.period_ms.has_value() ? *pace.period_ms : 0.0;
            frame.timeout_ms = pace.timeout_ms.has_value() ? *pace.timeout_ms : 0u;
        } else {
            frame.period_ms = static_cast<double>(options_.min_period_ms);
            frame.timeout_ms = site_.capture_timeout_ms;
        }
        // last_error 只在真失败时写；成功帧不清空它（status 里要能看到最近一次失败）。
    }
    frame.wall_clock_ms = wall_clock_ms();
    frame.captured_monotonic_ms = monotonic_ms();

    std::function<libs::Result<libs::devices::CaptureResult>()> hook;
    libs::devices::Spectrometer* device = nullptr;
    config::SiteConfig site;
    Options options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hook = capture_hook_;
        device = device_;
        site = site_;
        options = options_;
    }

    // 真正动设备。**device_mutex 在这里取、在这里放**，
    // 绝不跨帧持有：采集是同步阻塞的，持锁整轮会让并发的
    // status/health 全部排队卡住。
    libs::Result<libs::devices::CaptureResult> result;
    {
        std::lock_guard<std::mutex> device_lock(device_mutex_);
        if (hook) {
            result = hook();
        } else if (device == nullptr) {
            result = libs::Error::make(libs::ErrorKind::unsupported, "spectrometer_not_open",
                                       "光谱仪未打开或未启用，无法采集");
        } else {
            // 预览的站点副本。
            //
            // **只在自由触发时**才清掉 master/sync：那一路不要任何触发，而
            // "配了 master 就当外触发"的老逻辑会把它顶成等 pin6，四个通道
            // 会一起超时（现场就是"点了开始检测，一条谱都没有"）。
            //
            // **跟随触发时绝不能清**：这时三台从机正是靠 SYNC 总线被主通道
            // 带起来的（物理上只有主通道的 pin6 接了激光 TTL）。清掉 sync
            // 等于让三台从机去等一个永远不会来的触发 —— 实测就是这样：
            // 四通道一起超时，而激光明明在打。
            config::SiteConfig preview_site = site;
            if (!options.follow_site_trigger_mode) {
                preview_site.spectrometer.synchronized = false;
                preview_site.spectrometer.master_serial.clear();
            }
            const CaptureCoordinator coordinator(preview_site);
            const CapturePace pace = CaptureCoordinator::pace_from_config(preview_site);
            // 预览默认自由触发：wait_for_laser_trigger=false。
            // 这一行是本服务"不发激光写指令"纪律的落点——它只决定
            // 光谱仪要不要等 pin 6 的 TTL，碰不到激光器一根线。
            if (options.follow_site_trigger_mode) {
                result = coordinator.capture(device, pace,
                                             site.spectrometer.wait_for_laser_trigger,
                                             site.spectrometer.integration_delay_ns);
            } else {
                result = coordinator.capture(device, pace, false, 0.0);
            }
        }
    }

    if (!result) {
        // 整次采集失败：这才是"采集失败"。如实带上 code/message，
        // UI 能区分"没采到"和"采到了但通道缺失"。
        frame.capture_error_code = result.error().code;
        frame.capture_error_message = result.error().message;
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_code = frame.capture_error_code;
        last_error_message = frame.capture_error_message;
        return frame;
    }

    for (const auto& group : result.value().groups) {
        frame.shot_id = group.shot_id;                       // 原样上送
        frame.hardware_synchronized = group.hardware_synchronized;   // 原样上送（§3.10.1）
        for (const auto& spectrum : group.spectra) {
            ChannelFrame channel;
            channel.serial = spectrum.serial;
            channel.pixels = spectrum.pixels;
            channel.device_time_ticks = spectrum.device_time_ticks;
            channel.wavelength_nm = spectrum.wavelength_nm;
            channel.counts = spectrum.counts;
            if (!spectrum.wavelength_nm.empty() && !spectrum.counts.empty()) {
                channel.state = ChannelState::present;
            } else {
                // 通道缺失是**第三态**（§7 纪律 2）：标 absent + 原因，
                // 不用零值填充冒充一条平线。
                channel.state = ChannelState::absent;
                channel.reason = "该通道本帧无数据（SDK 返回空 wavelength/counts）";
            }
            frame.channels.push_back(std::move(channel));
        }
    }
    // 没有 groups 也算一种失败（complete=false 但 groups 为空）。
    if (result.value().groups.empty()) {
        frame.capture_error_code = "no_scan_groups";
        frame.capture_error_message = "采集未产出任何光谱组（组为空）";
    }
    return frame;
}

void SpectrumStream::publish(const SpectrumFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_error_code.empty() && frame.ok()) {
        last_error_code.clear();
    }
    last_frame_monotonic_ms = frame.captured_monotonic_ms;
    delivered_frames_++;

    for (auto& entry : subscribers_) {
        auto& queue = entry.second.queue;
        // 丢最旧的一帧（背压见文件头）：宁可 UI 少看几帧，
        // 也不能让一个慢客户端把采集节奏拖住。
        while (queue.size() >= options_.subscriber_queue_frames && !queue.empty()) {
            queue.pop_front();
            dropped_frames_++;
        }
        queue.push_back(frame);
    }
}

}  // namespace business::services
