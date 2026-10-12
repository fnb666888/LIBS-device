use crate::{
    algorithm::{self, Engine},
    error::{Error, Result},
    frames::{Frame, FrameBus},
};
use parking_lot::{Condvar, Mutex};
use serde_json::{Value, json};
use std::{
    collections::BTreeSet,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread::JoinHandle,
    time::Duration,
};

pub fn contract() -> Value {
    serde_json::from_str(include_str!("../contracts/algorithm-v1.json")).unwrap()
}
fn invalid(path: &str, message: &str) -> Error {
    Error::invalid("algorithm_input_invalid", format!("{path}: {message}"))
}
fn nonempty(v: &Value) -> bool {
    v.as_str()
        .is_some_and(|s| !s.trim_matches([' ', '\t', '\r', '\n']).is_empty())
}
fn integer(v: &Value, min: f64) -> bool {
    v.as_f64()
        .is_some_and(|v| v >= min && v <= 9007199254740991. && v.fract() == 0.)
}
pub fn validate(input: &Value) -> Result<Value> {
    if !input.is_object() {
        return Err(invalid("input", "must be an object"));
    }
    if input["schema_version"] != "libs.algorithm.v1" {
        return Err(invalid("input.schema_version", "must be libs.algorithm.v1"));
    }
    if !nonempty(&input["request_id"]) {
        return Err(invalid("input.request_id", "must be a non-empty string"));
    }
    let mut missing = vec![];
    for key in ["context", "parameters", "model"] {
        if let Some(v) = input.get(key) {
            if !v.is_object() {
                return Err(invalid(
                    &format!("input.{key}"),
                    "must be an object when supplied",
                ));
            }
        } else {
            missing.push(format!("input.{key}"));
        }
    }
    let source = if input.get("spectrum").is_some() {
        "spectrum"
    } else {
        "raw_capture"
    };
    let spectrum = &input[source];
    let sp = format!("input.{source}");
    if !spectrum.is_object() {
        return Err(invalid(
            "input.spectrum",
            "must supply an object containing real captured spectra",
        ));
    }
    if spectrum["spectrum_source"] != "LIBS" {
        return Err(invalid(&format!("{sp}.spectrum_source"), "must be LIBS"));
    }
    if spectrum["spectrum_format"] != "wavelength_nm+counts" {
        return Err(invalid(
            &format!("{sp}.spectrum_format"),
            "must be wavelength_nm+counts",
        ));
    }
    let groups = spectrum["scan_groups"]
        .as_array()
        .filter(|v| !v.is_empty())
        .ok_or_else(|| {
            invalid(
                &format!("{sp}.scan_groups"),
                "must be a non-empty array of real captures",
            )
        })?;
    let mut channel_count = 0;
    let mut sample_count = 0;
    let mut all = BTreeSet::new();
    let mut incomplete = vec![];
    let mut metadata = vec![];
    for (gi, group) in groups.iter().enumerate() {
        let gp = format!("{sp}.scan_groups[{gi}]");
        if !group.is_object() {
            return Err(invalid(&gp, "must be an object"));
        }
        if let Some(v) = group.get("shot_id") {
            if !integer(v, 0.) {
                return Err(invalid(
                    &format!("{gp}.shot_id"),
                    "must be a non-negative exact integer",
                ));
            }
        }
        for key in ["shot_id", "complete", "hardware_synchronized", "arm_order"] {
            if group.get(key).is_none() {
                metadata.push(format!("{gp}.{key}"));
            }
        }
        for key in ["complete", "hardware_synchronized"] {
            if group.get(key).is_some_and(|v| !v.is_boolean()) {
                return Err(invalid(&format!("{gp}.{key}"), "must be boolean"));
            }
        }
        if group["complete"] == false {
            incomplete.push(gi);
        }
        if let Some(v) = group.get("arm_order") {
            let a = v
                .as_array()
                .ok_or_else(|| invalid(&format!("{gp}.arm_order"), "must be an array"))?;
            if a.iter().any(|v| !nonempty(v)) {
                return Err(invalid(
                    &format!("{gp}.arm_order"),
                    "each entry must be a non-empty serial string",
                ));
            }
        }
        let channels = group["channels"]
            .as_array()
            .filter(|v| !v.is_empty())
            .ok_or_else(|| {
                invalid(
                    &format!("{gp}.channels"),
                    "must contain at least one captured channel",
                )
            })?;
        let mut serials = BTreeSet::new();
        for (ci, c) in channels.iter().enumerate() {
            let cp = format!("{gp}.channels[{ci}]");
            if !c.is_object() {
                return Err(invalid(&cp, "must be an object"));
            }
            if !nonempty(&c["serial"]) {
                return Err(invalid(
                    &format!("{cp}.serial"),
                    "must be a non-empty string",
                ));
            }
            let serial = c["serial"].as_str().unwrap();
            if !serials.insert(serial) {
                return Err(invalid(
                    &format!("{cp}.serial"),
                    "duplicate serial within this scan group",
                ));
            }
            all.insert(serial);
            let waves = c["wavelength_nm"]
                .as_array()
                .filter(|v| !v.is_empty())
                .ok_or_else(|| {
                    invalid(
                        &format!("{cp}.wavelength_nm"),
                        "must be a non-empty number array",
                    )
                })?;
            let counts = c["counts"]
                .as_array()
                .filter(|v| !v.is_empty())
                .ok_or_else(|| {
                    invalid(&format!("{cp}.counts"), "must be a non-empty number array")
                })?;
            if waves.len() != counts.len() {
                return Err(invalid(&cp, "wavelength_nm and counts lengths must match"));
            }
            if let Some(v) = c.get("pixels") {
                if !integer(v, 1.) || v.as_f64() != Some(counts.len() as f64) {
                    return Err(invalid(
                        &format!("{cp}.pixels"),
                        "must be a positive exact integer equal to array lengths",
                    ));
                }
            } else {
                metadata.push(format!("{cp}.pixels"));
            }
            if let Some(v) = c.get("device_time_ticks") {
                if !integer(v, 0.) {
                    return Err(invalid(
                        &format!("{cp}.device_time_ticks"),
                        "must be a non-negative exact integer",
                    ));
                }
            } else {
                metadata.push(format!("{cp}.device_time_ticks"));
            }
            for (pi, (w, c)) in waves.iter().zip(counts).enumerate() {
                if !w.as_f64().is_some_and(|v| v.is_finite() && v > 0.) {
                    return Err(invalid(
                        &format!("{cp}.wavelength_nm[{pi}]"),
                        "must be a positive finite number",
                    ));
                }
                if !c.as_f64().is_some_and(f64::is_finite) {
                    return Err(invalid(
                        &format!("{cp}.counts[{pi}]"),
                        "must be a finite number",
                    ));
                }
            }
            channel_count += 1;
            sample_count += counts.len();
        }
    }
    Ok(
        json!({"valid":true,"schema_version":"libs.algorithm.v1","request_id":input["request_id"],"spectrum_source":"LIBS","scan_group_count":groups.len(),
        "channel_capture_count":channel_count,"sample_count":sample_count,"channel_serials":all,"incomplete_group_indices":incomplete,"missing_optional_fields":missing,
        "missing_metadata":metadata,"input_source_key":source,"note":"仅校验结构和数值有效性；不判断光谱质量、物理同步或成分准确性"}),
    )
}

pub fn frame_input(frame: &Frame, branch: &str, request: &str) -> Value {
    let channels: Vec<_> = frame
        .channels
        .iter()
        .filter(|c| c.state == "present")
        .collect();
    json!({"schema_version":"libs.algorithm.v1","request_id":request,"context":{"frame_index":frame.frame_index,"shot_id":frame.shot_id,"captured_at_ms":frame.wall_clock_ms},
        "parameters":{"branch":branch},"spectrum":{"spectrum_source":"LIBS","spectrum_format":"wavelength_nm+counts","scan_groups":[{"shot_id":frame.shot_id,
            "hardware_synchronized":frame.hardware_synchronized,"complete":frame.ok&&frame.expected_groups==frame.received_groups&&channels.len()==frame.channels.len(),"channels":channels}]}})
}
#[derive(Clone)]
struct Task {
    input: Value,
    source: &'static str,
    generation: u64,
}
struct State {
    enabled: bool,
    branch: String,
    state: &'static str,
    detail: String,
    error_code: String,
    processing: bool,
    active: String,
    completed: u64,
    ineligible: u64,
    failed: u64,
    skipped: u64,
    model: Value,
    latest: Value,
    generation: u64,
    next: u64,
    last_frame: Option<u64>,
    pending: Option<Task>,
    parameters: Value,
    revision: u64,
    reload: bool,
}
impl Default for State {
    fn default() -> Self {
        Self {
            enabled: false,
            branch: String::new(),
            state: "disabled",
            detail: String::new(),
            error_code: String::new(),
            processing: false,
            active: String::new(),
            completed: 0,
            ineligible: 0,
            failed: 0,
            skipped: 0,
            model: Value::Null,
            latest: Value::Null,
            generation: 0,
            next: 0,
            last_frame: None,
            pending: None,
            parameters: json!({}),
            revision: 0,
            reload: false,
        }
    }
}
pub struct Prediction {
    state: Mutex<State>,
    wake: Condvar,
    stop: AtomicBool,
    worker: Mutex<Option<JoinHandle<()>>>,
    bus: Arc<FrameBus>,
    subscription: u64,
    directory: PathBuf,
    algorithm_dir: PathBuf,
    epoch: AtomicU64,
    external: AtomicBool,
}
impl Prediction {
    pub fn start(bus: Arc<FrameBus>, directory: PathBuf, algorithm_dir: PathBuf) -> Arc<Self> {
        let subscription = bus.subscribe();
        let service = Arc::new(Self {
            state: Mutex::new(State::default()),
            wake: Condvar::new(),
            stop: AtomicBool::new(false),
            worker: Mutex::new(None),
            bus,
            subscription,
            directory,
            external: AtomicBool::new(algorithm::uses_external(&algorithm_dir)),
            algorithm_dir,
            epoch: AtomicU64::new(0),
        });
        let clone = service.clone();
        *service.worker.lock() = Some(std::thread::spawn(move || clone.run()));
        service
    }
    pub fn status(&self) -> Value {
        let s = self.state.lock();
        json!({"available":true,"enabled":s.enabled,"branch":s.branch,"state":s.state,"detail":s.detail,"error_code":s.error_code,"processing":s.processing,
        "active_request_id":s.active,"completed":s.completed,"ineligible":s.ineligible,"failed":s.failed,"skipped_frames":s.skipped,"model":s.model,"latest":s.latest})
    }
    pub fn configure(&self, enabled: bool, branch: &str) -> Result<Value> {
        self.configure_parameters(enabled, branch, None)
    }
    pub fn configure_parameters(
        &self,
        enabled: bool,
        branch: &str,
        parameters: Option<&Value>,
    ) -> Result<Value> {
        let mut parameters = parameters.cloned().unwrap_or(json!({}));
        if !parameters.is_object() {
            return Err(Error::invalid(
                "prediction_parameters_invalid",
                "parameters must be an object",
            ));
        }
        if !branch.is_empty() {
            parameters["branch"] = branch.into();
        }
        let branch = parameters["branch"].as_str().unwrap_or("").to_owned();
        if enabled
            && !self.external.load(Ordering::Acquire)
            && !["oxide", "sulfide"].contains(&branch.as_str())
        {
            return Err(Error::invalid(
                "prediction_branch_required",
                "请选择氧化矿或硫化矿模型分支",
            ));
        }
        if enabled && self.external.load(Ordering::Acquire) {
            validate_mode(&parameters)?;
        }
        if self.stop.load(Ordering::Acquire) {
            return Err(Error::invalid("prediction_stopped", "预测服务已停止"));
        }
        while self.bus.pop(self.subscription).is_some() {}
        let mut s = self.state.lock();
        s.generation += 1;
        self.epoch.store(s.generation, Ordering::Release);
        s.enabled = enabled;
        s.branch = if enabled { branch } else { String::new() };
        s.state = if enabled { "loading" } else { "disabled" };
        s.detail.clear();
        s.error_code.clear();
        s.latest = Value::Null;
        s.active.clear();
        s.pending = None;
        s.last_frame = None;
        s.parameters = if enabled { parameters } else { json!({}) };
        drop(s);
        self.wake.notify_all();
        Ok(self.status())
    }
    pub fn reload(&self) -> Result<Value> {
        if self.stop.load(Ordering::Acquire) {
            return Err(Error::invalid("prediction_stopped", "预测服务已停止"));
        }
        let external = algorithm::uses_external(&self.algorithm_dir);
        let mut s = self.state.lock();
        s.generation += 1;
        s.revision += 1;
        self.epoch.store(s.generation, Ordering::Release);
        self.external.store(external, Ordering::Release);
        s.reload = true;
        s.state = "loading";
        s.detail.clear();
        s.error_code.clear();
        s.model = Value::Null;
        s.latest = Value::Null;
        s.pending = None;
        s.active.clear();
        s.last_frame = None;
        drop(s);
        while self.bus.pop(self.subscription).is_some() {}
        self.wake.notify_all();
        Ok(json!({"accepted":true,"state":"loading"}))
    }
    pub fn submit(&self, input: &Value) -> Result<Value> {
        validate(input)?;
        if !self.external.load(Ordering::Acquire)
            && !["oxide", "sulfide"].contains(&input["parameters"]["branch"].as_str().unwrap_or(""))
        {
            return Err(Error::invalid(
                "prediction_branch_required",
                "预测输入须指定 oxide 或 sulfide 分支",
            ));
        }
        if self.external.load(Ordering::Acquire) {
            validate_mode(&input["parameters"])?;
        }
        if serde_json::to_vec(input).unwrap().len() > 32 * 1024 * 1024 {
            return Err(Error::invalid(
                "prediction_input_size",
                "预测输入超过 32 MB",
            ));
        }
        let mut s = self.state.lock();
        if self.stop.load(Ordering::Acquire) {
            return Err(Error::invalid("prediction_stopped", "预测服务已停止"));
        }
        if s.pending.is_some() || s.processing {
            return Err(Error::new(
                "device",
                "prediction_busy",
                "预测正在处理，请稍后提交",
            ));
        }
        s.pending = Some(Task {
            input: input.clone(),
            source: "offline",
            generation: s.generation,
        });
        s.state = "queued";
        s.detail.clear();
        s.error_code.clear();
        s.latest = Value::Null;
        self.wake.notify_all();
        Ok(json!({"accepted":true,"request_id":input["request_id"]}))
    }
    pub fn stop(&self) {
        self.stop.store(true, Ordering::Release);
        self.wake.notify_all();
        if let Some(worker) = self.worker.lock().take() {
            let _ = worker.join();
        }
        self.bus.unsubscribe(self.subscription);
    }
    fn run(&self) {
        let mut model: Option<Engine> = None;
        let mut loaded_generation = u64::MAX;
        let mut loaded_revision = 0;
        while !self.stop.load(Ordering::Acquire) {
            let (mut task, enabled, generation, revision, reload) = {
                let mut s = self.state.lock();
                let task = s.pending.take();
                if task.is_some() {
                    s.processing = true;
                }
                (task, s.enabled, s.generation, s.revision, s.reload)
            };
            if loaded_revision != revision {
                model.take();
                loaded_revision = revision;
                loaded_generation = u64::MAX;
            }
            let cancelled = || {
                self.stop.load(Ordering::Acquire)
                    || self.epoch.load(Ordering::Acquire) != generation
            };
            if task.is_none() && (enabled || reload) && loaded_generation != generation {
                let loaded = if let Some(m) = &model {
                    Ok(Engine::status(m))
                } else {
                    Engine::load(&self.algorithm_dir, &cancelled).map(|m| {
                        let status = m.status();
                        model = Some(m);
                        status
                    })
                };
                let mut s = self.state.lock();
                if generation != s.generation {
                    continue;
                }
                loaded_generation = generation;
                s.reload = false;
                match loaded {
                    Ok(status) => {
                        s.model = status;
                        s.state = if enabled { "waiting" } else { "disabled" };
                        s.detail.clear();
                        s.error_code.clear();
                    }
                    Err(e) => {
                        s.model = Value::Null;
                        s.state = "model_unavailable";
                        s.detail = e.message;
                        s.error_code = e.code;
                        s.failed += 1;
                    }
                }
            }
            if task.is_none() {
                if let Some(frame) = self.bus.pop(self.subscription) {
                    let mut s = self.state.lock();
                    if !s.enabled || generation != s.generation || s.state == "model_unavailable" {
                        continue;
                    }
                    if let Some(last) = s.last_frame {
                        if frame.frame_index > last + 1 {
                            s.skipped += frame.frame_index - last - 1;
                        }
                    }
                    s.last_frame = Some(frame.frame_index);
                    s.next += 1;
                    let request = format!("live-{}-frame-{}", s.next, frame.frame_index);
                    if !frame.ok || !frame.laser_shot {
                        s.latest = unavailable(
                            &request,
                            frame
                                .capture_error_message
                                .as_deref()
                                .unwrap_or("非激光测量帧"),
                        );
                        s.state = "ineligible";
                        s.ineligible += 1;
                        continue;
                    }
                    let mut input = frame_input(&frame, &s.branch, &request);
                    input["parameters"] = s.parameters.clone();
                    task = Some(Task {
                        input,
                        source: "live",
                        generation: s.generation,
                    });
                    s.processing = true;
                }
            }
            if let Some(task) = task {
                {
                    let mut s = self.state.lock();
                    if task.generation != s.generation {
                        s.processing = false;
                        continue;
                    }
                    s.state = "predicting";
                    s.active = task.input["request_id"].as_str().unwrap_or("").into();
                    s.detail.clear();
                    s.error_code.clear();
                }
                let result = (|| {
                    if model.is_none() {
                        model = Some(Engine::load(&self.algorithm_dir, &cancelled)?);
                    }
                    let mut output =
                        model
                            .as_mut()
                            .unwrap()
                            .analyze(&task.input, &self.stop, &cancelled)?;
                    output["validation"] = validate(&task.input)?;
                    Ok::<_, Error>(output)
                })();
                if let Some(engine) = &model {
                    let mut s = self.state.lock();
                    if generation == s.generation {
                        s.model = engine.status();
                    }
                }
                if matches!(&model, Some(Engine::External(_)))
                    && result.as_ref().is_err_and(|e| {
                        ["timeout", "protocol", "transport", "internal"].contains(&e.kind)
                    })
                {
                    model.take();
                }
                let mut record = match &result {
                    Ok(v) => v.clone(),
                    Err(e) => {
                        let mut v = unavailable(
                            task.input["request_id"].as_str().unwrap_or(""),
                            &e.message,
                        );
                        v["error_code"] = e.code.clone().into();
                        v
                    }
                };
                record["source"] = task.source.into();
                record["context"] = task.input["context"].clone();
                record["branch"] = task.input["parameters"]["branch"].clone();
                let index = {
                    let mut s = self.state.lock();
                    if self.stop.load(Ordering::Acquire) || task.generation != s.generation {
                        s.processing = false;
                        continue;
                    }
                    s.next += 1;
                    s.next
                };
                let path = self.directory.join(format!(
                    "prediction-{}-{index}.json",
                    crate::wall_clock_ms()
                ));
                record["saved"] = true.into();
                record["result_path"] = path.to_string_lossy().into_owned().into();
                let saved = std::fs::create_dir_all(&self.directory)
                    .and_then(|_| {
                        std::fs::write(&path, serde_json::to_vec_pretty(&record).unwrap())
                    })
                    .is_ok();
                record["saved"] = saved.into();
                if !saved {
                    record["result_path"] = "".into();
                }
                let mut s = self.state.lock();
                s.processing = false;
                if self.stop.load(Ordering::Acquire) || task.generation != s.generation {
                    continue;
                }
                s.latest = record;
                match result {
                    Ok(_) if s.latest["availability"] == "available" => {
                        s.completed += 1;
                        s.state = "completed";
                    }
                    Ok(_) => {
                        s.ineligible += 1;
                        s.state = "ineligible";
                    }
                    Err(e) => {
                        s.failed += 1;
                        s.state = "error";
                        s.detail = e.message;
                        s.error_code = e.code;
                    }
                }
            } else {
                let mut s = self.state.lock();
                self.wake.wait_for(&mut s, Duration::from_millis(40));
            }
        }
    }
}
fn unavailable(request: &str, message: &str) -> Value {
    json!({"request_id":request,"availability":"unavailable","executed":false,"detail":message})
}
fn validate_mode(parameters: &Value) -> Result<()> {
    if !["calibration_free", "calibrated"].contains(&parameters["mode"].as_str().unwrap_or("")) {
        return Err(Error::invalid(
            "prediction_mode_required",
            "parameters.mode must explicitly be calibration_free or calibrated",
        ));
    }
    Ok(())
}
