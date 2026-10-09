#include "libs/business/measurement/job.hpp"

#include <utility>
#include <condition_variable>
#include <memory>
#include <exception>

#include "libs/business/shared/time_util.hpp"

namespace business::measurement {

namespace {

libs::devices::LaserSessionSettings to_device_settings(const config::LaserFireConfig& cfg) {
    libs::devices::LaserSessionSettings s;
    s.discharge_current_a = cfg.discharge_current_a;
    s.pulse_width_us = cfg.pulse_width_us;
    s.frequency_hz = cfg.frequency_hz;
    s.discharge_delay_us = cfg.discharge_delay_us;
    s.q_delay_us = cfg.q_delay_us;
    s.q_warmup_s = cfg.q_warmup_s;
    return s;
}

}  // namespace

MeasurementJob::MeasurementJob(libs::devices::Laser* laser,
                               libs::devices::Spectrometer* spectrometer,
                               readiness::ReadinessMonitor& readiness,
                               FrameQueue& frame_bus,
                               Writer& writer, std::mutex* acquisition_mutex)
    : acquisition_mutex_(acquisition_mutex), laser_(laser),
      spectrometer_(spectrometer),
      readiness_(readiness),
      frame_bus_(frame_bus),
      writer_(writer) {}

MeasurementJob::~MeasurementJob() {
    // 收光优先于一切。关串口不会让激光停下来 —— 设备自己保持着"工作"状态，
    // 所以析构必须先尽力收光，再停线程。反过来做 = 光留在外面。
    stop();
}

void MeasurementJob::bind_devices(libs::devices::Laser* laser,
                                  libs::devices::Spectrometer* spectrometer) {
    std::lock_guard<std::mutex> lock(mutex_);
    laser_ = laser;
    spectrometer_ = spectrometer;
}

void MeasurementJob::configure(const config::SiteConfig& site,
                               std::function<SpectrumMeasurement()> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    site_ = site;
    measurement_provider_ = std::move(provider);
}

libs::Result<MeasurementStartRecord> MeasurementJob::start(const FireParameters& params) {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    if (running_.load())
        return libs::Error::make(libs::ErrorKind::device, "laser_already_running", "测量作业已在运行");
    if (worker_.joinable()) worker_.join();
    writer_.stop();
    if (laser_ == nullptr)
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open", "激光器未打开");
    std::function<SpectrumMeasurement()> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        provider = measurement_provider_;
    }
    if (!provider)
        return libs::Error::make(libs::ErrorKind::invalid_argument,
                                 "measurement_not_configured", "测量参数未配置");
    const readiness::GateVerdict verdict = readiness_.evaluate_now();
    if (!verdict.allowed)
        return libs::Error::make(libs::ErrorKind::device, "gate_blocked", "门禁未放行，未发出任何写指令：" + verdict.detail);
    SpectrumMeasurement initial = provider();
    initial.integration_ms = params.integration_ms;
    initial.timeout_ms = params.capture_timeout_ms;
    initial.integration_delay_ns = params.integration_delay_ns;
    initial.scans = 1;
    const auto rate = validate_capture_rate(params.laser.frequency_hz, initial);
    if (!rate) return rate.error();
    struct Startup {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        libs::Error error;
        libs::devices::LaserOperationTrace trace;
    };
    const auto startup = std::make_shared<Startup>();
    const auto prepared = laser_->prepareContinuous(to_device_settings(params.laser),
                                                   params.laser.settle_timeout_ms, startup->trace);
    if (!prepared) return prepared.error();
    stopping_.store(false);
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        laser_frequency_hz_ = params.laser.frequency_hz;
        active_params_ = params;
        last_gate_check_ms_ = monotonic_ms();
    }
    writer_.start();
    worker_ = std::thread([this, startup, initial]() {
        auto armed = [this, startup]() -> libs::VoidResult {
            // Preparing the laser may take seconds; refresh interlocks before opening.
            const auto verdict = readiness_.evaluate_now();
            const auto opened = verdict.allowed
                ? laser_->openQGate(startup->trace)
                : libs::VoidResult(libs::Error::make(libs::ErrorKind::device,
                    "gate_lost_during_startup", "布防完成后门禁未放行：" + verdict.detail));
            {
                std::lock_guard<std::mutex> lock(startup->mutex);
                if (!opened) startup->error = opened.error();
                startup->done = true;
            }
            startup->cv.notify_one();
            return opened;
        };
        auto first = [this, startup](const SpectrumFrame& frame) {
            std::lock_guard<std::mutex> lock(startup->mutex);
            if (!startup->done) {
                startup->error = libs::Error::make(libs::ErrorKind::device,
                    frame.capture_error_code.empty() ? "capture_not_armed" : frame.capture_error_code,
                    frame.capture_error_message.empty() ? "光谱仪未完成布防" : frame.capture_error_message);
                startup->done = true;
                stopping_.store(true);
                startup->cv.notify_one();
            }
            if (!startup->error.ok()) stopping_.store(true);
        };
        auto failed = [this, startup](const std::string& message) {
            stopping_.store(true);
            libs::devices::LaserOperationTrace stopped;
            (void)laser_->stopContinuous(stopped);
            running_.store(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_code_ = "measurement_worker_exception";
                last_error_message_ = message;
            }
            {
                std::lock_guard<std::mutex> lock(startup->mutex);
                startup->error = libs::Error::make(libs::ErrorKind::internal,
                                                  "measurement_worker_exception", message);
                startup->done = true;
            }
            startup->cv.notify_one();
        };
        try { loop(armed, first, initial); }
        catch (const std::exception& e) { failed(e.what()); }
        catch (...) { failed("测量线程发生未知异常"); }
    });
    std::unique_lock<std::mutex> wait(startup->mutex);
    startup->cv.wait(wait, [&]() { return startup->done; });
    const auto error = startup->error;
    MeasurementStartRecord record;
    record.sent_frames = startup->trace.sent_frames;
    record.settle_ms = startup->trace.settle_ms;
    record.discharge_counter_at_start = startup->trace.counter_before;
    wait.unlock();
    if (!error.ok()) {
        stopping_.store(true);
        libs::devices::LaserOperationTrace stopped;
        (void)laser_->stopContinuous(stopped);
        if (worker_.joinable()) worker_.join();
        writer_.stop();
        running_.store(false);
        return error;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_sent_frames_ = record.sent_frames;
        last_settle_ms_ = record.settle_ms;
        last_discharge_counter_ = record.discharge_counter_at_start;
    }
    record.ok = true;
    record.note = "光谱仪已布防后开光闸，测量帧由统一作业采集、分发和落盘";
    log_record(logging::RecordKind::state_change, "测量作业已启动", [record]() {
        json::Value v = json::object();
        v.set("settle_ms", json::Value(static_cast<double>(record.settle_ms)));
        v.set("spectrometer_armed_before_gate", json::Value(true));
        return v;
    });
    return record;
}

json::Value to_json(const MeasurementStatus& s) {
    json::Value v = json::object();
    v.set("running", json::Value(s.running));
    v.set("shots_captured", json::Value(static_cast<double>(s.shots_captured)));
    v.set("shots_failed", json::Value(static_cast<double>(s.shots_failed)));
    v.set("disk_writes_ok", json::Value(static_cast<double>(s.disk_writes_ok)));
    v.set("disk_writes_failed", json::Value(static_cast<double>(s.disk_writes_failed)));
    // backlog 是"写盘跟不上采集"的唯一信号。现场看到它持续增长，
    // 就该去查磁盘，而不是加 CPU —— 采集节奏是被刻意与写盘解耦的。
    v.set("disk_backlog", json::Value(static_cast<double>(s.disk_backlog)));
    v.set("last_error_code", json::Value(s.last_error_code));
    v.set("last_error_message", json::Value(s.last_error_message));
    if (!s.last_error_code.empty()) v.set("ok", json::Value(false));
    return v;
}

libs::VoidResult MeasurementJob::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    // Signal cooperative SDK polling cancellation, close the gate and stop the laser,
    // then join the acquisition thread before closing any device handles.
    stopping_.store(true);

    libs::devices::Laser* laser = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        laser = laser_;
    }
    if (laser == nullptr) {
        // 激光器没打开：收光这条命令**发不出去**，但线程仍然要收干净。
        // 不能因为发不出收光指令就把落盘线程和采集线程晾在后台。
        if (worker_.joinable()) worker_.join();
        writer_.stop();
        running_.store(false);
        frame_bus_.clear_pending();
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open",
                                 "激光器未打开，无法收光");
    }

    // 收光**无条件执行**：哪怕 start 从没成功过、哪怕中途哪一步失败，
    // 都要尽力把光闸关掉。守护进程退出时也靠它。
    libs::devices::LaserOperationTrace trace;
    const libs::VoidResult stopped = laser->stopContinuous(trace);

    // join：采集线程的 capture 超时返回后自然退出。
    if (worker_.joinable()) worker_.join();
    // 落盘线程：把队列里剩下的帧写完再退出。
    writer_.stop();
    running_.store(false);
    frame_bus_.clear_pending();

    if (stopped) {
        log_record(logging::RecordKind::state_change, "测量作业已停止",
                   [this, &trace]() {
                       json::Value v = json::object();
                       json::Value frames = json::array();
                       for (const std::string& f : trace.sent_frames) {
                           frames.push_back(json::Value(f));
                       }
                       v.set("sent_frames", json::Value(std::move(frames)));
                       v.set("shots_captured",
                             json::Value(static_cast<double>(shots_captured_.load())));
                       v.set("shots_failed",
                             json::Value(static_cast<double>(shots_failed_.load())));
                       v.set("disk_writes_ok",
                             json::Value(static_cast<double>(writer_.writes_ok())));
                       v.set("disk_writes_failed",
                             json::Value(static_cast<double>(writer_.writes_failed())));
                       return v;
                   });
    }
    return stopped;
}

MeasurementStatus MeasurementJob::status() const {
    MeasurementStatus s;
    s.running = running_.load();
    s.shots_captured = shots_captured_.load();
    s.shots_failed = shots_failed_.load();
    s.disk_writes_ok = writer_.writes_ok();
    s.disk_writes_failed = writer_.writes_failed();
    s.disk_backlog = writer_.backlog();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.last_error_code = last_error_code_;
        s.last_error_message = last_error_message_;
    }
    // 落盘侧的最近失败原因要并进来：一次采集成功不代表这次作业没出错。
    // 采集侧只记采集错误，落盘侧只记落盘错误，两边都要能被 status 看到。
    const std::string disk_error_code = writer_.last_error_code();
    if (!disk_error_code.empty()) {
        s.last_error_code = disk_error_code;
        s.last_error_message = writer_.last_error_message();
    }
    return s;
}

// ── 采集循环 ───────────────────────────────────────────────────────────────

void MeasurementJob::loop(std::function<libs::VoidResult()> on_armed,
                          std::function<void(const SpectrumFrame&)> on_first,
                          SpectrumMeasurement initial) {
    bool first = true;
    while (!stopping_.load()) {
        // ── 纪律 2：运行期重判 ──────────────────────────────────────────
        //
        // start() 里那次判定是**快照**。连续出光可能一直打到你手动停止，
        // 中间有人打开了安全门 —— 事实变了，但没人再问，光就一直在打。
        //
        // 所以这里周期性重判。**判定的执行方和收光的执行方必须是同一个对象**：
        // 不能靠 UI（可能崩/被关/卡住），也不能靠互锁自己
        // （互锁是"事实变了"，它不负责"去把光关掉"）。
        if (!recheck_gate()) break;

        const SpectrumFrame frame = capture_once(first ? on_armed : std::function<libs::VoidResult()>{},
                                                first ? &initial : nullptr);
        if (first) { on_first(frame); first = false; }
        if (stopping_.load()) break;
        if (!frame.ok()) {
            // 采集失败**不终止循环**：激光还在打，光谱仪可能只是偶发丢一帧。
            // 但每失败一次都要计数并留下最近错误，绝不静默。
            shots_failed_++;
            // 失败帧也要推给 UI：界面据此把图置灰，让人一眼看出
            // "这一帧不是新数据"，而不是停在上一帧假装一切正常。
            frame_bus_.publish(frame);
            if (frame.capture_error_code == "laser_spectrometer_rate_mismatch" ||
                frame.capture_error_code == "capture_parameters_invalid") {
                libs::devices::LaserOperationTrace trace;
                (void)laser_->stopContinuous(trace);
                stopping_.store(true);
                running_.store(false);
                break;
            }
            continue;
        }

        shots_captured_++;
        // 帧 → UI（纯内存 push，不碰设备、不阻塞）
        frame_bus_.publish(frame);
        // 帧 → 落盘（异步队列，O(1)，不反压采集）
        writer_.enqueue(frame);
    }
}

bool MeasurementJob::recheck_gate() {
    const std::uint64_t now = monotonic_ms();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_gate_check_ms_ != 0 &&
            now - last_gate_check_ms_ < kGateRecheckIntervalMs) {
            return true;   // 还没到周期，判一次太贵
        }
        last_gate_check_ms_ = now;
    }

    const readiness::GateVerdict verdict = readiness_.evaluate_now();
    if (verdict.allowed) return true;

    // 中途失锁：**自己收光**。不等 UI，不等互锁，不等下一次 status 查询。
    libs::devices::Laser* laser = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        laser = laser_;
        last_error_code_ = "gate_lost_mid_run";
        last_error_message_ = "运行期重判发现门禁已关闭，作业自行收光：" + verdict.detail;
    }

    libs::devices::LaserOperationTrace trace;
    if (laser != nullptr) {
        laser->stopContinuous(trace);
    }
    // 让循环自然结束：stopping_ 不由这里置，因为那会让 stop() 的收光命令
    // 与线程退出互相踩。这里置它，让 loop() 走完这一轮就退出，
    // stop() 随后照常 join 并再收一次光（幂等）。
    stopping_.store(true);
    // running_ 也要立即落下来，不能等 stop() 才落。光已经关了，
    // 这时向 UI 报“正在测”就是撒谎：操作员会继续等待一个永远不来的帧，
    // 而下一次 start() 还会被 laser_already_running 拒绝。
    // 线程本身仍在退出途中，但从“还在采集”的角度看，它已经结束了。
    running_.store(false);

    log_record(logging::RecordKind::state_change, "运行期门禁关闭，已自行收光",
               [this, &verdict]() {
                   json::Value v = json::object();
                   v.set("error_code", json::Value("gate_lost_mid_run"));
                   v.set("detail", json::Value(verdict.detail));
                   v.set("shots_captured",
                         json::Value(static_cast<double>(shots_captured_.load())));
                   v.set("shots_failed",
                         json::Value(static_cast<double>(shots_failed_.load())));
                   return v;
               });
    return false;
}

SpectrumFrame MeasurementJob::capture_once(const std::function<libs::VoidResult()>& on_armed,
                                           const SpectrumMeasurement* initial) {
    std::unique_lock<std::mutex> acquisition;
    if (acquisition_mutex_) acquisition = std::unique_lock<std::mutex>(*acquisition_mutex_);
    SpectrumFrame frame;

    std::function<SpectrumMeasurement()> provider;
    libs::devices::Spectrometer* spectrometer = nullptr;
    config::SiteConfig site;
    std::function<libs::Result<libs::devices::CaptureResult>()> hook;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame.frame_index = static_cast<std::int64_t>(next_frame_index_++);
        frame.requested_sync = site_.spectrometer.synchronized;
        provider = measurement_provider_;
        spectrometer = spectrometer_;
        site = site_;
        hook = capture_hook_;
    }

    frame.wall_clock_ms = wall_clock_ms();
    frame.captured_monotonic_ms = monotonic_ms();
    // **本流程每一帧都是激光触发采到的测量谱**，所以恒为 true。
    // 这个字段存在，是为了将来加"空闲预览"时能把背景谱（false）与
    // 测量谱（true）在数据上分开，而不是靠注释或口头约定。
    frame.laser_shot = true;

    SpectrumMeasurement measurement;
    if (provider) {
        measurement = initial ? *initial : provider();
    } else {
        frame.capture_error_code = "measurement_not_configured";
        frame.capture_error_message = "测量参数未配置（measurement provider 缺失）";
        return frame;
    }
    if (active_params_.override_integration) measurement.integration_ms = active_params_.integration_ms;
    if (active_params_.override_timeout) measurement.timeout_ms = active_params_.capture_timeout_ms;
    if (active_params_.override_delay) measurement.integration_delay_ns = active_params_.integration_delay_ns;
    const auto rate = validate_capture_rate(laser_frequency_hz_, measurement);
    if (!rate) {
        frame.capture_error_code = rate.error().code;
        frame.capture_error_message = rate.error().message;
        return frame;
    }
    measurement.scans = 1;
    frame.period_ms = measurement.period_ms;
    frame.timeout_ms = measurement.timeout_ms;

    // 采集请求就地构造。归属见 `capture_plan.hpp`：装机配置给接线与通道，
    // 测量参数给测量本身，wait_for_laser_trigger 硬编码 true。
    const libs::devices::CaptureRequest request = build_capture_request(site, measurement);

    libs::Result<libs::devices::CaptureResult> result;
    if (hook) {
        const auto armed = on_armed ? on_armed() : libs::VoidResult();
        result = armed ? hook() : libs::Result<libs::devices::CaptureResult>(armed.error());
    } else if (spectrometer == nullptr) {
        result = libs::Error::make(libs::ErrorKind::unsupported, "spectrometer_not_open",
                                   "光谱仪未打开或未启用，无法采集");
    } else {
        result = spectrometer->captureOnce(request, on_armed, [this] { return stopping_.load(); });
    }

    if (!result) {
        frame.capture_error_code = result.error().code;
        frame.capture_error_message = result.error().message;
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_code_ = frame.capture_error_code;
        last_error_message_ = frame.capture_error_message;
        return frame;
    }

    frame.expected_groups = result.value().expected_groups;
    frame.received_groups = result.value().received_groups;
    for (const auto& group : result.value().groups) {
        frame.shot_id = group.shot_id;                                     // 原样上送
        frame.hardware_synchronized = group.hardware_synchronized;        // 原样上送
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
                // 通道缺失是**第三态**：标 absent + 原因，
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
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_code_ = frame.capture_error_code;
        last_error_message_ = frame.capture_error_message;
    }
    return frame;
}

void MeasurementJob::log_record(logging::RecordKind kind, const std::string& message,
                                std::function<json::Value()> payload) {
    logging::RunLog* log = log_;
    if (log == nullptr || !log->enabled()) return;
    json::Value fields = payload ? payload() : json::object();
    log->write(kind, message, std::move(fields));
}

}  // namespace business::measurement
