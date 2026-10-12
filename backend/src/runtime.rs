use crate::{
    config::SiteConfig,
    error::{self, Error, Result},
    frames::{Frame, FrameBus},
    parameters::{self, FireParameters, Parameters},
    prediction::{self, Prediction},
    readiness::{self, Monitor},
    serial::{Indicator, Laser, LaserTrace},
    spectrometer::Spectrometer,
};
use parking_lot::{Condvar, Mutex};
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, VecDeque},
    fs::{File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc,
    },
    thread::JoinHandle,
    time::{Duration, Instant},
};

#[cfg(test)]
#[path = "runtime_tests.rs"]
mod tests;

pub const COMMANDS: [&str; 17] = [
    "ping",
    "config_show",
    "status",
    "gate",
    "precheck",
    "param_set",
    "param_get",
    "capture",
    "recovery_list",
    "recovery_write",
    "health",
    "shutdown",
    "measurement_start",
    "measurement_stop",
    "measurement_status",
    "spectrum_stream",
    "prediction",
];
pub const GATEWAY: &str = "① 上传协议与载荷格式 ② 接收地址与端口 ③ 交付确认层级 ④ IPC->网关 与 网关->海关 的确认区分（任务书 §6.3 未给出网关协议/地址/端口/海关格式；范围报告 §3.6.2 G1/G2）";

pub fn replace_file(source: &Path, target: &Path) -> std::io::Result<()> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn MoveFileExW(source: *const u16, target: *const u16, flags: u32) -> i32;
    }
    let source: Vec<_> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let target: Vec<_> = target.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { MoveFileExW(source.as_ptr(), target.as_ptr(), 1 | 8) } == 0 {
        Err(std::io::Error::last_os_error())
    } else {
        Ok(())
    }
}

#[derive(Default)]
struct MeasurementState {
    shots_captured: u64,
    shots_failed: u64,
    last_error_code: String,
    last_error_message: String,
}
struct WriterState {
    queue: VecDeque<Arc<Frame>>,
    stopping: bool,
    writes_ok: u64,
    writes_failed: u64,
    error_code: String,
    error_message: String,
}
struct Writer {
    state: Mutex<WriterState>,
    wake: Condvar,
    worker: Mutex<Option<JoinHandle<()>>>,
    directory: PathBuf,
    station: String,
}
impl Writer {
    fn new(directory: PathBuf, station: String) -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(WriterState {
                queue: VecDeque::new(),
                stopping: false,
                writes_ok: 0,
                writes_failed: 0,
                error_code: String::new(),
                error_message: String::new(),
            }),
            wake: Condvar::new(),
            worker: Mutex::new(None),
            directory,
            station,
        })
    }
    fn start(self: &Arc<Self>) {
        self.state.lock().stopping = false;
        let writer = self.clone();
        *self.worker.lock() = Some(std::thread::spawn(move || writer.run()));
    }
    fn enqueue(&self, frame: Arc<Frame>) {
        let mut s = self.state.lock();
        if s.queue.len() >= 64 {
            s.queue.pop_front();
            s.writes_failed += 1;
            s.error_code = "disk_backlog_dropped".into();
            s.error_message = "落盘队列积压，已丢弃较早的一帧（磁盘太慢）".into();
        }
        s.queue.push_back(frame);
        self.wake.notify_one();
    }
    fn stop(&self) {
        self.state.lock().stopping = true;
        self.wake.notify_all();
        if let Some(worker) = self.worker.lock().take() {
            let _ = worker.join();
        }
    }
    fn run(&self) {
        loop {
            let frame = {
                let mut s = self.state.lock();
                while s.queue.is_empty() && !s.stopping {
                    self.wake.wait(&mut s);
                }
                match s.queue.pop_front() {
                    Some(frame) => frame,
                    None => return,
                }
            };
            let delivery = save_capture(&self.directory, &self.station, &frame, &[]);
            let mut s = self.state.lock();
            if delivery["local_saved"] == true {
                s.writes_ok += 1;
            } else {
                s.writes_failed += 1;
                s.error_code = delivery["local_error"]
                    .as_str()
                    .filter(|s| !s.is_empty())
                    .unwrap_or("local_save_failed")
                    .into();
                s.error_message = format!(
                    "落盘失败：{}",
                    delivery["local_error"].as_str().unwrap_or("")
                );
            }
        }
    }
}

pub struct Runtime {
    pub config: SiteConfig,
    pub data_dir: PathBuf,
    pub parameters: Mutex<Parameters>,
    pub bus: Arc<FrameBus>,
    pub prediction: Arc<Prediction>,
    laser: Option<Arc<Laser>>,
    indicator: Mutex<Option<Indicator>>,
    spectrometer: Mutex<Option<Spectrometer>>,
    health: Value,
    monitor: Mutex<Monitor>,
    lamp: Mutex<Value>,
    last_delivery: Mutex<Value>,
    log: Mutex<Option<File>>,
    log_count: AtomicU64,
    writer: Arc<Writer>,
    measurement: Mutex<MeasurementState>,
    lifecycle: Mutex<()>,
    worker: Mutex<Option<JoinHandle<()>>>,
    acquisition: Mutex<()>,
    pub shutdown: AtomicBool,
    running: AtomicBool,
    measuring: AtomicBool,
    cancel: AtomicBool,
    session: AtomicBool,
    frame_index: AtomicU64,
}
impl Runtime {
    pub fn start(
        config: SiteConfig,
        data_dir: PathBuf,
        open_devices: bool,
        algorithm_dir: PathBuf,
    ) -> Result<Arc<Self>> {
        let log = std::fs::create_dir_all(&data_dir).ok().and_then(|_| {
            OpenOptions::new()
                .create(true)
                .append(true)
                .open(data_dir.join("run.jsonl"))
                .ok()
        });
        let mut params = Parameters::open(data_dir.join("parameters.json"));
        let seed = params.seed();
        let mut laser = None;
        let mut indicator = None;
        let mut spectrum = None;
        let device =
            || json!({"configured":false,"opened":false,"endpoint":"","model":"","note":""});
        let mut health = json!({"laser":device(),"indicator":device(),"spectrometer":device()});
        if open_devices {
            for (name, label, cfg) in [
                ("laser", "激光器", &config.laser),
                ("indicator", "状态灯", &config.indicator),
            ] {
                if !cfg.enabled {
                    health[name]["note"] = "配置未启用（enabled=false）".into();
                    continue;
                }
                health[name]["configured"] = true.into();
                health[name]["endpoint"] = cfg.port.clone().into();
                let result = if name == "laser" {
                    Laser::open(cfg).map(|l| laser = Some(Arc::new(l)))
                } else {
                    Indicator::open(
                        cfg,
                        config.indicator_model.clone(),
                        config.indicator_address,
                    )
                    .map(|i| indicator = Some(i))
                };
                match result {
                    Ok(()) => health[name]["opened"] = true.into(),
                    Err(e) => {
                        health[name]["note"] =
                            format!("{label} 打开失败：{}/{}: {}", e.kind, e.code, e.message).into()
                    }
                }
            }
            if config.spectrometer.enabled {
                health["spectrometer"]["configured"] = true.into();
                health["spectrometer"]["endpoint"] = config.spectrometer.sdk_dll.clone().into();
                match Spectrometer::open(Path::new(&config.spectrometer.sdk_dll)) {
                    Ok(s) => {
                        health["spectrometer"]["opened"] = true.into();
                        health["spectrometer"]["model"] = s.model.clone().into();
                        spectrum = Some(s);
                    }
                    Err(e) => {
                        health["spectrometer"]["note"] =
                            format!("光谱仪打开失败：{}/{}: {}", e.kind, e.code, e.message).into()
                    }
                }
            } else {
                health["spectrometer"]["note"] = "配置未启用（enabled=false）".into();
            }
        }
        let mut all = true;
        let mut summary = vec![];
        for (name, label) in [
            ("laser", "激光器"),
            ("indicator", "状态灯"),
            ("spectrometer", "光谱仪"),
        ] {
            let d = &health[name];
            summary.push(if d["configured"] != true {
                format!("{label}: 未启用")
            } else if d["opened"] == true {
                format!("{label}: 已打开 {}", d["endpoint"].as_str().unwrap())
            } else {
                all = false;
                format!("{label}: 失败 {}", d["note"].as_str().unwrap())
            });
        }
        health["all_required_open"] = all.into();
        health["summary"] = summary.join("; ").into();
        let monitor = Monitor {
            parameters_ready: params.readiness()["ready"] == true,
            laser_enabled: config.laser.enabled,
            laser_commissioned: params.groups.get("laser").is_some_and(|g| g.complete),
            devices_opened: laser.is_some(),
            spectrometer_ready: config.spectrometer.enabled,
            ..Default::default()
        };
        let bus = Arc::new(FrameBus::default());
        let prediction =
            Prediction::start(bus.clone(), data_dir.join("predictions"), algorithm_dir);
        let writer = Writer::new(data_dir.join("captures"), config.station_id.clone());
        let runtime = Arc::new(Self {
            config,
            data_dir,
            parameters: Mutex::new(params),
            bus,
            prediction,
            laser,
            indicator: Mutex::new(indicator),
            spectrometer: Mutex::new(spectrum),
            health,
            monitor: Mutex::new(monitor),
            lamp: Mutex::new(Value::Null),
            last_delivery: Mutex::new(Value::Null),
            log: Mutex::new(log),
            log_count: AtomicU64::new(0),
            writer,
            measurement: Mutex::new(MeasurementState::default()),
            lifecycle: Mutex::new(()),
            worker: Mutex::new(None),
            acquisition: Mutex::new(()),
            shutdown: AtomicBool::new(false),
            running: AtomicBool::new(true),
            measuring: AtomicBool::new(false),
            cancel: AtomicBool::new(false),
            session: AtomicBool::new(false),
            frame_index: AtomicU64::new(0),
        });
        runtime.log(
            "general",
            "运行开始，正在装配业务层依赖",
            json!({"log_file":runtime.data_dir.join("run.jsonl")}),
        );
        if let Err(e) = seed {
            runtime.log(
                "general",
                "写入默认测量参数失败",
                json!({"error":e.message}),
            );
        }
        runtime.log(
            "general",
            if open_devices {
                "全部已启用设备打开成功"
            } else {
                "按选项跳过设备打开（离线模式）"
            },
            if open_devices {
                json!({"health":runtime.health})
            } else {
                json!({})
            },
        );
        runtime.sync_lamp();
        Ok(runtime)
    }
    pub fn log(&self, kind: &str, message: &str, fields: Value) {
        if let Some(file) = self.log.lock().as_mut() {
            let record = json!({"kind":kind,"wall_clock_ms":crate::wall_clock_ms(),"monotonic_ms":crate::monotonic_ms(),"message":message,"fields":fields});
            if writeln!(file, "{record}")
                .and_then(|_| file.flush())
                .is_ok()
            {
                self.log_count.fetch_add(1, Ordering::Relaxed);
            }
        }
    }
    fn evaluate(&self, emitting: bool) -> Value {
        let actual = self.laser.as_ref().and_then(|l| l.actual().ok());
        let mut monitor = self.monitor.lock();
        monitor.evaluate(actual.as_ref(), emitting);
        monitor.snapshot()
    }
    fn sync_lamp(&self) {
        let state = self.monitor.lock().state;
        self.apply_lamp(state);
    }
    fn apply_lamp(&self, state: &str) {
        let result = self
            .indicator
            .lock()
            .as_mut()
            .map(|i| i.apply(state))
            .unwrap_or_else(|| {
                Err(Error::new(
                    "unsupported",
                    "indicator_not_open",
                    "状态灯未打开或未启用，本次灯色未下发",
                ))
            });
        let mut lamp = json!({"intent":readiness::lamp_intent(state),"applied":result.is_ok()});
        if let Err(e) = result {
            lamp["last_error"] = e.message.into();
        }
        *self.lamp.lock() = lamp;
    }
    fn arm(&self) -> Result<()> {
        if !self.config.laser.enabled {
            return Err(Error::new(
                "device",
                "gate_laser_disabled",
                "激光器在配置中未启用，无法布防",
            ));
        }
        self.monitor.lock().armed = true;
        self.evaluate(false);
        Ok(())
    }
    pub fn handle_line(self: &Arc<Self>, line: &str) -> Value {
        let input = match serde_json::from_str::<Value>(line) {
            Ok(v) => v,
            Err(e) => {
                return Error::invalid(
                    "request_bad_json",
                    format!(
                        "请求不是合法 JSON，偏移 {}: {e}",
                        e.column().saturating_sub(1)
                    ),
                )
                .response();
            }
        };
        if !input.is_object() || input.get("command").is_none() {
            return Error::invalid(
                "request_missing_command",
                "请求必须是一个含 command 字段的 JSON 对象",
            )
            .response();
        }
        let Some(command) = input["command"].as_str() else {
            return Error::invalid("request_command_not_string", "command 必须是字符串").response();
        };
        let args = input.get("args").cloned().unwrap_or(json!({}));
        if !args.is_object() {
            return Error::invalid("request_args_not_object", "args 必须是 JSON 对象").response();
        }
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.dispatch(command, &args)
        }));
        match result {
            Ok(reply) => reply,
            Err(_) => Error::new(
                "internal",
                "request_unhandled_exception",
                "命令执行抛出异常",
            )
            .response(),
        }
    }
    fn dispatch(self: &Arc<Self>, command: &str, args: &Value) -> Value {
        if command == "precheck" {
            return self.precheck();
        }
        if command == "param_set" {
            return self.param_set(args);
        }
        error::response(self.command(command, args))
    }
    fn command(self: &Arc<Self>, command: &str, args: &Value) -> Result<Value> {
        match command {
            "ping" => Ok(
                json!({"pong":true,"wall_clock_ms":crate::wall_clock_ms(),"running":self.running.load(Ordering::Acquire),"commands":COMMANDS}),
            ),
            "config_show" => Ok(serde_json::to_value(&self.config).unwrap()),
            "health" => Ok(self.health.clone()),
            "status" => Ok(
                json!({"running":self.running.load(Ordering::Acquire),"gate":self.monitor.lock().snapshot(),"laser_running":self.measuring.load(Ordering::Acquire),"health":self.health,"prediction":self.prediction.status(),
                "log_records":self.log_count.load(Ordering::Relaxed),"indicator":self.lamp.lock().clone(),"last_delivery":self.last_delivery.lock().clone(),"unavailable_capabilities":[format!("gateway_delivery: {GATEWAY}")]}),
            ),
            "gate" => {
                if let Some(v) = args["parameters_ready"].as_bool() {
                    self.monitor.lock().parameters_ready = v;
                }
                if let Some(action) = args.get("action") {
                    match action.as_str().unwrap_or("") {
                        "arm" => self.arm()?,
                        "disarm" => {
                            let mut m = self.monitor.lock();
                            m.armed = false;
                            m.state = "standby";
                        }
                        "refresh" => {
                            self.evaluate(false);
                        }
                        other => {
                            return Err(Error::invalid(
                                "gate_bad_action",
                                format!("action 只能是 arm / disarm / refresh，收到: {other}"),
                            ));
                        }
                    }
                }
                Ok(self.monitor.lock().snapshot())
            }
            "param_get" => {
                let params = self.parameters.lock();
                let mut v = if let Some(group) = args["group"].as_str() {
                    let g = params.groups.get(group).ok_or_else(|| {
                        Error::invalid("param_query_rejected", format!("参数分组 {group} 尚未设置"))
                    })?;
                    json!({"group":group,"values":g.values})
                } else {
                    json!({"groups":params.groups})
                };
                let r = params.readiness();
                v["readiness"] = json!({"ready":r["ready"],"summary":r["summary"]});
                self.monitor.lock().parameters_ready = r["ready"] == true;
                Ok(v)
            }
            "measurement_start" => self.start_measurement(args),
            "measurement_stop" => {
                let _lifecycle = self.lifecycle.lock();
                let was = self.measuring.load(Ordering::Acquire);
                self.stop_measurement()?;
                self.log("state_change", "测量作业停止", json!({"was_running":was}));
                Ok(
                    json!({"running":false,"status":self.measurement_status(false),
                "note":if was{"测量作业已停止：光闸关、已停机，读回确认通过；落盘线程已收尾"}else{"已下发收光与停机并读回确认（此前未在运行）"}}),
                )
            }
            "measurement_status" => Ok(self.measurement_status(true)),
            "spectrum_stream" => self.stream(args),
            "capture" => self.capture(),
            "prediction" => match args
                .get("action")
                .map(|v| v.as_str().unwrap_or(""))
                .unwrap_or("status")
            {
                "contract" => Ok(prediction::contract()),
                "interface" => Ok(crate::algorithm::interface()),
                "reload" => self.prediction.reload(),
                "validate" => prediction::validate(&args["input"]),
                "status" => Ok(self.prediction.status()),
                "latest" => Ok(self.prediction.status()["latest"].clone()),
                "configure" => {
                    let enabled = args["enabled"].as_bool().ok_or_else(|| {
                        Error::invalid("prediction_enabled_required", "enabled 必须为布尔值")
                    })?;
                    self.prediction.configure_parameters(
                        enabled,
                        args["branch"].as_str().unwrap_or(""),
                        args.get("parameters"),
                    )
                }
                "analyze" => self.prediction.submit(&args["input"]),
                _ => Err(Error::invalid(
                    "prediction_action_invalid",
                    "action 只能是 status/latest/configure/contract/validate/analyze/interface/reload",
                )),
            },
            "recovery_list" => {
                let mut paths = vec![];
                if let Ok(entries) = std::fs::read_dir(self.data_dir.join("recovery")) {
                    for entry in entries.flatten() {
                        if entry.path().is_file()
                            && entry.path().extension().is_some_and(|e| e == "json")
                        {
                            paths.push(entry.path().to_string_lossy().into_owned());
                        }
                    }
                }
                paths.sort_by(|a, b| b.cmp(a));
                Ok(
                    json!({"snapshots":paths,"note":"快照是观测记录，不是命令队列；不得据此自动重发运动或触发（任务书 §6.2）"}),
                )
            }
            "recovery_write" => {
                let directory = self.data_dir.join("recovery");
                std::fs::create_dir_all(&directory).map_err(|e| {
                    Error::new(
                        "transport",
                        "recovery_dir_failed",
                        format!(
                            "cannot create recovery directory {}: {e}",
                            directory.display()
                        ),
                    )
                })?;
                let now = crate::wall_clock_ms();
                let path = directory.join(format!("recovery_t{now}.json"));
                let monitor = self.monitor.lock();
                let snapshot = json!({"snapshot_at_ms":now,"machine_state":monitor.state,"blocked_by":monitor.blocked_by,"reason":args["reason"].as_str().unwrap_or("手工快照"),
                    "notice":"恢复上下文只是异常时刻的观测快照，不是命令队列；不得据此自动重发运动或激光触发命令（任务书 §6.2 红线）"});
                std::fs::write(&path, serde_json::to_vec_pretty(&snapshot).unwrap()).map_err(
                    |_| {
                        Error::new(
                            "transport",
                            "recovery_open_failed",
                            format!("cannot write recovery snapshot: {}", path.display()),
                        )
                    },
                )?;
                Ok(
                    json!({"path":path,"written":true,"note":"只写快照，不执行任何恢复动作（任务书 §6.2）"}),
                )
            }
            "shutdown" => {
                self.shutdown.store(true, Ordering::Release);
                Ok(json!({"shutdown_requested":true}))
            }
            other => Err(Error::new(
                "unsupported",
                "unknown_command",
                format!(
                    "未知命令: {other}；可用命令见 commands 字段。注意：出光只有 measurement_start 一条路径（原 laser_fire / laser_run / laser_stop 已删除）"
                ),
            )),
        }
    }
    fn precheck(&self) -> Value {
        let ready = self.parameters.lock().readiness()["ready"] == true;
        self.monitor.lock().parameters_ready = ready;
        let armed = self.arm();
        let mut report = json!({"state":"standby","allowed":false,"detail":"","blocked_by":[],"laser_readable":false});
        let outcome = match armed {
            Err(e) => {
                report["state"] = self.monitor.lock().state.into();
                report["detail"] = format!("门禁布防失败：{}", e.message).into();
                Err(e)
            }
            Ok(()) => {
                let gate = self.evaluate(false);
                for key in ["state", "allowed", "detail", "blocked_by", "laser_readable"] {
                    report[key] = gate[key].clone();
                }
                if gate["allowed"] == true {
                    Ok(())
                } else {
                    Err(Error::new(
                        "device",
                        "gate_blocked",
                        format!("门禁未放行：{}", gate["detail"].as_str().unwrap()),
                    ))
                }
            }
        };
        self.sync_lamp();
        let lamp = self.lamp.lock();
        report["lamp_intent"] = lamp["intent"].clone();
        report["lamp_applied"] = lamp["applied"].clone();
        if let Some(e) = lamp.get("last_error") {
            report["lamp_error"] = e.clone();
        }
        match outcome {
            Ok(()) => error::response(Ok(report)),
            Err(e) => {
                let mut reply = e.response();
                reply["payload"]["report"] = report;
                reply
            }
        }
    }
    fn param_set(&self, args: &Value) -> Value {
        let Some(group) = args["group"].as_str() else {
            return Error::invalid("param_missing_group", "缺少 group 字段").response();
        };
        let Some(values) = args["values"].as_object() else {
            return Error::invalid("param_missing_values", "values 必须是 JSON 对象").response();
        };
        let mut numbers = BTreeMap::new();
        for (field, v) in values {
            let Some(number) = v.as_f64() else {
                return Error::invalid(
                    "param_value_not_number",
                    format!("字段 {field} 的值必须是数字（缺失/unknown 用不上参数请省略该字段）"),
                )
                .response();
            };
            numbers.insert(field.clone(), number);
        }
        for (field, v) in &numbers {
            if let Some((lo, hi, integer)) = parameters::range(group, field) {
                if !parameters::accepts(group, field, *v) {
                    let (unit, source) = parameter_info(group, field);
                    let message = format!(
                        "参数被拒绝：{field} 必须在 {lo:0.6}~{hi:0.6} {unit}{}（{source}）",
                        if integer { "，且必须为整数" } else { "" }
                    );
                    let mut reply =
                        Error::invalid("param_out_of_range", message.clone()).response();
                    reply["payload"]["report"] = json!({"state":"rejected","completed":false,"group":group,"message":message,"failed_fields":[field]});
                    return reply;
                }
            }
        }
        let mut params = self.parameters.lock();
        let mut result = params.apply(group, &numbers);
        result["completed"] = (result["state"] == "completed").into();
        result["note"]="completed=false 表示“收到请求但未完成”；调用方必须检查它，不得把非 completed 当成成功（任务书 §4.2）".into();
        let r = params.readiness();
        result["readiness"] = json!({"ready":r["ready"],"summary":r["summary"]});
        self.monitor.lock().parameters_ready = r["ready"] == true;
        error::response(Ok(result))
    }
    fn stream(&self, args: &Value) -> Result<Value> {
        match args
            .get("action")
            .map(|v| v.as_str().unwrap_or(""))
            .unwrap_or("status")
        {
            "status" => Ok(self.bus.status()),
            "subscribe" => {
                let id = self.bus.subscribe();
                let mut v = self.bus.status();
                v["subscriber"] = id.into();
                v["subscribed"] = true.into();
                Ok(v)
            }
            "unsubscribe" => {
                let id = args["subscriber"].as_f64().ok_or_else(|| {
                    Error::invalid(
                        "stream_subscriber_required",
                        "unsubscribe 需要 subscriber 句柄",
                    )
                })? as u64;
                self.bus.unsubscribe(id);
                let mut v = self.bus.status();
                v["unsubscribed"] = true.into();
                Ok(v)
            }
            "latest" => {
                let id = args["subscriber"]
                    .as_f64()
                    .filter(|v| *v >= 0.)
                    .ok_or_else(|| {
                        Error::invalid(
                            "stream_subscriber_required",
                            "latest 需要 subscriber 句柄；先用 action=subscribe 订阅",
                        )
                    })? as u64;
                if let Some(frame) = self.bus.pop(id) {
                    let mut v = serde_json::to_value(frame).unwrap();
                    v["has_frame"] = true.into();
                    v["pending"] = self.bus.pending(id).into();
                    Ok(v)
                } else {
                    Ok(
                        json!({"has_frame":false,"pending":self.bus.pending(id),"note":"订阅队列为空：本次没有新帧，不是错误"}),
                    )
                }
            }
            "start" | "stop" => Err(Error::new(
                "unsupported",
                "stream_no_start_stop",
                "spectrum_stream 已不再负责采集，没有 start/stop 可调。要开工请用 measurement_start（连续出光 + 采集 + 落盘 + 显示），要停请用 measurement_stop；本命令只做 subscribe / unsubscribe / latest / status",
            )),
            other => Err(Error::invalid(
                "stream_bad_action",
                format!("action 只能是 status / subscribe / unsubscribe / latest，收到: {other}"),
            )),
        }
    }
    fn measurement_status(&self, available: bool) -> Value {
        let s = self.measurement.lock();
        let w = self.writer.state.lock();
        let (code, message) = if w.error_code.is_empty() {
            (&s.last_error_code, &s.last_error_message)
        } else {
            (&w.error_code, &w.error_message)
        };
        let mut v = json!({"running":self.measuring.load(Ordering::Acquire),"shots_captured":s.shots_captured,"shots_failed":s.shots_failed,"disk_writes_ok":w.writes_ok,"disk_writes_failed":w.writes_failed,"disk_backlog":w.queue.len(),"last_error_code":code,"last_error_message":message});
        if !code.is_empty() {
            v["ok"] = false.into();
        }
        if available {
            v["available"] = true.into();
            v["laser_session_active"] = self.session.load(Ordering::Acquire).into();
        }
        v
    }
    fn start_measurement(self: &Arc<Self>, args: &Value) -> Result<Value> {
        let params = self.parameters.lock().fire(args)?;
        let _lifecycle = self.lifecycle.lock();
        if self.session.swap(true, Ordering::AcqRel) {
            return Err(Error::new(
                "device",
                "laser_already_running",
                "已有出光会话在进行（测量作业已在运行）；要停止请用 measurement_stop",
            ));
        }
        let result = self.start_inner(params.clone());
        match result {
            Err(e) => {
                self.session.store(false, Ordering::Release);
                self.log(
                    "exception_recovery",
                    "测量作业启动失败",
                    json!({"error_code":e.code,"error":e.message}),
                );
                Err(e)
            }
            Ok(trace) => {
                self.log("state_change","连续出光测量作业开始",json!({"settle_ms":trace.settle_ms,"discharge_counter_at_start":trace.counter_before}));
                Ok(
                    json!({"running":true,"note":"光谱仪已布防后开光闸，测量帧由统一作业采集、分发和落盘","settle_ms":trace.settle_ms,"discharge_counter_at_start":trace.counter_before,
                    "session_parameters":format!("来自参数库已标定值 + 本次覆盖：电流 {:0.0}A / 脉宽 {:0.0}us / 频率 {:0.0}Hz / Q 延时 {:0.0}us",params.laser["discharge_current_a"],params.laser["pulse_width_us"],params.laser["frequency_hz"],params.laser["q_delay_us"]),
                    "display_hint":"帧已经自动 publish 到帧队列。UI 订阅 spectrum_stream {action:subscribe} 即可实时显示，无需再单独启动预览流",
                    "warning":"激光正在持续出光。停止请调用 measurement_stop；守护进程退出时也会自动收光，但操作员离开前必须确认光闸已关","sent_frames":trace.sent_frames}),
                )
            }
        }
    }
    fn start_inner(self: &Arc<Self>, params: FireParameters) -> Result<LaserTrace> {
        if let Some(worker) = self.worker.lock().take() {
            let _ = worker.join();
        }
        self.writer.stop();
        let laser = self
            .laser
            .as_ref()
            .ok_or_else(|| Error::new("unsupported", "laser_not_open", "激光器未打开"))?;
        let gate = self.evaluate(false);
        if gate["allowed"] != true {
            return Err(Error::new(
                "device",
                "gate_blocked",
                format!(
                    "门禁未放行，未发出任何写指令：{}",
                    gate["detail"].as_str().unwrap()
                ),
            ));
        }
        params
            .spectrum
            .validate_rate(params.laser["frequency_hz"] as u32)?;
        let trace = laser.prepare(&params)?;
        self.cancel.store(false, Ordering::Release);
        self.measuring.store(true, Ordering::Release);
        self.writer.start();
        let (sender, receiver) = mpsc::sync_channel(1);
        let runtime = self.clone();
        *self.worker.lock() = Some(std::thread::spawn(move || {
            let sender = Mutex::new(Some(sender));
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                runtime.measure_loop(params, trace, &sender)
            }));
            if result.is_err() {
                runtime.cancel.store(true, Ordering::Release);
                if let Some(laser) = &runtime.laser {
                    let _ = laser.stop();
                }
                runtime.measuring.store(false, Ordering::Release);
                let e = Error::new(
                    "internal",
                    "measurement_worker_exception",
                    "测量线程发生未知异常",
                );
                runtime.set_measurement_error(&e);
                if let Some(sender) = sender.lock().take() {
                    let _ = sender.send(Err(e));
                }
            }
        }));
        let result = receiver.recv().unwrap_or_else(|_| {
            Err(Error::new(
                "internal",
                "measurement_worker_exception",
                "测量线程发生未知异常",
            ))
        });
        if result.is_err() {
            let _ = self.stop_measurement();
        }
        result
    }
    fn measure_loop(
        &self,
        params: FireParameters,
        mut trace: LaserTrace,
        sender: &Mutex<Option<mpsc::SyncSender<Result<LaserTrace>>>>,
    ) {
        let mut first = true;
        let mut failures = 0;
        let mut checked = Instant::now();
        while !self.cancel.load(Ordering::Acquire) {
            if checked.elapsed() >= Duration::from_millis(500) {
                checked = Instant::now();
                let gate = self.evaluate(!first);
                if gate["allowed"] != true {
                    let e = Error::new(
                        "device",
                        "gate_lost_mid_run",
                        format!(
                            "运行期重判发现门禁已关闭，作业自行收光：{}",
                            gate["detail"].as_str().unwrap()
                        ),
                    );
                    self.set_measurement_error(&e);
                    if let Some(laser) = &self.laser {
                        let _ = laser.stop();
                    }
                    self.cancel.store(true, Ordering::Release);
                    self.measuring.store(false, Ordering::Release);
                    let counters = {
                        let s = self.measurement.lock();
                        (s.shots_captured, s.shots_failed)
                    };
                    self.log("state_change","运行期门禁关闭，已自行收光",json!({"error_code":e.code,"detail":gate["detail"],"shots_captured":counters.0,"shots_failed":counters.1}));
                    break;
                }
            }
            let mut m = if first {
                params.spectrum
            } else {
                self.parameters.lock().spectrum()
            };
            if params.overrides[0] {
                m.integration_ms = params.spectrum.integration_ms;
            }
            if params.overrides[1] {
                m.timeout_ms = params.spectrum.timeout_ms;
            }
            if params.overrides[2] {
                m.integration_delay_ns = params.spectrum.integration_delay_ns;
            }
            let index = self.frame_index.fetch_add(1, Ordering::Relaxed) + 1;
            let mut armed = || {
                let gate = self.evaluate(false);
                let opened = if gate["allowed"] == true {
                    self.laser.as_ref().unwrap().open_gate(&mut trace)
                } else {
                    Err(Error::new(
                        "device",
                        "gate_lost_during_startup",
                        format!("布防完成后门禁未放行：{}", gate["detail"].as_str().unwrap()),
                    ))
                };
                if let Some(sender) = sender.lock().take() {
                    let _ =
                        sender.send(opened.as_ref().map(|_| trace.clone()).map_err(Clone::clone));
                }
                opened
            };
            let result = (|| {
                m.validate_rate(params.laser["frequency_hz"] as u32)?;
                let _acquisition = self.acquisition.lock();
                let mut spec = self.spectrometer.lock();
                let spec = spec.as_mut().ok_or_else(|| {
                    Error::new(
                        "unsupported",
                        "spectrometer_not_open",
                        "光谱仪未打开或未启用，无法采集",
                    )
                })?;
                spec.capture(
                    &self.config.spectrometer,
                    m,
                    true,
                    index,
                    if first {
                        Some(&mut armed as &mut dyn FnMut() -> Result<()>)
                    } else {
                        None
                    },
                    &self.cancel,
                )
                .map(|(frame, _)| frame)
            })();
            if first {
                if let Some(sender) = sender.lock().take() {
                    let e = result.as_ref().err().cloned().unwrap_or_else(|| {
                        Error::new("device", "capture_not_armed", "光谱仪未完成布防")
                    });
                    let _ = sender.send(Err(e));
                    self.cancel.store(true, Ordering::Release);
                }
                first = false;
            }
            if self.cancel.load(Ordering::Acquire) {
                break;
            }
            match result {
                Ok(frame) => {
                    failures = 0;
                    self.measurement.lock().shots_captured += 1;
                    let frame = Arc::new(frame);
                    self.bus.publish(frame.clone());
                    self.writer.enqueue(frame);
                }
                Err(e) => {
                    failures += 1;
                    self.measurement.lock().shots_failed += 1;
                    self.set_measurement_error(&e);
                    self.bus.publish(Arc::new(Frame::failed(
                        index,
                        self.config.spectrometer.synchronized,
                        m.timeout_ms,
                        &e,
                    )));
                    let fatal = [
                        "laser_spectrometer_rate_mismatch",
                        "capture_parameters_invalid",
                        "spectrometer_disconnected",
                        "serial_not_found",
                        "serial_not_available",
                        "no_devices",
                        "spectrometer_not_open",
                    ]
                    .contains(&e.code.as_str());
                    if fatal || failures >= 3 {
                        self.cancel.store(true, Ordering::Release);
                        let stopped = self.laser.as_ref().unwrap().stop();
                        self.measuring.store(false, Ordering::Release);
                        self.measurement.lock().last_error_message = format!(
                            "{}{}{}",
                            e.message,
                            if fatal {
                                "；设备或配置异常，已请求自动停止测量。"
                            } else {
                                "；连续三次采集失败，已请求自动停止测量。"
                            },
                            stopped
                                .err()
                                .map(|e| format!(" 停止回读失败：{}", e.message))
                                .unwrap_or_default()
                        );
                        self.log("exception_recovery","采集异常，自动停止测量",json!({"error_code":e.code,"detail":e.message,"consecutive_failures":failures}));
                        break;
                    }
                    self.log("exception_recovery","采集失败，等待重试",json!({"error_code":e.code,"detail":e.message,"consecutive_failures":failures}));
                    for _ in 0..4 {
                        if self.cancel.load(Ordering::Acquire) {
                            break;
                        }
                        std::thread::sleep(Duration::from_millis(25));
                    }
                }
            }
        }
    }
    fn set_measurement_error(&self, e: &Error) {
        let mut s = self.measurement.lock();
        s.last_error_code = e.code.clone();
        s.last_error_message = e.message.clone();
    }
    fn stop_measurement(&self) -> Result<()> {
        self.cancel.store(true, Ordering::Release);
        let result = self.laser.as_ref().map(|l| l.stop()).unwrap_or_else(|| {
            Err(Error::new(
                "unsupported",
                "laser_not_open",
                "激光器未打开，无法收光",
            ))
        });
        if let Some(worker) = self.worker.lock().take() {
            let _ = worker.join();
        }
        self.writer.stop();
        self.measuring.store(false, Ordering::Release);
        self.session.store(false, Ordering::Release);
        self.bus.clear();
        result
    }
    fn capture(&self) -> Result<Value> {
        let _acquisition = self
            .acquisition
            .try_lock()
            .filter(|_| !self.session.load(Ordering::Acquire))
            .ok_or_else(|| {
                Error::new(
                    "device",
                    "acquisition_busy",
                    "测量已占用光谱仪，独立采集被拒绝",
                )
            })?;
        let m = self.parameters.lock().spectrum();
        let mut spec = self.spectrometer.lock();
        let spec = spec.as_mut().ok_or_else(|| {
            Error::new(
                "unsupported",
                "spectrometer_not_open",
                "光谱仪未打开或未启用，无法采集",
            )
        })?;
        let (frame, arm_order) = spec.capture(
            &self.config.spectrometer,
            m,
            self.config.spectrometer.wait_for_laser_trigger,
            0,
            None,
            &AtomicBool::new(false),
        )?;
        let delivery = save_capture(
            &self.data_dir.join("captures"),
            &self.config.station_id,
            &frame,
            &arm_order,
        );
        *self.last_delivery.lock() = json!({"local_saved":delivery["local_saved"],"local_path":delivery["local_path"],"gateway_delivered":false});
        self.log("general", "采集交付状态", delivery.clone());
        if delivery["local_saved"] != true {
            return Err(Error::new(
                "device",
                "delivery_local_failed",
                format!(
                    "本地交付失败，本次采集的光谱没有落盘：{}",
                    delivery["local_error"].as_str().unwrap_or("")
                ),
            )
            .with_payload(json!({"captured":true,"delivery":delivery})));
        }
        Ok(
            json!({"expected_groups":frame.expected_groups,"received_groups":frame.received_groups,"measurement_source":"parameters.json","scan_groups":[{"shot_id":frame.shot_id,"complete":true,"hardware_synchronized":frame.hardware_synchronized,"requested_sync":self.config.spectrometer.synchronized,"arm_order":arm_order}],
            "note":"hardware_synchronized 原样上送 SDK 事实；业务层不宣称物理同步（范围报告 §3.10.1）","delivery":delivery}),
        )
    }
    pub fn stop(&self) {
        let _lifecycle = self.lifecycle.lock();
        if !self.running.swap(false, Ordering::AcqRel) {
            return;
        }
        self.log("general", "运行结束，开始优雅关闭", json!({}));
        self.monitor.lock().armed = false;
        let stopped = self.stop_measurement();
        self.log(
            "general",
            if stopped.is_ok() {
                "已停止测量作业并收光"
            } else {
                "停止测量作业时收光未确认"
            },
            json!({}),
        );
        self.prediction.stop();
        self.apply_lamp("offline");
        if let Some(mut spec) = self.spectrometer.lock().take() {
            let _ = spec.close();
        }
        self.indicator.lock().take();
        self.log("general", "设备已关闭", json!({}));
    }
}

pub fn capture_payload(frame: &Frame, arm_order: &[String]) -> Value {
    let channels:Vec<_>=frame.channels.iter().filter(|c|c.state=="present").map(|c|json!({"serial":c.serial,"pixels":c.pixels,"device_time_ticks":c.device_time_ticks,"wavelength_nm":c.wavelength_nm,"counts":c.counts})).collect();
    json!({"scan_groups":[{"shot_id":frame.shot_id,"complete":frame.ok,"hardware_synchronized":frame.hardware_synchronized,"arm_order":arm_order,"channels":channels}]})
}
fn save_capture(directory: &Path, station: &str, frame: &Frame, arm_order: &[String]) -> Value {
    let mut delivery = json!({"local_saved":false,"local_path":"","local_error":"","gateway_delivered":false,"gateway_error":GATEWAY});
    if let Err(e) = std::fs::create_dir_all(directory) {
        delivery["local_error"] =
            format!("cannot create directory: {} ({e})", directory.display()).into();
        return delivery;
    }
    let prefix = if station.is_empty() {
        String::new()
    } else {
        format!("{station}_")
    };
    let path = directory.join(format!(
        "cap_{prefix}t{}_shot{}",
        frame.wall_clock_ms, frame.shot_id
    ));
    let result = File::create(&path).and_then(|mut f| {
        let blob = serde_json::to_vec(&capture_payload(frame, arm_order)).unwrap();
        f.write_all(&blob)?;
        f.flush()
    });
    if result.is_ok() {
        delivery["local_saved"] = true.into();
        delivery["local_path"] = path.to_string_lossy().replace('\\', "/").into();
    } else {
        delivery["local_error"] = format!("cannot open file for write: {}", path.display()).into();
    }
    delivery
}
fn parameter_info(group: &str, field: &str) -> (&'static str, &'static str) {
    match (group, field) {
        ("laser", "discharge_current_a") => ("A", "激光协议 §3.1"),
        ("laser", "frequency_hz") => ("Hz", "激光协议 §3.1"),
        ("laser", "q_warmup_s") => ("s", "软件限制，厂家协议未标量程"),
        ("laser", "settle_timeout_ms") => ("ms", "软件等待上限"),
        ("laser", "emission_hold_ms") => ("ms", "软件会话上限"),
        ("laser", "min_discharges") => ("次", "软件判据上限"),
        ("spectrometer", "integration_ms") => ("ms", "SDK 通用下限与软件上限；探测器另由 SDK 校验"),
        ("spectrometer", "capture_timeout_ms") => ("ms", "软件采集等待上限"),
        ("spectrometer", "integration_delay_ns") => ("ns", "软件延时上限"),
        _ => ("us", "激光协议 §3.1"),
    }
}
