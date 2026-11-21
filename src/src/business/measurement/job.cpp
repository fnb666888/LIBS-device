#include "libs/business/measurement/job.hpp"

#include <utility>
#include <condition_variable>
#include <memory>
#include <exception>

#include "libs/business/shared/time_util.hpp"

namespace business::measurement {

namespace {

libs::devices::LaserSessionSettings to_device_settings(const config::LaserFireConfig& cfg) {
    return {cfg.discharge_current_a, cfg.pulse_width_us, cfg.frequency_hz,
            cfg.discharge_delay_us, cfg.q_delay_us, cfg.q_warmup_s};
}

}  // namespace

MeasurementJob::MeasurementJob(libs::devices::Laser* laser,
                               libs::devices::Spectrometer* spectrometer,
                               readiness::ReadinessMonitor& readiness,
                               FrameQueue& frame_bus,
                               Writer& writer, std::mutex* acquisition_mutex,
                               std::mutex* spectrometer_poll_mutex)
    : acquisition_mutex_(acquisition_mutex), spectrometer_poll_mutex_(spectrometer_poll_mutex), laser_(laser),
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

void MeasurementJob::configure_gate_sampling(std::function<void()> sample,
                                             std::function<void()> phase_changed) {
    sample_gate_inputs_ = std::move(sample);
    gate_phase_changed_ = std::move(phase_changed);
}

libs::Result<MeasurementStartRecord> MeasurementJob::start(const FireParameters& params) {
    std::unique_lock<std::mutex> lifecycle(lifecycle_mutex_);
    if (running_.load())
        return libs::Error::make(libs::ErrorKind::device, "laser_already_running", "测量作业已在运行");
    if (worker_.joinable()) worker_.join();
    if (stop_unconfirmed())
        return libs::Error::make(libs::ErrorKind::device, "laser_stop_unconfirmed", "上次停机未确认，请先执行停止检测并核对结果");
    if (laser_source_) laser_ = laser_source_();
    writer_.stop();
    std::unique_lock<std::mutex> acquisition;
    if (acquisition_mutex_) {
        // Let an idle probe finish before acquiring the SDK for this job.
        std::unique_lock<std::mutex> poll;
        if (spectrometer_poll_mutex_) poll = std::unique_lock<std::mutex>(*spectrometer_poll_mutex_);
        acquisition = std::unique_lock<std::mutex>(*acquisition_mutex_, std::try_to_lock);
        if (!acquisition.owns_lock())
            return libs::Error::make(libs::ErrorKind::device, "acquisition_busy", "光谱仪正在进行独立采集");
    }
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
    const readiness::GateVerdict verdict = refresh_gate();
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
    stopping_.store(false);
    running_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_confirmation_.store(StopConfirmation::pending);
        stop_error_ = {};
        faulted_ = false;
        gate_fault_ = false;
        last_error_code_.clear();
        last_error_message_.clear();
    }
    const auto prepared = laser_->prepareContinuous(to_device_settings(params.laser),
                                                   params.laser.settle_timeout_ms, startup->trace);
    if (!prepared) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_code_ = prepared.error().code;
            last_error_message_ = prepared.error().message;
        }
        finish_stop(true);
        return prepared.error();
    }
    if (acquisition.owns_lock()) acquisition.unlock();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        laser_frequency_hz_ = params.laser.frequency_hz;
        active_params_ = params;
        last_gate_check_ms_ = monotonic_ms();
    }
    try {
        writer_.start();
        worker_ = std::thread([this, startup, initial]() {
            auto armed = [this, startup]() -> libs::VoidResult {
                // Preparing the laser may take seconds; refresh interlocks before opening.
                std::lock_guard<std::mutex> gate_lock(gate_mutex_);
                if (sample_gate_inputs_) sample_gate_inputs_();
                const auto verdict = readiness_.evaluate_now();
                const auto opened = verdict.allowed
                    ? laser_->openQGate(startup->trace)
                    : libs::VoidResult(libs::Error::make(libs::ErrorKind::device,
                        "gate_lost_during_startup", "布防完成后门禁未放行：" + verdict.detail));
                emitting_ = opened.ok();
                if (emitting_ && gate_phase_changed_) gate_phase_changed_();
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
            };
            bool faulted = true;
            try { faulted = loop(armed, first, initial); }
            catch (const std::exception& e) { failed(e.what()); }
            catch (...) { failed("测量线程发生未知异常"); }
            {
                std::lock_guard<std::mutex> lock(startup->mutex);
                if (!startup->done) {
                    startup->error = libs::Error::make(libs::ErrorKind::device,
                        "capture_not_armed", "首次采集激活前测量已结束");
                    startup->done = true;
                }
                faulted = faulted || !startup->error.ok();
            }
            finish_stop(faulted);
            startup->cv.notify_one();
        });
    } catch (const std::exception& e) {
        finish_stop(true);
        return libs::Error::make(libs::ErrorKind::internal, "measurement_thread_start", e.what());
    }
    std::unique_lock<std::mutex> wait(startup->mutex);
    startup->cv.wait(wait, [&]() { return startup->done; });
    const auto error = startup->error;
    MeasurementStartRecord record;
    record.sent_frames = startup->trace.sent_frames;
    record.settle_ms = startup->trace.settle_ms;
    record.discharge_counter_at_start = startup->trace.counter_before;
    wait.unlock();
    if (!error.ok()) {
        if (worker_.joinable()) worker_.join();
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
    const char* confirmation = "unknown";
    if (s.stop_confirmation == StopConfirmation::pending) confirmation = "pending";
    if (s.stop_confirmation == StopConfirmation::confirmed) confirmation = "confirmed";
    if (s.stop_confirmation == StopConfirmation::unconfirmed) confirmation = "unconfirmed";
    v.set("stop_confirmation", json::Value(confirmation));
    v.set("stop_error", json::Value(s.stop_error));
    v.set("faulted", json::Value(s.faulted));
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
    const bool retry = !running() && stop_unconfirmed();
    stopping_.store(true);
    if (worker_.joinable()) worker_.join();
    frame_bus_.clear_pending();
    if (laser_source_) laser_ = laser_source_();
    if (laser_ == nullptr)
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open", "激光器未打开，无法收光");
    if (retry || stop_confirmation_.load() == StopConfirmation::unknown) finish_stop(false);
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_error_ ? libs::VoidResult(stop_error_) : libs::VoidResult();
}

void MeasurementJob::finish_stop(bool faulted) {
    stopping_.store(true);
    libs::devices::LaserOperationTrace trace;
    const auto stopped = [&] {
        std::lock_guard<std::mutex> gate_lock(gate_mutex_);
        const auto result = laser_->stopContinuous(trace);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_error_ = result.error();
            stop_confirmation_.store(result ? StopConfirmation::confirmed : StopConfirmation::unconfirmed);
        }
        emitting_ = false;
        return result;
    }();
    writer_.stop();
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        faulted = faulted || gate_fault_;
        faulted_ = faulted || !stopped;
        reason = last_error_code_;
        running_.store(false);
    }
    if (gate_phase_changed_) gate_phase_changed_();
    log_record(stopped ? logging::RecordKind::state_change : logging::RecordKind::exception_recovery,
               stopped ? "测量已停止，停机回读确认" : "测量已结束，停机回读未确认", [&]() {
        json::Value fields = json::object();
        fields.set("stop_readback_confirmed", json::Value(stopped.ok()));
        if (faulted) fields.set("error_code", json::Value(reason));
        if (!stopped) fields.set("stop_error", json::Value(stopped.error().message));
        return fields;
    });
}

MeasurementStatus MeasurementJob::status() const {
    MeasurementStatus s;
    s.shots_captured = shots_captured_.load();
    s.shots_failed = shots_failed_.load();
    s.disk_writes_ok = writer_.writes_ok();
    s.disk_writes_failed = writer_.writes_failed();
    s.disk_backlog = writer_.backlog();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.running = running_.load();
        s.stop_confirmation = stop_confirmation_.load();
        s.stop_error = stop_error_.message;
        s.faulted = faulted_;
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

bool MeasurementJob::loop(std::function<libs::VoidResult()> on_armed,
                          std::function<void(const SpectrumFrame&)> on_first,
                          SpectrumMeasurement initial) {
    bool first = true;
    unsigned consecutive_failures = 0;
    while (!stopping_.load()) {
        // ── 纪律 2：运行期重判 ──────────────────────────────────────────
        //
        // start() 里那次判定是**快照**。连续出光可能一直打到你手动停止，
        // 中间有人打开了安全门 —— 事实变了，但没人再问，光就一直在打。
        //
        // 所以这里周期性重判。**判定的执行方和收光的执行方必须是同一个对象**：
        // 不能靠 UI（可能崩/被关/卡住），也不能靠互锁自己
        // （互锁是"事实变了"，它不负责"去把光关掉"）。
        if (!sample_gate_inputs_ && !refresh_gate(false).allowed) return true;

        const SpectrumFrame frame = capture_once(first ? on_armed : std::function<libs::VoidResult()>{},
                                                first ? &initial : nullptr);
        if (first) { on_first(frame); first = false; }
        if (stopping_.load()) break;
        if (!frame.ok()) {
            // An isolated missed trigger may recover; device loss cannot.
            shots_failed_++;
            ++consecutive_failures;
            // 失败帧也要推给 UI：界面据此把图置灰，让人一眼看出
            // "这一帧不是新数据"，而不是停在上一帧假装一切正常。
            frame_bus_.publish(frame);
            const auto& code = frame.capture_error_code;
            const bool fatal = code == "laser_spectrometer_rate_mismatch" ||
                code == "capture_parameters_invalid" || code == "spectrometer_disconnected" ||
                code == "serial_not_found" || code == "serial_not_available" ||
                code == "no_devices" || code == "spectrometer_not_open";
            if (fatal || consecutive_failures >= 3) {
                if (sample_gate_inputs_ && spectrometer_reset_requested_.load()) {
                    stopping_.store(true);
                    on_device_status_changed();
                }
                return true;
            }
            log_record(logging::RecordKind::exception_recovery, "采集失败，等待重试",
                [&]() {
                    json::Value v = json::object();
                    v.set("error_code", json::Value(code));
                    v.set("detail", json::Value(frame.capture_error_message));
                    v.set("consecutive_failures", json::Value(static_cast<double>(consecutive_failures)));
                    return v;
                });
            // Bound immediate SDK error retries and keep manual stop responsive.
            for (int i = 0; i < 4 && !stopping_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        consecutive_failures = 0;
        shots_captured_++;
        // 帧 → UI（纯内存 push，不碰设备、不阻塞）
        frame_bus_.publish(frame);
        // 帧 → 落盘（异步队列，O(1)，不反压采集）
        writer_.enqueue(frame);
    }
    return false;
}

readiness::GateVerdict MeasurementJob::refresh_gate(bool force) {
    std::lock_guard<std::mutex> gate_lock(gate_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = static_cast<std::uint64_t>(monotonic_ms());
        if (!force && last_gate_check_ms_ != 0 &&
            now - last_gate_check_ms_ < kGateRecheckIntervalMs) return readiness_.latest();
    }
    if (sample_gate_inputs_) sample_gate_inputs_();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_gate_check_ms_ = monotonic_ms();
    }
    return evaluate_gate_locked();
}

void MeasurementJob::on_device_status_changed() {
    std::lock_guard<std::mutex> gate_lock(gate_mutex_);
    evaluate_gate_locked();
}

readiness::GateVerdict MeasurementJob::evaluate_gate_locked() {
    const auto verdict = readiness_.evaluate_now(emitting_
        ? readiness::GateCheckPhase::during_emission
        : readiness::GateCheckPhase::before_emission);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (emitting_ && !verdict.allowed && !stopping_.load()) {
            gate_fault_ = true;
            last_error_code_ = "gate_lost_mid_run";
            last_error_message_ = "运行期门禁未放行：" + verdict.detail;
            stopping_.store(true);
        }
    }
    return verdict;
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
        if (frame.capture_error_code == "spectrometer_disconnected" ||
            frame.capture_error_code == "no_devices" ||
            frame.capture_error_code == "serial_not_found" ||
            frame.capture_error_code == "serial_not_available") {
            readiness_.set_spectrometer_ready(false);
            spectrometer_reset_requested_.store(true);
        }
        // Owner cancellation is the normal stop path, not a failed acquisition.
        // Keep any earlier real error; the loop discards this frame on stopping_.
        if (stopping_.load() && result.error().code == "capture_cancelled") return frame;
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
