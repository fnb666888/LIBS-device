use libs_backend::{algorithm::Engine, frames::FrameBus, prediction::Prediction};
use serde_json::{Value, json};
use std::{
    path::PathBuf,
    sync::{Arc, atomic::AtomicBool},
    time::{Duration, Instant},
};

struct Package(PathBuf);
impl Package {
    fn new() -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path =
            std::env::temp_dir().join(format!("libs-algorithm-{}-{unique}", std::process::id()));
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }
    fn config(&self, version: &str, behavior: &str) {
        let executable = std::env::current_exe()
            .unwrap()
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("examples/algorithm_test_worker.exe");
        assert!(executable.exists(), "cargo test must build examples");
        let config = json!({"engine":"external","executable":executable,"args":[version,behavior],"timeout_ms":1000});
        let temp = self.0.join("runtime.new.json");
        std::fs::write(&temp, config.to_string()).unwrap();
        libs_backend::runtime::replace_file(&temp, &self.0.join("runtime.local.json")).unwrap();
    }
}
impl Drop for Package {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn input(id: &str, mode: &str) -> Value {
    json!({"schema_version":"libs.algorithm.v1","request_id":id,"parameters":{"mode":mode,"calibration":{"factor":2.,"profile_id":"lab-v1"},"custom":42},
        "spectrum":{"spectrum_source":"LIBS","spectrum_format":"wavelength_nm+counts","scan_groups":[{"shot_id":1,"complete":true,"channels":[{"serial":"A","wavelength_nm":[200.,201.,202.],"counts":[1.,2.,3.]}]}]}})
}
fn wait(service: &Prediction, ready: impl Fn(&Value) -> bool) -> Value {
    let started = Instant::now();
    loop {
        let status = service.status();
        if ready(&status) {
            return status;
        }
        assert!(
            started.elapsed() < Duration::from_secs(5),
            "status: {status}"
        );
        std::thread::sleep(Duration::from_millis(10));
    }
}
#[test]
fn interchangeable_modes_parameters_and_protocol_failures() {
    let package = Package::new();
    package.config("v1", "normal");
    let mut engine = Engine::load(&package.0, &|| false).unwrap();
    for (mode, expected) in [("calibration_free", 6.), ("calibrated", 12.)] {
        let request = input("mode", mode);
        let result = engine
            .analyze(&request, &AtomicBool::new(false), &|| false)
            .unwrap();
        assert_eq!(result["composition"]["target_values"]["TFe"], expected);
        assert_eq!(result["parameters_received"], request["parameters"]);
    }
    drop(engine);
    for (behavior, code) in [
        ("bad_id", "algorithm_bad_envelope"),
        ("bad_result", "algorithm_result_invalid"),
        ("crash", "algorithm_exited"),
        ("timeout", "algorithm_timeout"),
    ] {
        package.config("v1", behavior);
        let mut engine = Engine::load(&package.0, &|| false).unwrap();
        let error = engine
            .analyze(
                &input("bad", "calibration_free"),
                &AtomicBool::new(false),
                &|| false,
            )
            .unwrap_err();
        assert_eq!(error.code, code);
    }
}
#[test]
fn reload_replaces_whole_algorithm_and_discards_previous_task() {
    let package = Package::new();
    package.config("v1", "normal");
    let service = Prediction::start(
        Arc::new(FrameBus::default()),
        package.0.join("results"),
        package.0.clone(),
    );
    service.submit(&input("first", "calibration_free")).unwrap();
    let result = wait(&service, |v| v["latest"]["request_id"] == "first");
    assert_eq!(result["latest"]["model"]["version"], "v1");
    package.config("old", "timeout");
    service.reload().unwrap();
    wait(&service, |v| v["model"]["version"] == "old");
    service.submit(&input("stale", "calibration_free")).unwrap();
    wait(&service, |v| v["state"] == "predicting");
    package.config("v2", "normal");
    service.reload().unwrap();
    assert!(service.status()["latest"].is_null());
    wait(&service, |v| v["model"]["version"] == "v2");
    service.submit(&input("new", "calibrated")).unwrap();
    let result = wait(&service, |v| v["latest"]["request_id"] == "new");
    assert_eq!(result["latest"]["model"]["version"], "v2");
    assert_eq!(result["latest"]["composition"]["target_values"]["TFe"], 12.);
    for entry in std::fs::read_dir(package.0.join("results")).unwrap() {
        let record: Value =
            serde_json::from_slice(&std::fs::read(entry.unwrap().path()).unwrap()).unwrap();
        assert_ne!(record["request_id"], "stale");
    }
    service.stop();
}
#[test]
fn blocked_plugin_input_and_output_can_be_cancelled() {
    let package = Package::new();
    for behavior in ["timeout", "block_input"] {
        package.config("v1", behavior);
        let mut engine = Engine::load(&package.0, &|| false).unwrap();
        let mut request = input("cancel", "calibration_free");
        if behavior == "block_input" {
            request["context"] = json!({"large":"x".repeat(4 * 1024 * 1024)});
        }
        let started = Instant::now();
        let error = engine
            .analyze(&request, &AtomicBool::new(false), &|| {
                started.elapsed() > Duration::from_millis(50)
            })
            .unwrap_err();
        assert_eq!(error.code, "algorithm_cancelled");
        assert!(started.elapsed() < Duration::from_millis(500));
    }
}
