use crate::{
    config::SpectrumConfig,
    error::{Error, Result},
    frames::{Channel, Frame},
    parameters::{self, SpectrumMeasurement},
};
use libloading::Library;
use std::{
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, WAIT_ABANDONED, WAIT_OBJECT_0},
    System::Threading::{CreateMutexW, ReleaseMutex, WaitForSingleObject},
};

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct Identity {
    serial: [u8; 10],
    name: [u8; 64],
    status: u8,
}
#[repr(C, packed)]
#[derive(Clone, Copy, PartialEq)]
struct Measure([u8; 41]);
const _: () =
    assert!(std::mem::size_of::<Identity>() == 75 && std::mem::size_of::<Measure>() == 41);

struct Owner {
    release: mpsc::SyncSender<()>,
    thread: Option<std::thread::JoinHandle<()>>,
}
impl Drop for Owner {
    fn drop(&mut self) {
        let _ = self.release.send(());
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
impl Owner {
    fn acquire() -> Result<Self> {
        let (release, receiver) = mpsc::sync_channel(1);
        let (started, ready) = mpsc::sync_channel(1);
        // Win32 mutex ownership is thread-bound; keep its acquire/release on one thread.
        let thread = std::thread::Builder::new()
            .name("avaspec-owner".into())
            .spawn(move || {
                let name: Vec<u16> = "Local\\LibsAvaSpecSdkOwner\0".encode_utf16().collect();
                let handle = unsafe { CreateMutexW(std::ptr::null(), 0, name.as_ptr()) };
                if handle.is_null() {
                    let _ = started.send(Err(Error::new(
                        "transport",
                        "sdk_load_failed",
                        "cannot create the AvaSpec owner mutex",
                    )));
                    return;
                }
                let wait = unsafe { WaitForSingleObject(handle, 0) };
                if wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED {
                    let _ = started.send(Ok(()));
                    let _ = receiver.recv();
                    unsafe {
                        ReleaseMutex(handle);
                    }
                } else {
                    let _ = started.send(Err(Error::new(
                        "transport",
                        "sdk_load_failed",
                        "another local AvaSpec operation is already running",
                    )));
                }
                unsafe {
                    CloseHandle(handle);
                }
            })
            .map_err(|e| Error::new("transport", "sdk_load_failed", e.to_string()))?;
        let owner = Self {
            release,
            thread: Some(thread),
        };
        ready.recv().map_err(|_| {
            Error::new(
                "transport",
                "sdk_load_failed",
                "AvaSpec owner thread stopped",
            )
        })??;
        Ok(owner)
    }
}
struct Api {
    _library: Library,
    init: unsafe extern "system" fn(i16) -> i32,
    done: unsafe extern "system" fn() -> i32,
    list: unsafe extern "system" fn(u32, *mut u32, *mut Identity) -> i32,
    activate: unsafe extern "system" fn(*mut Identity) -> i32,
    deactivate: unsafe extern "system" fn(i32) -> bool,
    pixels: unsafe extern "system" fn(i32, *mut u16) -> i32,
    wavelength: unsafe extern "system" fn(i32, *mut f64) -> i32,
    sync: unsafe extern "system" fn(i32, u8) -> i32,
    prepare: unsafe extern "system" fn(i32, *mut Measure) -> i32,
    measure: unsafe extern "system" fn(i32, *mut std::ffi::c_void, i16) -> i32,
    poll: unsafe extern "system" fn(i32) -> i32,
    scope: unsafe extern "system" fn(i32, *mut u32, *mut f64) -> i32,
    stop: unsafe extern "system" fn(i32) -> i32,
}
impl Api {
    fn load(path: &Path) -> Result<Self> {
        if !path.is_absolute() {
            return Err(Error::new(
                "transport",
                "sdk_load_failed",
                "sdk_dll must be an absolute path",
            ));
        }
        let library: Library =
            unsafe { libloading::os::windows::Library::load_with_flags(path, 0x100 | 0x1000) }
                .map_err(|e| Error::new("transport", "sdk_load_failed", e.to_string()))?
                .into();
        macro_rules! bind {
            ($name:literal) => {
                unsafe {
                    *library
                        .get(concat!($name, "\0").as_bytes())
                        .map_err(|e| Error::new("transport", "sdk_load_failed", e.to_string()))?
                }
            };
        }
        // Resolve the same export set as the original adapter, including probe-only exports.
        unsafe {
            let _ = library
                .get::<unsafe extern "system" fn(i32, *mut u8, *mut u8, *mut u8) -> i32>(
                    b"AVS_GetVersionInfo\0",
                )
                .map_err(|e| Error::new("transport", "sdk_load_failed", e.to_string()))?;
            let _ = library
                .get::<unsafe extern "system" fn(i32, *mut i32) -> i32>(b"AVS_GetDeviceType\0")
                .map_err(|e| Error::new("transport", "sdk_load_failed", e.to_string()))?;
        }
        Ok(Self {
            init: bind!("AVS_Init"),
            done: bind!("AVS_Done"),
            list: bind!("AVS_GetList"),
            activate: bind!("AVS_Activate"),
            deactivate: bind!("AVS_Deactivate"),
            pixels: bind!("AVS_GetNumPixels"),
            wavelength: bind!("AVS_GetLambda"),
            sync: bind!("AVS_SetSyncMode"),
            prepare: bind!("AVS_PrepareMeasure"),
            measure: bind!("AVS_Measure"),
            poll: bind!("AVS_PollScan"),
            scope: bind!("AVS_GetScopeData"),
            stop: bind!("AVS_StopMeasure"),
            _library: library,
        })
    }
}
struct Device {
    handle: i32,
    serial: String,
    pixels: u16,
    wavelengths: Arc<Vec<f64>>,
    config: Measure,
    prepared: Option<(Measure, u8)>,
    touched: bool,
}
pub struct Spectrometer {
    api: Api,
    devices: Vec<Device>,
    initialized: bool,
    closed: bool,
    _owner: Owner,
    pub model: String,
}
impl Spectrometer {
    pub fn open(path: &Path) -> Result<Self> {
        let owner = Owner::acquire()?;
        let api = Api::load(path)?;
        let mut session = Self {
            api,
            devices: vec![],
            initialized: false,
            closed: false,
            _owner: owner,
            model: String::new(),
        };
        let identities = session
            .identities()
            .map_err(|e| Error::new("transport", "enumerate_failed", e.message))?;
        for identity in identities {
            if session.model.is_empty() {
                session.model = bounded(&identity.name);
            }
            if identity.status == 1 {
                session.activate(identity)?;
            }
        }
        Ok(session)
    }
    fn identities(&mut self) -> Result<Vec<Identity>> {
        if !self.initialized {
            let rc = unsafe { (self.api.init)(0) };
            if rc < 0 {
                return Err(Error::new(
                    "transport",
                    "enumerate_failed",
                    format!("AVS_Init failed: SDK return {rc}"),
                ));
            }
            self.initialized = true;
        }
        let mut required = 0;
        let rc = unsafe { (self.api.list)(0, &mut required, std::ptr::null_mut()) };
        if rc < 0 && rc != -9 {
            return Err(Error::new(
                "transport",
                "enumerate_failed",
                format!("AVS_GetList size query failed: {rc}"),
            ));
        }
        for _ in 0..3 {
            if required == 0 {
                return Ok(vec![]);
            }
            if required % 75 != 0 || required > 75 * 256 {
                return Err(Error::new(
                    "transport",
                    "enumerate_failed",
                    "AVS_GetList returned an invalid required size",
                ));
            }
            let capacity = required;
            let mut ids = vec![
                Identity {
                    serial: [0; 10],
                    name: [0; 64],
                    status: 0
                };
                required as usize / 75
            ];
            let count = unsafe { (self.api.list)(capacity, &mut required, ids.as_mut_ptr()) };
            if required > capacity {
                continue;
            }
            if count < 0 || count as usize > ids.len() {
                return Err(Error::new(
                    "transport",
                    "enumerate_failed",
                    format!("AVS_GetList failed or returned an inconsistent count: {count}"),
                ));
            }
            ids.truncate(count as usize);
            return Ok(ids);
        }
        Err(Error::new(
            "transport",
            "enumerate_failed",
            "the device list kept changing during enumeration",
        ))
    }
    fn activate(&mut self, mut identity: Identity) -> Result<usize> {
        let serial = bounded(&identity.serial);
        if let Some(index) = self.devices.iter().position(|d| d.serial == serial) {
            return Ok(index);
        }
        let handle = unsafe { (self.api.activate)(&mut identity) };
        if handle < 0 || handle == 1000 {
            return Err(Error::new(
                "transport",
                "capture_failed",
                format!("AVS_Activate failed for {serial}"),
            ));
        }
        let result = (|| {
            let mut pixels = 0;
            let rc = unsafe { (self.api.pixels)(handle, &mut pixels) };
            sdk_check(rc, "AVS_GetNumPixels", &serial, "capture_failed")?;
            if pixels == 0 || pixels > 16384 {
                return Err(Error::new(
                    "transport",
                    "capture_failed",
                    format!("invalid pixel count: {serial}"),
                ));
            }
            let mut waves = vec![0.; pixels as usize];
            sdk_check(
                unsafe { (self.api.wavelength)(handle, waves.as_mut_ptr()) },
                "AVS_GetLambda",
                &serial,
                "capture_failed",
            )?;
            if waves.iter().any(|v| !v.is_finite()) {
                return Err(Error::new(
                    "transport",
                    "capture_failed",
                    format!("non-finite wavelength calibration: {serial}"),
                ));
            }
            Ok(Device {
                handle,
                serial: serial.clone(),
                pixels,
                wavelengths: Arc::new(waves),
                config: Measure([0; 41]),
                prepared: None,
                touched: false,
            })
        })();
        match result {
            Ok(device) => {
                self.devices.push(device);
                Ok(self.devices.len() - 1)
            }
            Err(e) => {
                unsafe {
                    (self.api.deactivate)(handle);
                }
                Err(e)
            }
        }
    }
    pub fn capture(
        &mut self,
        site: &SpectrumConfig,
        m: SpectrumMeasurement,
        wait: bool,
        index: u64,
        on_armed: Option<&mut dyn FnMut() -> Result<()>>,
        cancelled: &AtomicBool,
    ) -> Result<(Frame, Vec<String>)> {
        if self.closed {
            return Err(Error::new(
                "internal",
                "not_open",
                "the session is not open",
            ));
        }
        for (field, value, code, message) in [
            (
                "integration_ms",
                m.integration_ms,
                "bad_integration",
                "integration_ms must be finite and in [0.002, 60000]",
            ),
            (
                "capture_timeout_ms",
                m.timeout_ms as f64,
                "bad_timeout",
                "timeout_ms must be in 1..600000 ms",
            ),
            (
                "integration_delay_ns",
                m.integration_delay_ns,
                "bad_integration_delay",
                "integration_delay_ns must be finite and in 0..1000000 ns",
            ),
        ] {
            if !parameters::accepts("spectrometer", field, value) {
                return Err(Error::invalid(code, message));
            }
        }
        if !site.master_serial.is_empty() && !site.serials.contains(&site.master_serial) {
            return Err(Error::invalid(
                "master_not_listed",
                "master_serial must appear in the explicit serials list",
            ));
        }
        if m.integration_delay_ns > 0. && !wait {
            return Err(Error::invalid(
                "delay_without_trigger",
                "integration_delay_ns is set but wait_for_laser_trigger is false; a delay only makes sense when an external laser trigger starts the integration",
            ));
        }
        let result = self.capture_inner(site, m, wait, index, on_armed, cancelled);
        if result.is_err() {
            for device in &mut self.devices {
                if device.touched {
                    unsafe {
                        (self.api.stop)(device.handle);
                    }
                }
                device.prepared = None;
            }
        }
        result
    }
    fn capture_inner(
        &mut self,
        site: &SpectrumConfig,
        m: SpectrumMeasurement,
        wait: bool,
        index: u64,
        mut on_armed: Option<&mut dyn FnMut() -> Result<()>>,
        cancelled: &AtomicBool,
    ) -> Result<(Frame, Vec<String>)> {
        let identities = self.identities()?;
        let mut active = vec![];
        if site.serials.is_empty() {
            for identity in identities {
                if identity.status == 1 {
                    active.push(self.activate(identity)?);
                }
            }
        } else {
            for serial in &site.serials {
                let identity = identities
                    .iter()
                    .find(|id| bounded(&id.serial) == *serial)
                    .ok_or_else(|| {
                        Error::new(
                            "device",
                            "serial_not_found",
                            format!("no attached spectrometer has serial '{serial}'"),
                        )
                    })?;
                if identity.status != 1 && !self.devices.iter().any(|d| d.serial == *serial) {
                    return Err(Error::new(
                        "device",
                        "serial_not_available",
                        format!("spectrometer '{serial}' is present but not available"),
                    ));
                }
                active.push(self.activate(*identity)?);
            }
        }
        if active.is_empty() {
            return Err(Error::new(
                "device",
                "no_devices",
                "no available spectrometer was found",
            ));
        }
        if active
            .iter()
            .map(|i| self.devices[*i].pixels as u64)
            .sum::<u64>()
            > 4_000_000
        {
            return Err(Error::invalid(
                "too_many_samples",
                "the requested capture exceeds the four-million-sample in-memory limit; split it into smaller runs",
            ));
        }
        let mut order = active.clone();
        order.sort_by_key(|i| self.devices[*i].serial == site.master_serial);
        for &i in &order {
            let d = &mut self.devices[i];
            let master = d.serial == site.master_serial;
            let mut config = Measure([0; 41]);
            config.0[2..4].copy_from_slice(&(d.pixels - 1).to_le_bytes());
            config.0[4..8].copy_from_slice(&(m.integration_ms as f32).to_le_bytes());
            let cycles = (6.0 * (m.integration_delay_ns + 20.84) / 125.0) as u32;
            config.0[8..12].copy_from_slice(&cycles.to_le_bytes());
            config.0[12..16].copy_from_slice(&1u32.to_le_bytes());
            config.0[17] = 100;
            config.0[22] = u8::from(wait);
            if site.synchronized && !master {
                config.0[22] = 1;
                config.0[23] = 1;
            }
            let sync = u8::from(site.synchronized && master);
            d.config = config;
            d.touched = true;
            if d.prepared == Some((config, sync)) {
                continue;
            }
            d.prepared = None;
            unsafe {
                (self.api.stop)(d.handle);
            }
            sdk_check(
                unsafe { (self.api.sync)(d.handle, sync) },
                "AVS_SetSyncMode",
                &d.serial,
                "set_sync_failed",
            )?;
            sdk_check(
                unsafe { (self.api.prepare)(d.handle, &mut config) },
                "AVS_PrepareMeasure",
                &d.serial,
                "prepare_failed",
            )?;
            d.prepared = Some((config, sync));
        }
        let started = Instant::now();
        let mut arm_order = vec![];
        for &i in &order {
            let d = &self.devices[i];
            if started.elapsed().as_millis() >= m.timeout_ms as u128 {
                return Err(Error::new(
                    "timeout",
                    "arm_deadline",
                    "the deadline passed before every channel was armed",
                ));
            }
            sdk_check(
                unsafe { (self.api.measure)(d.handle, std::ptr::null_mut(), 1) },
                "AVS_Measure",
                &d.serial,
                "measure_failed",
            )?;
            arm_order.push(d.serial.clone());
        }
        if let Some(callback) = on_armed.as_mut() {
            callback()?;
        }
        let mut channels = vec![];
        let mut ready = vec![false; active.len()];
        while channels.len() < active.len() {
            if cancelled.load(Ordering::Acquire) {
                return Err(Error::new(
                    "device",
                    "capture_cancelled",
                    "capture stopped by owner",
                ));
            }
            if started.elapsed().as_millis() >= m.timeout_ms as u128 {
                let missing: Vec<_> = active
                    .iter()
                    .enumerate()
                    .filter(|(k, _)| !ready[*k])
                    .map(|(_, i)| self.devices[*i].serial.as_str())
                    .collect();
                return Err(Error::new(
                    "timeout",
                    "capture_timeout",
                    format!(
                        "timed out waiting for {}; no complete group received",
                        missing.join(", ")
                    ),
                ));
            }
            for (k, &i) in active.iter().enumerate() {
                if ready[k] {
                    continue;
                }
                let d = &self.devices[i];
                let rc = unsafe { (self.api.poll)(d.handle) };
                if rc == 0 {
                    continue;
                }
                if rc < 0 {
                    sdk_check(rc, "AVS_PollScan", &d.serial, "poll_failed")?;
                }
                if rc != 1 {
                    return Err(Error::new(
                        "device",
                        "poll_unexpected",
                        format!("AVS_PollScan {} returned {rc}", d.serial),
                    ));
                }
                let mut counts = vec![0.; d.pixels as usize];
                let mut ticks = 0;
                sdk_check(
                    unsafe { (self.api.scope)(d.handle, &mut ticks, counts.as_mut_ptr()) },
                    "AVS_GetScopeData",
                    &d.serial,
                    "scope_failed",
                )?;
                if counts.iter().any(|v| !v.is_finite()) {
                    return Err(Error::new(
                        "device",
                        "non_finite_spectrum",
                        format!("the spectrum from {} contains non-finite values", d.serial),
                    ));
                }
                channels.push(Channel {
                    serial: d.serial.clone(),
                    state: "present".into(),
                    reason: None,
                    pixels: Some(d.pixels),
                    wavelength_nm: Some(d.wavelengths.clone()),
                    counts: Some(counts),
                    device_time_ticks: ticks,
                });
                ready[k] = true;
            }
            if channels.len() < active.len() {
                std::thread::sleep(Duration::from_millis(2));
            }
        }
        Ok((
            Frame {
                frame_index: index,
                shot_id: 1,
                wall_clock_ms: crate::wall_clock_ms(),
                captured_monotonic_ms: crate::monotonic_ms(),
                hardware_synchronized: false,
                requested_sync: site.synchronized,
                laser_shot: wait,
                expected_groups: 1,
                received_groups: 1,
                period_ms: 0.,
                timeout_ms: m.timeout_ms,
                ok: true,
                capture_error_code: None,
                capture_error_message: None,
                channels,
            },
            arm_order,
        ))
    }
    pub fn close(&mut self) -> Result<()> {
        if self.closed {
            return Ok(());
        }
        self.closed = true;
        let mut errors = vec![];
        for d in &mut self.devices {
            if d.touched {
                let rc = unsafe { (self.api.stop)(d.handle) };
                if rc != 0 {
                    errors.push(format!("AVS_StopMeasure {} -> {rc}", d.serial));
                }
                let mut quiet = d.config;
                quiet.0[22..41].fill(0);
                let rc = unsafe { (self.api.prepare)(d.handle, &mut quiet) };
                if rc != 0 {
                    errors.push(format!("AVS_PrepareMeasure quiet {} -> {rc}", d.serial));
                }
                let rc = unsafe { (self.api.sync)(d.handle, 0) };
                if rc != 0 {
                    errors.push(format!("AVS_SetSyncMode off {} -> {rc}", d.serial));
                }
            }
            if !unsafe { (self.api.deactivate)(d.handle) } {
                errors.push(format!("AVS_Deactivate {} failed", d.serial));
            }
        }
        if self.initialized {
            let rc = unsafe { (self.api.done)() };
            if rc != 0 {
                errors.push(format!("AVS_Done -> {rc}"));
            }
            self.initialized = false;
        }
        if errors.is_empty() {
            Ok(())
        } else {
            Err(Error::new(
                "internal",
                "cleanup_incomplete",
                errors.join("; "),
            ))
        }
    }
}
impl Drop for Spectrometer {
    fn drop(&mut self) {
        let _ = self.close();
    }
}
fn bounded(bytes: &[u8]) -> String {
    String::from_utf8_lossy(&bytes[..bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len())])
        .into_owned()
}
fn sdk_check(rc: i32, operation: &str, serial: &str, code: &'static str) -> Result<()> {
    if rc == 0 {
        Ok(())
    } else {
        Err(Error::new(
            "device",
            if rc == -3 {
                "spectrometer_disconnected"
            } else {
                code
            },
            format!("{operation} {serial} failed: {rc}"),
        ))
    }
}
