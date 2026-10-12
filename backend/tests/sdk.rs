use libs_backend::{
    config::SpectrumConfig, parameters::SpectrumMeasurement, spectrometer::Spectrometer,
};
use std::{path::PathBuf, sync::atomic::AtomicBool, time::Instant};

#[test]
#[ignore = "requires the offline DLL fixture built by backend/build.ps1"]
fn vendor_abi_and_repeated_capture_and_cancellation() {
    let dll = std::env::var_os("LIBS_FAKE_AVASPEC_DLL")
        .map(PathBuf::from)
        .expect("Run backend/build.ps1 to build and supply the offline SDK fixture");
    let library = unsafe { libloading::Library::new(&dll).unwrap() };
    let reset = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32)>(b"FakeReset\0")
            .unwrap()
    };
    let set_mode = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32)>(b"FakeSetMode\0")
            .unwrap()
    };
    let counter = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32) -> i32>(b"FakeCounter\0")
            .unwrap()
    };
    let byte = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32, i32) -> u8>(b"FakeConfigByte\0")
            .unwrap()
    };
    let word = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32, i32) -> u32>(b"FakeConfigU16\0")
            .unwrap()
    };
    let uint = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32, i32) -> u32>(b"FakeConfigU32\0")
            .unwrap()
    };
    let sync = unsafe {
        *library
            .get::<unsafe extern "C" fn(i32) -> i32>(b"FakeSyncMode\0")
            .unwrap()
    };
    unsafe {
        reset(5);
    }
    let mut spec = Spectrometer::open(&dll).unwrap();
    let site = SpectrumConfig {
        serials: vec!["MASTER".into(), "SLAVE1".into(), "SLAVE2".into()],
        master_serial: "MASTER".into(),
        synchronized: true,
        ..Default::default()
    };
    let m = SpectrumMeasurement {
        integration_ms: 1.05,
        timeout_ms: 200,
        integration_delay_ns: 1280.,
    };
    for index in 1..=3 {
        let mut callback = || Ok(());
        let (frame, order) = spec
            .capture(
                &site,
                m,
                true,
                index,
                Some(&mut callback),
                &AtomicBool::new(false),
            )
            .unwrap();
        assert_eq!(order, ["SLAVE1", "SLAVE2", "MASTER"]);
        assert_eq!(frame.channels.len(), 3);
        assert!(frame.ok);
        assert!(!frame.hardware_synchronized);
        for c in frame.channels {
            assert_eq!(c.counts.unwrap().len(), c.pixels.unwrap() as usize);
        }
    }
    unsafe {
        assert_eq!(counter(0), 3);
        assert_eq!(counter(1), 9);
        for (channel, pixels) in [4, 7, 9].into_iter().enumerate() {
            let channel = channel as i32;
            assert_eq!(word(channel, 0), 0);
            assert_eq!(word(channel, 2), pixels - 1);
            assert_eq!(uint(channel, 4), 1.05f32.to_bits());
            assert_eq!(uint(channel, 8), 62);
            assert_eq!(uint(channel, 12), 1);
            assert_eq!(byte(channel, 17), 100);
            assert_eq!(byte(channel, 22), 1);
            assert_eq!(byte(channel, 23), u8::from(channel != 0));
            assert_eq!(sync(channel), i32::from(channel == 0));
            for offset in 25..41 {
                assert_eq!(byte(channel, offset), 0);
            }
        }
    }
    let started = Instant::now();
    let error = spec
        .capture(&site, m, true, 4, None, &AtomicBool::new(true))
        .unwrap_err();
    assert_eq!(error.code, "capture_cancelled");
    assert!(started.elapsed().as_millis() < 100);
    assert!(
        spec.capture(&site, m, true, 5, None, &AtomicBool::new(false))
            .is_ok()
    );
    spec.close().unwrap();
    spec.close().unwrap();
    assert_eq!(
        spec.capture(&site, m, true, 6, None, &AtomicBool::new(false))
            .unwrap_err()
            .code,
        "not_open"
    );
    unsafe {
        assert_eq!(counter(3), 3);
        assert_eq!(counter(4), 1);
    }
    drop(spec);
    for (mode, code) in [
        (1, "prepare_failed"),
        (2, "poll_failed"),
        (3, "scope_failed"),
        (4, "capture_timeout"),
        (8, "spectrometer_disconnected"),
    ] {
        unsafe {
            reset(mode);
        }
        let mut spec = Spectrometer::open(&dll).unwrap();
        let error = spec
            .capture(&site, m, true, 1, None, &AtomicBool::new(false))
            .unwrap_err();
        assert_eq!(error.code, code);
        unsafe {
            set_mode(0);
        }
        assert!(
            spec.capture(&site, m, true, 2, None, &AtomicBool::new(false))
                .is_ok()
        );
        spec.close().unwrap();
    }
    unsafe {
        reset(6);
    }
    let spec = Spectrometer::open(&dll).unwrap();
    std::thread::spawn(move || {
        let mut spec = spec;
        spec.close().unwrap();
    })
    .join()
    .unwrap();
    // Moving/dropping the session must release the named owner for the next instance.
    let mut spec = Spectrometer::open(&dll).unwrap();
    spec.capture(&site, m, true, 1, None, &AtomicBool::new(false))
        .unwrap();
    let changed = SpectrumMeasurement {
        integration_ms: 2.,
        ..m
    };
    spec.capture(&site, changed, true, 2, None, &AtomicBool::new(false))
        .unwrap();
    spec.close().unwrap();
}
