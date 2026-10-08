#include "libs/business/app/runtime.hpp"

#include <utility>

namespace business::app {

json::Value to_json(const session::HealthReport& report) {
    json::Value v = json::object();
    v.set("all_required_open", json::Value(report.all_required_open));
    v.set("summary", json::Value(report.summary));
    const auto device = [](const session::DeviceState& s) {
        json::Value d = json::object();
        d.set("configured", json::Value(s.configured));
        d.set("opened", json::Value(s.opened));
        d.set("endpoint", json::Value(s.endpoint));
        d.set("model", json::Value(s.model));
        d.set("note", json::Value(s.note));
        return d;
    };
    v.set("laser", device(report.laser));
    v.set("indicator", device(report.indicator));
    v.set("spectrometer", device(report.spectrometer));
    return v;
}

json::Value Runtime::health_json() const { return business::app::to_json(devices_.health()); }

// ── 出光会话记账（进程级资源互斥，不是业务逻辑）───────────────────────────

bool Runtime::begin_laser_session() {
    std::lock_guard<std::mutex> lock(laser_session_mutex_);
    if (laser_session_active_) return false;
    laser_session_active_ = true;
    return true;
}

void Runtime::end_laser_session() noexcept {
    std::lock_guard<std::mutex> lock(laser_session_mutex_);
    laser_session_active_ = false;
}

bool Runtime::laser_session_active() const {
    std::lock_guard<std::mutex> lock(laser_session_mutex_);
    return laser_session_active_;
}

Runtime::Runtime() = default;

Runtime::~Runtime() { stop(); }

// ── 生命周期 ──────────────────────────────────────────────────────────────

libs::VoidResult Runtime::start(const config::SiteConfig& config, const Options& options) {
    config_ = config;
    options_ = options;

    const std::string data_dir = options.data_dir;
    const std::string log_file = options.log_file.empty() ? data_dir + "/run.jsonl" : options.log_file;
    const std::string parameters_file =
        options.parameters_file.empty() ? data_dir + "/parameters.json" : options.parameters_file;
    const std::string captures_dir = options.captures_dir.empty() ? data_dir + "/captures" : options.captures_dir;
    const std::string recovery_dir = options.recovery_dir.empty() ? data_dir + "/recovery" : options.recovery_dir;

    // 顺序：日志先开，后面所有步骤才有地方记。日志开不了不是致命错误。
    log_ = std::make_unique<logging::RunLog>(log_file);
    const libs::VoidResult log_open = log_->open();
    if (log_open) {
        json::Value fields = json::object();
        fields.set("log_file", json::Value(log_file));
        log_->write(logging::RecordKind::general, "运行开始，正在装配业务层依赖", fields);
    }

    recovery_ = std::make_unique<logging::RecoveryContextStore>(recovery_dir);

    // ── shared：参数库 ──────────────────────────────────────────────────
    //
    // 分组与必填字段由 ParameterService 定义（它同时也是出光时读参数的地方），
    // 两边必须用同一份定义，否则会出现"判定认为齐备、出光却取不到值"。
    auto store = std::unique_ptr<config::ParameterStore>(
        new config::FileParameterStore(parameters_file,
                                       ParameterService::required_groups(),
                                       ParameterService::required_fields()));
    store->load();
    parameters_ = std::move(store);

    // 出厂默认只覆盖光谱侧三个字段（只补缺失的）。顺序必须是先 load 再 seed：
    // 反了会拿默认值盖掉操作员已经调好的参数。激光侧不播种，见 seed_defaults()。
    const libs::VoidResult seeded = parameters_service_.seed_defaults();
    if (!seeded) {
        json::Value f = json::object();
        f.set("error", json::Value(seeded.error().message));
        log_->write(logging::RecordKind::general, "写入默认测量参数失败", f);
    }

    // ── readiness：业务一 ───────────────────────────────────────────────
    //
    // 事实源是一个闭包：**只负责去设备上把事实读出来，别的什么都不做**。
    // 判定、状态映射、亮灯全在 readiness/ 内部，Runtime 一概不碰。
    //
    // 每根控制线都要串行化，所以读设备必须拿 device_mutex_。
    // "读不到"如实返回 false —— **不假定安全**。
    monitor_ = std::make_unique<readiness::ReadinessMonitor>(
        [this](readiness::ReadinessFacts& facts) {
            std::lock_guard<std::mutex> lock(device_mutex_);
            libs::devices::Laser* laser = devices_.laser();
            if (laser == nullptr) return false;
            const libs::Result<libs::devices::LaserActual> actual = laser->readActual();
            if (!actual) return false;
            // 极性换算集中在 readiness::parse_actual 一处，
            // 出光前复核与运行期重判都调它 —— 不允许两处各写一遍。
            facts.laser = readiness::parse_actual(actual.value());
            return true;
        });

    // 配置事实（不是设备读数）在这里一次性注入。参数齐备与否是**启动时就能
    // 确定的事实**，不该依赖某个客户端先来问一句 —— 否则守护进程每次重启后
    // 判定都卡在 parameters_incomplete，现场表现为"刚开机预检莫名不通过"，
    // 而参数其实早就齐备。
    monitor_->set_parameters_ready(parameters_service_.readiness().ready);
    monitor_->set_laser_enabled(config_.laser.enabled);
    // 出光参数是否已标定，**按参数库实际内容算**，不再读 site.json 的 commissioned：
    // 参数管理页面填齐 9 个合法值即视为已标定。未标定 -> 判定拦截，一条写指令都不发。
    monitor_->set_laser_commissioned(parameters_service_.commissioned_from_store());
    monitor_->set_spectrometer_ready(config_.spectrometer.enabled);

    precheck_ = std::make_unique<readiness::PrecheckService>(*monitor_, devices_.indicator());

    // ── shared：数据交付 ────────────────────────────────────────────────
    local_sink_ = std::unique_ptr<data_delivery::DeliverySink>(
        new data_delivery::LocalFileSink(captures_dir));

    if (options.open_devices) {
        const libs::VoidResult opened = devices_.open(config_);
        if (opened) {
            json::Value f = json::object();
            f.set("health", health_json());
            log_->write(logging::RecordKind::general, "全部已启用设备打开成功", f);
        } else {
            // 部分失败允许，但要如实记录：这台机器可能处于"降级可用"。
            json::Value f = json::object();
            f.set("error", json::Value(opened.error().message));
            f.set("health", health_json());
            log_->write(logging::RecordKind::exception_recovery,
                        "设备打开部分失败，机器以降级状态运行", f);
        }
    } else {
        log_->write(logging::RecordKind::general, "按选项跳过设备打开（离线模式）");
    }
    // 设备开了没有，是一个**事实**，交给判定去解释。
    monitor_->set_devices_opened(devices_.laser() != nullptr);

    // ── measurement：业务二 ─────────────────────────────────────────────
    //
    // 等设备与 sink 都就位后才构造。它在 start() 里**只构造、不启动** ——
    // 测量是显式业务动作，由 UI/CLI 发 measurement_start 才开光闸；
    // 不随守护进程起来就自动出光（那就成了开机即打光）。
    writer_ = std::make_unique<measurement::Writer>(*local_sink_, config_.station_id);
    measurement_ = std::make_unique<measurement::MeasurementJob>(
        devices_.laser(), devices_.spectrometer(), *monitor_, frame_bus_, *writer_);
    measurement_->set_log(log_.get());
    // 测量参数同样来自参数库 —— 与出光读同一份，
    // 否则采集的积分时间和出光参数各走各的。
    // 包成 lambda 而不是直接传结构体：MeasurementJob 每帧都要取一次参数，
    // 取的动作发生在采集线程上，而参数库是可被 UI 改的。这里保证
    // "每帧读当前参数"，操作员中途改积分时间对下一帧立即生效。
    measurement_->configure(config_, [this]() {
        return parameters_service_.spectrometer_measurement();
    });

    // 开机就把灯刷到位：现场看到的第一眼必须是真实结论，不是"上一次的灯色"。
    // 此时还没布防，状态停在 standby -> 黄灯。预检（precheck->run()）会
    // arm() + evaluate() 并再刷一次。
    sync_indicator();

    running_ = true;
    return libs::VoidResult();
}

libs::VoidResult Runtime::stop() {
    if (!running_) {
        // start 中途失败也可能已经建出 measurement_（它是在 devices_.open()
        // 之后构造的）。它没跑起来，析构会兜底，但这里先 stop() 一次是零成本的，
        // 而且能把"设备还没关、作业已经不在了"这个顺序摆正。
        if (measurement_ != nullptr) measurement_->stop();
        devices_.close();   // start 中途打开过也要关
        return libs::VoidResult();
    }
    running_ = false;
    log_->write(logging::RecordKind::general, "运行结束，开始优雅关闭");

    // 撤防。撤防后判定恒不放行 —— 停机过程中不可能再有人开光闸。
    if (monitor_ != nullptr) monitor_->disarm();

    // **收光**：连续出光模式下激光正在出光，不能就这么关掉走人。
    // 关串口不会让激光停下来 —— 它自己保持着工作状态。所以必须显式
    // 光闸关 + 停机，而且要在 devices_.close() **之前**做（那之后没有设备句柄）。
    //
    // **无条件调用 job->stop()，不看 running()**：它自己的注释说得很清楚，
    // 收光是无条件的 —— 哪怕 start 从没成功过、哪怕中途哪一步失败，
    // 都要尽力把光闸关掉。这里少写一个 if，光就有可能留在外面。
    if (measurement_ != nullptr) {
        const libs::VoidResult stopped = measurement_->stop();
        log_->write(logging::RecordKind::general,
                    stopped ? "已停止测量作业并收光"
                            : ("停止测量作业时收光未确认（现场需人工确认光闸）: " +
                               stopped.error().message));
    }
    end_laser_session();

    // 灯必须**在关设备之前**刷成"全灭"。顺序反了 = 灯色停在最后一个业务状态
    // （比如还绿着），而软件已经关了。下次开机前现场看到的是一片绿的机器，
    // 会误以为它可以出光。
    //
    // 退出路径要的是**全灭**，不是撤防后的 standby 黄灯 —— 软件都要关了，
    // 留一圈灯在假装系统还活着。
    if (precheck_ != nullptr) precheck_->force_offline();
    const libs::VoidResult closed = devices_.close();
    log_->write(logging::RecordKind::general,
                closed ? "设备已关闭" : ("设备关闭报错: " + closed.error().message));
    return closed;
}

libs::VoidResult Runtime::sync_indicator() {
    // 灯显示的是**判定结论**，而真正刷灯的实现在 PrecheckService ——
    // 全库唯一的灯下发点。Runtime 只转发，不解释结论。
    if (precheck_ == nullptr) {
        return libs::Error::make(libs::ErrorKind::unsupported, "precheck_not_ready",
                                 "预检服务未装配，无法刷新状态灯");
    }
    return precheck_->sync_lamp_only();
}

void Runtime::set_last_delivery(data_delivery::DeliveryStatus status) {
    last_delivery_ = std::move(status);
    if (!last_delivery_.has_value()) return;
    json::Value f = json::object();
    f.set("local_saved", json::Value(last_delivery_->local_saved));
    f.set("local_path", json::Value(last_delivery_->local_path));
    f.set("local_error", json::Value(last_delivery_->local_error));
    f.set("gateway_delivered", json::Value(last_delivery_->gateway_delivered));
    f.set("gateway_error", json::Value(last_delivery_->gateway_error));
    log_->write(logging::RecordKind::general, "采集交付状态", f);
}

}  // namespace business::app
