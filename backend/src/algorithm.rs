use crate::{
    error::{Error, Result},
    model::Model,
};
use serde::Deserialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    io::{BufRead, BufReader, Write},
    os::windows::io::AsRawHandle,
    os::windows::process::CommandExt,
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    sync::{atomic::AtomicBool, mpsc},
    thread::JoinHandle,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE},
    System::JobObjects::{
        AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation,
        SetInformationJobObject, TerminateJobObject,
    },
};

struct ProcessGroup(HANDLE);
unsafe impl Send for ProcessGroup {}
impl ProcessGroup {
    fn attach(child: &mut Child) -> Result<Self> {
        let job = Self(unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) });
        let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = unsafe { std::mem::zeroed() };
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if job.0.is_null()
            || unsafe {
                SetInformationJobObject(
                    job.0,
                    JobObjectExtendedLimitInformation,
                    (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                    std::mem::size_of_val(&limits) as u32,
                ) == 0
                    || AssignProcessToJobObject(job.0, child.as_raw_handle()) == 0
            }
        {
            let error = std::io::Error::last_os_error();
            let _ = child.kill();
            let _ = child.wait();
            return Err(Error::new(
                "transport",
                "algorithm_process_group",
                error.to_string(),
            ));
        }
        Ok(job)
    }
}
impl Drop for ProcessGroup {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }
}

pub const PROTOCOL: &str = "libs.algorithm.worker.v1";
const MAX_REPLY: usize = 8 * 1024 * 1024;

#[derive(Clone, Deserialize)]
#[serde(default)]
struct Settings {
    engine: String,
    executable: PathBuf,
    args: Vec<String>,
    working_directory: PathBuf,
    timeout_ms: u64,
    executable_sha256: Option<String>,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            engine: "native".into(),
            executable: PathBuf::new(),
            args: vec![],
            working_directory: PathBuf::new(),
            timeout_ms: 60000,
            executable_sha256: None,
        }
    }
}
fn settings_path(directory: &Path) -> PathBuf {
    let local = directory.join("runtime.local.json");
    if local.exists() {
        local
    } else {
        directory.join("runtime.json")
    }
}
pub fn uses_external(directory: &Path) -> bool {
    std::fs::read(settings_path(directory))
        .ok()
        .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
        .is_some_and(|v| v["engine"] == "external")
}
fn resolve(directory: &Path, path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.into()
    } else {
        directory.join(path)
    }
}

pub enum Engine {
    Native(Model),
    External(Worker),
}
impl Engine {
    pub fn load(directory: &Path, cancelled: &dyn Fn() -> bool) -> Result<Self> {
        let path = settings_path(directory);
        let settings: Settings = if path.exists() {
            serde_json::from_slice(
                &std::fs::read(&path)
                    .map_err(|e| Error::new("transport", "algorithm_config_read", e.to_string()))?,
            )
            .map_err(|e| Error::invalid("algorithm_config_invalid", e.to_string()))?
        } else {
            Settings::default()
        };
        match settings.engine.as_str() {
            "native" => Ok(Self::Native(Model::load(directory)?)),
            "external" => {
                if settings.executable.as_os_str().is_empty()
                    || !(1000..=120000).contains(&settings.timeout_ms)
                {
                    return Err(Error::invalid(
                        "algorithm_config_invalid",
                        "external engine requires executable and timeout_ms in 1000..120000",
                    ));
                }
                let mut worker = Worker::start(directory, settings)?;
                let description = worker.call("describe", None, cancelled)?;
                validate_description(&description)?;
                worker.description = description;
                Ok(Self::External(worker))
            }
            _ => Err(Error::invalid(
                "algorithm_engine_invalid",
                "engine must be native or external",
            )),
        }
    }
    pub fn status(&self) -> Value {
        match self {
            Self::Native(model) => model.status(),
            Self::External(worker) => worker.description.clone(),
        }
    }
    pub fn analyze(
        &mut self,
        input: &Value,
        stop: &AtomicBool,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Value> {
        match self {
            Self::Native(model) => model.analyze(input, stop),
            Self::External(worker) => {
                let mode = &input["parameters"]["mode"];
                if !worker.description["modes"]
                    .as_array()
                    .unwrap()
                    .contains(mode)
                {
                    return Err(Error::invalid(
                        "algorithm_mode_unsupported",
                        "Selected algorithm does not support the requested calibration mode",
                    ));
                }
                let output = worker.call("analyze", Some(input), cancelled)?;
                validate_result(input, &output)?;
                if output["model"]["model_id"] != worker.description["model_id"]
                    || output["model"]["version"] != worker.description["version"]
                {
                    return Err(Error::new(
                        "protocol",
                        "algorithm_version_mismatch",
                        "Result model identity/version differs from describe",
                    ));
                }
                Ok(output)
            }
        }
    }
}

pub struct Worker {
    child: Child,
    stdin: Option<ChildStdin>,
    replies: mpsc::Receiver<Result<Value>>,
    reader: Option<JoinHandle<()>>,
    description: Value,
    timeout: Duration,
    next_id: u64,
    group: ProcessGroup,
}
impl Worker {
    fn start(directory: &Path, settings: Settings) -> Result<Self> {
        let executable = resolve(directory, &settings.executable);
        if let Some(expected) = settings.executable_sha256 {
            let bytes = std::fs::read(&executable)
                .map_err(|e| Error::new("transport", "algorithm_executable_read", e.to_string()))?;
            if format!("{:x}", Sha256::digest(bytes)) != expected.to_ascii_lowercase() {
                return Err(Error::new(
                    "protocol",
                    "algorithm_hash_mismatch",
                    "Algorithm executable SHA-256 mismatch",
                ));
            }
        }
        let mut child = Command::new(&executable)
            .args(&settings.args)
            .current_dir(resolve(directory, &settings.working_directory))
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .creation_flags(0x08000000)
            .spawn()
            .map_err(|e| {
                Error::new(
                    "transport",
                    "algorithm_start_failed",
                    format!("{}: {e}", executable.display()),
                )
            })?;
        let group = ProcessGroup::attach(&mut child)?;
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().unwrap();
        let (sender, replies) = mpsc::sync_channel(1);
        let reader = std::thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                let result = read_reply(&mut reader);
                let failed = result.is_err();
                if sender.send(result).is_err() || failed {
                    break;
                }
            }
        });
        Ok(Self {
            child,
            stdin,
            replies,
            reader: Some(reader),
            description: Value::Null,
            timeout: Duration::from_millis(settings.timeout_ms),
            next_id: 0,
            group,
        })
    }
    fn call(
        &mut self,
        action: &str,
        input: Option<&Value>,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Value> {
        self.next_id += 1;
        let id = self.next_id;
        let mut request = json!({"protocol":PROTOCOL,"id":id,"action":action});
        if let Some(input) = input {
            request["input"] = input.clone();
        }
        let mut bytes = serde_json::to_vec(&request).unwrap();
        bytes.push(b'\n');
        if self.stdin.is_none() {
            return Err(Error::new(
                "transport",
                "algorithm_stopped",
                "Algorithm process stopped",
            ));
        }
        // The dedicated writer prevents a worker that stops reading stdin from blocking shutdown.
        let mut stdin = self.stdin.take().unwrap();
        let (sent, written) = mpsc::sync_channel(1);
        std::thread::spawn(move || {
            let result = stdin.write_all(&bytes).and_then(|_| stdin.flush());
            let _ = sent.send((stdin, result));
        });
        let started = Instant::now();
        let result = (|| {
            loop {
                self.check_deadline(started, cancelled)?;
                match written.recv_timeout(Duration::from_millis(20)) {
                    Ok((stdin, result)) => {
                        self.stdin = Some(stdin);
                        result.map_err(|e| {
                            Error::new("transport", "algorithm_write_failed", e.to_string())
                        })?;
                        break;
                    }
                    Err(mpsc::RecvTimeoutError::Timeout) => (),
                    Err(_) => {
                        return Err(Error::new(
                            "transport",
                            "algorithm_write_failed",
                            "Algorithm input pipe closed",
                        ));
                    }
                }
            }
            let reply = loop {
                self.check_deadline(started, cancelled)?;
                match self.replies.recv_timeout(Duration::from_millis(20)) {
                    Ok(reply) => break reply?,
                    Err(mpsc::RecvTimeoutError::Timeout) => (),
                    Err(_) => {
                        return Err(Error::new(
                            "transport",
                            "algorithm_exited",
                            "Algorithm output pipe closed",
                        ));
                    }
                }
            };
            if reply["protocol"] != PROTOCOL
                || reply["id"].as_u64() != Some(id)
                || !reply["ok"].is_boolean()
            {
                return Err(Error::new(
                    "protocol",
                    "algorithm_bad_envelope",
                    "Expected matching protocol/id and boolean ok",
                ));
            }
            if reply["ok"] == false {
                let message = reply["error"]["message"]
                    .as_str()
                    .unwrap_or("Algorithm execution failed");
                let code = reply["error"]["code"]
                    .as_str()
                    .unwrap_or("algorithm_execution_failed");
                return Err(Error::new("device", code, message));
            }
            if !reply["payload"].is_object() {
                return Err(Error::new(
                    "protocol",
                    "algorithm_bad_payload",
                    "Algorithm payload must be an object",
                ));
            }
            Ok(reply["payload"].clone())
        })();
        if result
            .as_ref()
            .is_err_and(|e| ["timeout", "internal", "transport", "protocol"].contains(&e.kind))
        {
            self.terminate();
        }
        result
    }
    fn check_deadline(&mut self, started: Instant, cancelled: &dyn Fn() -> bool) -> Result<()> {
        if cancelled() {
            return Err(Error::new(
                "internal",
                "algorithm_cancelled",
                "Algorithm task cancelled",
            ));
        }
        if started.elapsed() >= self.timeout {
            return Err(Error::new(
                "timeout",
                "algorithm_timeout",
                "Algorithm exceeded timeout_ms",
            ));
        }
        Ok(())
    }
    fn terminate(&mut self) {
        unsafe {
            TerminateJobObject(self.group.0, 1);
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
        self.stdin.take();
    }
}
impl Drop for Worker {
    fn drop(&mut self) {
        self.terminate();
        // Discard pending unsolicited output before joining the bounded reader.
        while self.reader.as_ref().is_some_and(|r| !r.is_finished()) {
            let _ = self.replies.recv_timeout(Duration::from_millis(20));
        }
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
    }
}
fn read_reply(reader: &mut impl BufRead) -> Result<Value> {
    let mut line = Vec::new();
    loop {
        let bytes = reader
            .fill_buf()
            .map_err(|e| Error::new("transport", "algorithm_read_failed", e.to_string()))?;
        if bytes.is_empty() {
            return Err(Error::new(
                "transport",
                "algorithm_exited",
                "Algorithm exited before a complete response",
            ));
        }
        let end = bytes.iter().position(|b| *b == b'\n');
        let count = end.map_or(bytes.len(), |i| i + 1);
        if line.len() + count > MAX_REPLY {
            return Err(Error::new(
                "protocol",
                "algorithm_reply_size",
                "Algorithm reply exceeds 8 MB",
            ));
        }
        line.extend_from_slice(&bytes[..count]);
        reader.consume(count);
        if end.is_some() {
            break;
        }
    }
    serde_json::from_slice(&line)
        .map_err(|e| Error::new("protocol", "algorithm_bad_json", e.to_string()))
}
fn validate_description(value: &Value) -> Result<()> {
    let strings = ["model_id", "version", "input_schema", "output_schema"];
    if strings
        .iter()
        .any(|k| !value[*k].as_str().is_some_and(|s| !s.trim().is_empty()))
        || value["input_schema"] != "libs.algorithm.v1"
        || value["output_schema"] != "libs.algorithm.result.v1"
        || value["availability"] != "available"
        || value["implemented"] != true
        || !value["modes"].as_array().is_some_and(|m| {
            !m.is_empty()
                && m.iter()
                    .all(|v| ["calibration_free", "calibrated"].contains(&v.as_str().unwrap_or("")))
        })
    {
        return Err(Error::new(
            "protocol",
            "algorithm_bad_description",
            "describe must provide model_id/version, supported schemas and calibration modes",
        ));
    }
    Ok(())
}
pub fn validate_result(input: &Value, value: &Value) -> Result<()> {
    let bad = || {
        Error::new(
            "protocol",
            "algorithm_result_invalid",
            "Invalid algorithm result schema, request_id, availability, composition or quality",
        )
    };
    if value["schema_version"] != "libs.algorithm.result.v1"
        || value["request_id"] != input["request_id"]
        || !["available", "unavailable"].contains(&value["availability"].as_str().unwrap_or(""))
        || !value["executed"].is_boolean()
        || !value["status"].is_object()
        || !value["composition"].is_object()
        || !value["quality"].is_object()
        || !value["model"].is_object()
        || !value["composition"]["unit_confirmed"].is_boolean()
        || !value["composition"]["unit"].is_string()
        || !value["quality"]["issues"].is_array()
    {
        return Err(bad());
    }
    if value["availability"] == "available" && value["executed"] != true {
        return Err(bad());
    }
    if value["composition"]["availability"] != value["availability"]
        || value["mode"] != input["parameters"]["mode"]
        || (value["availability"] == "available"
            && (value["composition"]["unit_confirmed"] != true
                || value["composition"]["unit"] != "mass_fraction_percent"))
    {
        return Err(bad());
    }
    for key in ["values", "target_values"] {
        let map = value["composition"][key].as_object().ok_or_else(bad)?;
        if map
            .values()
            .any(|v| !v.is_null() && !v.as_f64().is_some_and(f64::is_finite))
        {
            return Err(bad());
        }
    }
    Ok(())
}

pub fn interface() -> Value {
    json!({"protocol":PROTOCOL,"input_schema":"libs.algorithm.v1","output_schema":"libs.algorithm.result.v1",
        "engines":["native","external"],"modes":["calibration_free","calibrated"],"worker_actions":["describe","analyze"],
        "runtime_file":"runtime.local.json","reload_command":{"command":"prediction","args":{"action":"reload"}},
        "parameter_policy":"algorithm-owned; forwarded without modification","documentation":"backend/ALGORITHM_INTERFACE.md"})
}
