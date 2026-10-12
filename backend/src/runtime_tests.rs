use crate::{
    config::SiteConfig,
    error::Result,
    protocol,
    runtime::Runtime,
    serial::{Laser, Transaction},
    spectrometer::Spectrometer,
};
use parking_lot::Mutex;
use serde_json::{Value, json};
use std::{
    path::PathBuf,
    sync::Arc,
    time::{Duration, Instant},
};

#[derive(Default)]
struct LaserState {
    commands: Vec<(u8, Vec<u8>)>,
    work: u8,
    gate: u8,
    interlock: u8,
}
struct MockLaser {
    state: Arc<Mutex<LaserState>>,
    counter: unsafe extern "C" fn(i32) -> i32,
}
impl Transaction for MockLaser {
    fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        std::thread::sleep(Duration::from_millis(1));
        let mut state = self.state.lock();
        let command = request[4];
        if command == 0xb2 {
            let mut payload = [0; 22];
            payload[0] = state.work;
            payload[3] = state.interlock;
            payload[13..17].copy_from_slice(&0x11223344u32.to_be_bytes());
            payload[19] = state.gate;
            payload[20] = state.work;
            return protocol::laser_frame(command, &payload);
        }
        let bytes = request[5..request.len() - 2].to_vec();
        if command == 0x24 {
            if bytes[0] == 1 {
                assert!(
                    unsafe { (self.counter)(1) } >= 3,
                    "Q opened before all spectrometers were armed"
                );
            }
            state.gate = bytes[0];
        }
        if command == 0x21 {
            state.work = bytes[0];
        }
        assert!(
            ![0x15, 0x17, 0x18].contains(&command),
            "Persistent machine settings were written"
        );
        state.commands.push((command, bytes));
        Ok(vec![])
    }
}
fn wait(runtime: &Arc<Runtime>, ready: impl Fn(&Value) -> bool) -> Value {
    let started = Instant::now();
    loop {
        let status = runtime.handle_line(r#"{"command":"measurement_status"}"#)["payload"].clone();
        if ready(&status) {
            return status;
        }
        assert!(started.elapsed() < Duration::from_secs(3), "{status}");
        std::thread::sleep(Duration::from_millis(5));
    }
}
#[test]
#[ignore = "requires the offline DLL fixture built by backend/build.ps1"]
fn measurement_arming_stop_restart_failure_and_gate_loss() {
    let dll = PathBuf::from(std::env::var_os("LIBS_FAKE_AVASPEC_DLL").unwrap());
    let library = unsafe { libloading::Library::new(&dll).unwrap() };
    let reset = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32)>(b"FakeReset\0")
            .unwrap()
    };
    let counter = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32) -> i32>(b"FakeCounter\0")
            .unwrap()
    };
    for mode in [7, 1, 2, 8, 4] {
        unsafe {
            reset(mode);
        }
        let mut config = SiteConfig::template();
        config.laser.enabled = true;
        config.spectrometer.enabled = true;
        config.spectrometer.serials = vec!["MASTER".into(), "SLAVE1".into(), "SLAVE2".into()];
        config.spectrometer.master_serial = "MASTER".into();
        config.spectrometer.synchronized = true;
        let data = std::env::temp_dir().join(format!("libs-job-{}-{mode}", std::process::id()));
        let mut runtime =
            Runtime::start(config, data.clone(), false, data.join("algorithms")).unwrap();
        let state = Arc::new(Mutex::new(LaserState::default()));
        let mutable = Arc::get_mut(&mut runtime).unwrap();
        mutable.laser = Some(Arc::new(Laser::with_transport(MockLaser {
            state: state.clone(),
            counter,
        })));
        *mutable.spectrometer.lock() = Some(Spectrometer::open(&dll).unwrap());
        mutable.monitor.lock().devices_opened = true;
        mutable.parameters.lock().apply(
            "spectrometer",
            &[("capture_timeout_ms".into(), 1000.)].into(),
        );
        assert_eq!(
            runtime.handle_line(r#"{"command":"gate","args":{"action":"arm"}}"#)["payload"]["allowed"],
            true
        );
        let start = runtime.handle_line(r#"{"command":"measurement_start"}"#);
        if mode == 1 {
            assert_eq!(start["error_code"], "prepare_failed");
            assert!(
                !state
                    .lock()
                    .commands
                    .iter()
                    .any(|(command, value)| *command == 0x24 && value == &[1])
            );
        } else {
            assert_eq!(start["ok"], true, "{start}");
            if mode == 7 {
                wait(&runtime, |s| s["shots_captured"].as_u64().unwrap() > 0);
                let started = Instant::now();
                assert_eq!(
                    runtime.handle_line(r#"{"command":"measurement_stop"}"#)["ok"],
                    true
                );
                assert!(
                    started.elapsed() < Duration::from_millis(150),
                    "Capture timeout blocked stop"
                );
                assert_eq!(
                    runtime.handle_line(r#"{"command":"measurement_start"}"#)["ok"],
                    true
                );
            } else if [2, 8].contains(&mode) {
                let status = wait(&runtime, |s| s["running"] == false);
                assert_eq!(
                    status["shots_failed"],
                    if mode == 2 { json!(3) } else { json!(1) }
                );
                assert_eq!(
                    status["last_error_code"],
                    if mode == 2 {
                        "poll_failed"
                    } else {
                        "spectrometer_disconnected"
                    }
                );
            } else {
                state.lock().interlock = 1;
                let status = wait(&runtime, |s| s["running"] == false);
                assert_eq!(status["last_error_code"], "gate_lost_mid_run");
            }
        }
        runtime.stop();
        assert_eq!(state.lock().gate, 0);
        assert_eq!(state.lock().work, 0);
        assert_eq!(counter_value(counter, 3), 3);
        assert_eq!(counter_value(counter, 4), 1);
        drop(runtime);
        std::fs::remove_dir_all(data).unwrap();
    }
}
fn counter_value(counter: unsafe extern "C" fn(i32) -> i32, index: i32) -> i32 {
    unsafe { counter(index) }
}
