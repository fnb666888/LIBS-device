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
    v.set("rangefinder", device(report.rangefinder));
    v.set("indicator", device(report.indicator));
    v.set("air_conditioner", device(report.air_conditioner));
    v.set("spectrometer", device(report.spectrometer));
    return v;
}

json::Value Runtime::health_json() const { return business::app::to_json(devices_.health()); }

namespace {
// 根参数分组（范围报告 §3.3.1：字段未细化，只表达"分组 + 是否齐备"）。
// 任务书 §4.1 明示"具体核心字段由后续接口细化"，因此这里不臆造字段语义，
// 只声明这些分组存在；"齐备"由分组内字段存在性判定。
std::vector<std::string> default_required_groups() {
    return {"spectrometer", "laser", "trigger"};
}
}  // namespace

Runtime::Runtime()
    : capture_(config::SiteConfig{}),
      laser_fire_(*this),
      ac_monitor_(1),
      ac_interlock_(config::SiteConfig{}) {}

Runtime::~Runtime() { stop(); }

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
    capture_ = services::CaptureCoordinator(config_);


    // 参数：先加载已有值；加载失败不算致命（首次运行是正常的）。
    std::map<std::string, std::vector<std::string>> required_fields;   // 字段未细化，留空
    auto store = std::unique_ptr<policy::ParameterStore>(
        new policy::FileParameterStore(parameters_file, default_required_groups(), required_fields));
    store->load();
    parameters_ = std::move(store);

    // 落盘的参数刚加载完，**立刻把它反映到门禁上**。
    //
    // 不这么做的话，守护进程每次重启后门禁都卡在 parameters_incomplete，
    // 一直要等到有人碰巧调一次 param_get / param_set 才同步过来 ——
    // 现场表现为"刚开机预检莫名不通过"，而参数其实早就齐备。参数齐备与否
    // 是启动时就能确定的事实，不该依赖某个客户端先来问一句。
    gate_.set_parameters_ready(parameters_->readiness().ready);

    // 门禁的静态条件：激光是否在配置里启用。
    gate_.set_laser_enabled(config_.laser.enabled);
    // 出光参数是否已标定。未标定 -> 门禁拦截，一条写指令都不会发。
    gate_.set_laser_commissioned(config_.laser_fire.commissioned);

    // 空调联锁 3.9.2 是 C 类：未标定时它**不参与门禁**，门禁默认放行这一项。
    // 标定后由 ac_interlock_ 的结论接管——但那时才轮得到它说话。
    ac_interlock_ = services::AirConditionerInterlock(config_);
    gate_.set_air_conditioner_interlock(true);
    ac_monitor_ = services::AirConditionerMonitor(config_.air_conditioner_address);

    local_sink_ = std::unique_ptr<data_delivery::DeliverySink>(
        new data_delivery::LocalFileSink(captures_dir));
    gateway_sink_ = std::unique_ptr<data_delivery::DeliverySink>(
        new data_delivery::UnsupportedGatewaySink());
    position_switch_ = std::unique_ptr<services::PositionSwitch>(new services::UnavailablePositionSwitch());
    motion_timing_ = services::MotionTiming::placeholder();

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

    // 实时光谱流：绑定站点配置与光谱仪句柄。
    // **必须在 devices_.open() 之后**——devices_.spectrometer() 返回的是
    // 打开之后才拿到的那个对象指针；放在 open() 之前拿到的是 nullptr，
    // 于是流会一直如实报 spectrometer_not_open，而 health 却说"已打开"。
    // 这种"一个说开了、一个说没开"的自相矛盾最难排障，踩过一次。
    // 默认**不启动**：实时预览是显式动作，由 UI/CLI 发 start，
    // 不随守护进程起来就自动跑（否则白烧 USB 与日志）。
    spectrum_stream_.configure(config_, devices_.spectrometer());

    // 首次读一次激光事实，让门禁不至于因为"还没读"而误判成"未布防"。
    poll_laser_facts();

    running_ = true;
    return libs::VoidResult();
}

libs::VoidResult Runtime::stop() {
    if (!running_) {
        devices_.close();   // start 中途打开过也要关
        return libs::VoidResult();
    }
    running_ = false;
    log_->write(logging::RecordKind::general, "运行结束，开始优雅关闭");
    gate_.disarm();
    // **收光**：连续出光模式下激光正在出光，不能就这么关掉走人。
    // 关串口不会让激光停下来 —— 它自己保持着工作状态。所以必须显式
    // 光闸关 + 停机，而且要在 devices_.close() **之前**做（那之后没有设备句柄）。
    if (laser_running_) {
        const libs::VoidResult stopped = laser_fire_.stop_continuous();
        laser_running_ = false;
        log_->write(logging::RecordKind::general,
                    stopped ? "已停止连续出光并收光"
                            : ("停止连续出光失败（现场需人工确认光闸）: " +
                               stopped.error().message));
    }
    // 流必须**先停**，再关设备。反了 = 采集线程对着正在销毁的
    // 设备句柄采集，或去 lock 一把已经没了的 mutex。
    // stop() 内部会 join 采集线程，返回后不会再有新帧。
    spectrum_stream_.stop();
    const libs::VoidResult closed = devices_.close();
    log_->write(logging::RecordKind::general,
                closed ? "设备已关闭" : ("设备关闭报错: " + closed.error().message));
    return closed;
}

libs::VoidResult Runtime::poll_laser_facts() {
    return poll_laser_facts(devices_.laser());
}

libs::VoidResult Runtime::poll_laser_facts(libs::devices::Laser* laser) {
    if (laser == nullptr) {
        // 读不到设备：available 保持默认 false，门禁据此拦截（"不知道" != "安全"）。
        gate_.update_laser_facts(policy::LaserSafetyFacts{});
        return libs::Error::make(libs::ErrorKind::unsupported, "laser_not_open",
                                 "激光器未打开；门禁按“读不到状态”处理，不假定安全");
    }
    const libs::Result<libs::devices::LaserTransaction> tx = laser->read(libs::laser::command::kLdActual);
    if (!tx) {
        gate_.update_laser_facts(policy::LaserSafetyFacts{});   // 读失败 = 事实缺失 = 不安全
        return tx.error();
    }
    // 极性换算集中在 policy::parse_actual_frame 一处，门禁与出光服务共用。
    const policy::LaserSafetyFacts facts = policy::parse_actual_frame(tx.value().frame);
    gate_.update_laser_facts(facts);

    return libs::VoidResult();
}

void Runtime::set_stage(events::DetectionStage stage) {
    if (stage_ == stage) return;
    log_stage_change(stage_, stage);
    stage_ = stage;
}

void Runtime::set_belt_sequence(std::int64_t seq) {
    belt_sequence_ = seq;
    json::Value f = json::object();
    f.set("belt_sequence", json::Value(static_cast<double>(seq)));
    f.set("target_belt", json::Value(target_belt_));
    log_->write(logging::RecordKind::belt_and_position, "带序号更新", f);
}

void Runtime::set_target_belt(std::string belt) {
    target_belt_ = std::move(belt);
    json::Value f = json::object();
    f.set("target_belt", json::Value(target_belt_));
    f.set("belt_sequence", json::Value(static_cast<double>(belt_sequence_)));
    log_->write(logging::RecordKind::belt_and_position, "目标带更新", f);
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

void Runtime::log_stage_change(events::DetectionStage from, events::DetectionStage to) {
    json::Value f = json::object();
    f.set("from", json::Value(events::to_string(from)));
    f.set("to", json::Value(events::to_string(to)));
    f.set("target_belt", json::Value(target_belt_));
    log_->write(logging::RecordKind::state_change, "检测阶段迁移", f);
}

}  // namespace business::app
