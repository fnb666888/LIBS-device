#include "libs/business/measurement/job.hpp"

#include <utility>

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
                               Writer& writer)
    : laser_(laser),
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
    MeasurementStartRecord record;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 已经在跑就别重复开：重复发 0x21=1/0x24=1 本身没害处，但会让操作员
        // 以为"点了两次是两次不同的动作"，状态反而更乱。
        if (running_.load()) {
            return libs::Error::make(libs::ErrorKind::device, "laser_already_running",
                                     "测量作业已在运行；要停止请用 measurement_stop");
        }
        if (laser_ == nullptr) {
            return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open",
                                     "激光器未打开或未启用，无法出光");
        }
    }

    // ── 纪律 1：门禁是唯一判定入口 ──────────────────────────────────────
    //
    // 不放行时**一条写指令都不发**。这是本流程最关键的安全性质，
    // 也是离线测试守的那一条。
    // 注意这一行的位置：它在任何 laser_ 调用**之前**，所以返回时设备一个字节
    // 都没被改过。
    const readiness::GateVerdict verdict = readiness_.evaluate_now();
    if (!verdict.allowed) {
        record.failed_step = "gate";
        record.error_code = "gate_blocked";
        record.error_message = "门禁未放行，未发出任何写指令：" + verdict.detail;
        return libs::Error::make(libs::ErrorKind::device, "gate_blocked", record.error_message);
    }

    // ── 出光：写会话参数 → 使能 → 等放电 → 开光闸 ──────────────────────
    //
    // 这一步含 5~6 秒的 settle（实测值），所以它**阻塞**。
    // 这也是为什么 start() 在开完光闸之后必须立刻返回、把循环丢给线程：
    // UI 的停止按钮不该被 6 秒的出光启动卡住。
    libs::devices::LaserOperationTrace trace;
    const libs::VoidResult started = laser_->startContinuous(
        to_device_settings(params.laser), params.laser.settle_timeout_ms, trace);
    if (!started) {
        record.failed_step = trace.failed_step.empty() ? "start_continuous" : trace.failed_step;
        record.error_code = started.error().code;
        record.error_message = started.error().message;
        return started.error();
    }

    record.sent_frames = trace.sent_frames;
    record.settle_ms = trace.settle_ms;
    record.discharge_counter_at_start = trace.counter_before;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_sent_frames_ = record.sent_frames;
        last_settle_ms_ = record.settle_ms;
        last_discharge_counter_ = record.discharge_counter_at_start;
        // 清掉上一次 stop() 留下的停止标志。
        // **这一行是"能重新启动"的全部要害**：少了它，stop() 置的
        // stopping_=true 永远留着，作业就再也起不来了 —— 而 start() 照样
        // 返回成功，现场表现是"点了开始，status 说没在跑，也没报错"。
        stopping_.store(false);
        last_gate_check_ms_ = monotonic_ms();
        running_.store(true);
    }
    // 落盘线程由子模块自己管：它有队列、有线程、有一套背压规则，
    // 作业不该替它做这些决定。
    writer_.start();
    worker_ = std::thread([this]() { loop(); });

    record.ok = true;
    record.note = "连续出光已开光闸，采集循环已启动：每次出光的 TTL 都会被光谱仪采成"
                 "一帧，逐帧落盘并推给 UI";
    log_record(logging::RecordKind::state_change, "测量作业已启动",
               [this, &record]() {
                   json::Value v = json::object();
                   v.set("settle_ms", json::Value(static_cast<double>(record.settle_ms)));
                   v.set("discharge_counter_at_start",
                         json::Value(static_cast<double>(record.discharge_counter_at_start)));
                   json::Value frames = json::array();
                   for (const std::string& f : record.sent_frames) {
                       frames.push_back(json::Value(f));
                   }
                   v.set("sent_frames", json::Value(std::move(frames)));
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
    // 采集循环线程持有停止标志并在**采集返回后**才退出。
    //
    // 为什么不在这里置标志然后立刻收光：capture() 是阻塞的，要等一个 TTL
    // （最长 timeout_ms）。如果先把光闸关了再等线程 join，那一帧会永远
    // 等不到触发、只能超时退出 —— 停止会因此白等一个超时。
    // 所以顺序是：先收光（设备停止出光）→ 循环里那一次 capture 自然超时返回
    // → 循环看到标志退出 → join。
    // 这个超时是**故意的**，它换来的是"停止时不会有一次挂死的 capture"。
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

void MeasurementJob::loop() {
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

        const SpectrumFrame frame = capture_once();
        if (!frame.ok()) {
            // 采集失败**不终止循环**：激光还在打，光谱仪可能只是偶发丢一帧。
            // 但每失败一次都要计数并留下最近错误，绝不静默。
            shots_failed_++;
            // 失败帧也要推给 UI：界面据此把图置灰，让人一眼看出
            // "这一帧不是新数据"，而不是停在上一帧假装一切正常。
            frame_bus_.publish(frame);
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

SpectrumFrame MeasurementJob::capture_once() {
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
        measurement = provider();
    } else {
        frame.capture_error_code = "measurement_not_configured";
        frame.capture_error_message = "测量参数未配置（measurement provider 缺失）";
        return frame;
    }
    frame.period_ms = measurement.period_ms;
    frame.timeout_ms = measurement.timeout_ms;

    // 采集请求就地构造。归属见 `capture_plan.hpp`：装机配置给接线与通道，
    // 测量参数给测量本身，wait_for_laser_trigger 硬编码 true。
    const libs::devices::CaptureRequest request = build_capture_request(site, measurement);

    libs::Result<libs::devices::CaptureResult> result;
    if (hook) {
        result = hook();
    } else if (spectrometer == nullptr) {
        result = libs::Error::make(libs::ErrorKind::unsupported, "spectrometer_not_open",
                                   "光谱仪未打开或未启用，无法采集");
    } else {
        result = spectrometer->captureOnce(request);
    }

    if (!result) {
        frame.capture_error_code = result.error().code;
        frame.capture_error_message = result.error().message;
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_code_ = frame.capture_error_code;
        last_error_message_ = frame.capture_error_message;
        return frame;
    }

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
