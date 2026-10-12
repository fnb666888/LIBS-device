pub mod algorithm;
pub mod aux_devices;
pub mod config;
pub mod error;
pub mod frames;
pub mod ipc;
pub mod modbus;
pub mod model;
pub mod parameters;
pub mod prediction;
pub mod protocol;
pub mod readiness;
pub mod runtime;
pub mod serial;
pub mod spectrometer;

pub fn wall_clock_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

pub fn monotonic_ms() -> u64 {
    unsafe { windows_sys::Win32::System::SystemInformation::GetTickCount64() }
}
