use libs_backend::{
    config::SiteConfig,
    frames::{Frame, FrameBus},
    parameters::{Parameters, SpectrumMeasurement},
    prediction, protocol,
    readiness::Monitor,
};
use serde_json::{Value, json};
use std::{path::PathBuf, sync::Arc};

fn temporary(name: &str) -> PathBuf {
    let directory = std::env::temp_dir().join(format!(
        "libs-rust-{name}-{}-{}",
        std::process::id(),
        libs_backend::wall_clock_ms()
    ));
    std::fs::create_dir_all(&directory).unwrap();
    directory
}
#[test]
fn protocol_known_vectors_and_corruption() {
    assert_eq!(protocol::crc16(b"123456789"), 0x4b37);
    assert_eq!(
        protocol::modbus_frame(1, 3, 0, 1),
        [1, 3, 0, 0, 0, 1, 0x84, 0x0a]
    );
    let mut payload = [0; 22];
    payload[13..17].copy_from_slice(&0x12345678u32.to_be_bytes());
    let frame = protocol::laser_frame(0xb2, &payload).unwrap();
    let mut noisy = vec![0x33, 0xaa];
    noisy.extend(protocol::laser_frame(0xb2, &[]).unwrap());
    noisy.extend(&frame);
    assert_eq!(
        protocol::LaserActual::decode(&noisy)
            .unwrap()
            .discharge_counter,
        0x12345678
    );
    noisy.extend(&frame);
    assert_eq!(
        protocol::LaserActual::decode(&noisy).unwrap_err().code,
        "ambiguous_reply"
    );
    let mut bad = frame.clone();
    bad[8] ^= 1;
    assert_eq!(
        protocol::laser_decode(&bad, 0xb2).unwrap_err().code,
        "crc_mismatch"
    );
    assert!(protocol::laser_frame(1, &[0; 252]).is_err());
    assert_eq!(
        protocol::lamp_coil(protocol::lamp_model("CL50S+K50-485").unwrap(), "yellow"),
        3
    );
    assert_eq!(
        protocol::lamp_coil(protocol::lamp_model("TL50-485").unwrap(), "yellow"),
        2
    );
}
#[test]
fn safety_polarity_and_emission_phase() {
    let mut monitor = Monitor {
        armed: true,
        parameters_ready: true,
        laser_enabled: true,
        laser_commissioned: true,
        devices_opened: true,
        spectrometer_ready: true,
        ..Default::default()
    };
    let mut actual = protocol::LaserActual::default();
    monitor.evaluate(Some(&actual), false);
    assert!(monitor.allowed);
    actual.q_gate = 1;
    monitor.evaluate(Some(&actual), false);
    assert_eq!(monitor.blocked_by, ["q_gate_open"]);
    monitor.evaluate(Some(&actual), true);
    assert!(monitor.allowed);
    actual.interlock_1 = 1;
    monitor.evaluate(Some(&actual), true);
    assert_eq!(monitor.blocked_by, ["interlock_open"]);
    monitor.evaluate(None, true);
    assert_eq!(monitor.blocked_by, ["laser_unavailable"]);
    monitor.devices_opened = false;
    monitor.evaluate(Some(&actual), false);
    assert!(!monitor.allowed);
    assert_eq!(monitor.state, "offline");
}
#[test]
fn parameter_persistence_limits_and_override() {
    let directory = temporary("parameters");
    let path = directory.join("parameters.json");
    let mut params = Parameters::open(path.clone());
    params.seed().unwrap();
    assert_eq!(params.readiness()["ready"], true);
    assert_eq!(
        params.apply("laser", &[("discharge_current_a".into(), 72.)].into())["state"],
        "completed"
    );
    assert_eq!(
        params.apply("laser", &[("frequency_hz".into(), 2.5)].into())["state"],
        "rejected"
    );
    let mut loaded = Parameters::open(path);
    loaded.seed().unwrap();
    assert_eq!(loaded.get("laser", "discharge_current_a", 0.), 72.);
    let fire = loaded
        .fire(
            &json!({"laser":{"discharge_current_a":65},"spectrometer":{"integration_delay_ns":0}}),
        )
        .unwrap();
    assert_eq!(fire.laser["discharge_current_a"], 65.);
    assert_eq!(loaded.get("laser", "discharge_current_a", 0.), 72.);
    assert!(fire.overrides[2]);
    assert!(
        loaded
            .fire(&json!({"laser":{"discharge_current_a":101}}))
            .is_err()
    );
    assert!(
        SpectrumMeasurement {
            integration_ms: 100.,
            timeout_ms: 1000,
            integration_delay_ns: 0.
        }
        .validate_rate(10)
        .is_err()
    );
    std::fs::remove_dir_all(directory).unwrap();
}
#[test]
fn slow_subscriber_loses_old_frames_without_affecting_others() {
    let bus = FrameBus::new(2);
    let first = bus.subscribe();
    let second = bus.subscribe();
    for index in 1..=5 {
        let frame = Frame::failed(
            index,
            false,
            100,
            &libs_backend::error::Error::new("timeout", "capture_timeout", "test"),
        );
        bus.publish(Arc::new(frame));
        assert_eq!(bus.pop(first).unwrap().frame_index, index);
    }
    assert_eq!(bus.pending(second), 2);
    assert_eq!(bus.pop(second).unwrap().frame_index, 4);
    assert_eq!(bus.status()["dropped_frames"], 3);
    bus.clear();
    assert_eq!(bus.pending(second), 0);
    bus.unsubscribe(second);
    assert!(bus.pop(second).is_none());
}
#[test]
fn algorithm_rejects_zero_fill_duplicates_and_mismatched_arrays() {
    let input = json!({"schema_version":"libs.algorithm.v1","request_id":"test","spectrum":{"spectrum_source":"LIBS","spectrum_format":"wavelength_nm+counts","scan_groups":[{"complete":false,"channels":[{"serial":"A","wavelength_nm":[200,201,202],"counts":[1,0,3]}]}]}});
    let summary = prediction::validate(&input).unwrap();
    assert_eq!(summary["sample_count"], 3);
    assert_eq!(summary["incomplete_group_indices"], json!([0]));
    let mut bad = input.clone();
    bad["spectrum"]["scan_groups"][0]["channels"][0]["counts"] = json!([1]);
    assert!(prediction::validate(&bad).is_err());
    let c = input["spectrum"]["scan_groups"][0]["channels"][0].clone();
    bad = input.clone();
    bad["spectrum"]["scan_groups"][0]["channels"] = json!([c, c]);
    assert!(prediction::validate(&bad).is_err());
    assert_eq!(
        prediction::contract()["output_template"]["composition"]["values"]["COMPO_P2O5"],
        Value::Null
    );
}
#[test]
fn config_template_and_invalid_wiring() {
    let template = SiteConfig::template();
    template.validate().unwrap();
    let mut value = serde_json::to_value(template).unwrap();
    assert!(SiteConfig::from_value(value.clone()).is_ok());
    value["spectrometer"]["synchronized"] = true.into();
    assert!(SiteConfig::from_value(value.clone()).is_err());
    value["spectrometer"]["synchronized"] = false.into();
    value["laser"]["enabled"] = true.into();
    assert!(SiteConfig::from_value(value).is_err());
}
