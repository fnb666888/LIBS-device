use crate::error::{Error, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct SerialConfig {
    pub port: String,
    pub baud: u32,
    pub parity: String,
    pub data_bits: u32,
    pub timeout_ms: u32,
    pub idle_ms: u32,
    pub enabled: bool,
}
impl Default for SerialConfig {
    fn default() -> Self {
        Self {
            port: String::new(),
            baud: 9600,
            parity: "none".into(),
            data_bits: 8,
            timeout_ms: 650,
            idle_ms: 80,
            enabled: true,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct SpectrumConfig {
    pub sdk_dll: String,
    pub serials: Vec<String>,
    pub master_serial: String,
    pub synchronized: bool,
    pub enabled: bool,
    pub wait_for_laser_trigger: bool,
}
impl Default for SpectrumConfig {
    fn default() -> Self {
        Self {
            sdk_dll: String::new(),
            serials: vec![],
            master_serial: String::new(),
            synchronized: false,
            enabled: true,
            wait_for_laser_trigger: false,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct SiteConfig {
    pub station_id: String,
    pub laser: SerialConfig,
    pub indicator: SerialConfig,
    pub spectrometer: SpectrumConfig,
    pub laser_fire: Value,
    pub indicator_model: String,
    pub indicator_address: u8,
}
impl SiteConfig {
    pub fn zero() -> Self {
        Self {
            indicator_address: 1,
            laser_fire: zero_laser(),
            ..Default::default()
        }
    }
    pub fn template() -> Self {
        let mut config = Self::zero();
        config.station_id = "libs-station-01".into();
        config.laser.enabled = false;
        config.indicator.enabled = false;
        config.spectrometer.enabled = false;
        config.spectrometer.wait_for_laser_trigger = true;
        config
    }
    pub fn from_value(value: Value) -> Result<Self> {
        let bad = |s: String| Error::invalid("bad_site_config", s);
        if !value.is_object() {
            return Err(bad(
                "the top level of a site config must be an object".into()
            ));
        }
        for key in ["station_id", "laser", "indicator", "spectrometer"] {
            if value[key].is_null() {
                return Err(bad(format!("{key} is required")));
            }
        }
        let mut config: Self =
            serde_json::from_value(value.clone()).map_err(|e| bad(e.to_string()))?;
        if !value.as_object().unwrap().contains_key("indicator_address") {
            config.indicator_address = 1;
        }
        let mut fire = zero_laser();
        if let Some(fields) = value["laser_fire"].as_object() {
            for (key, v) in fields {
                if fire.get(key).is_some() {
                    fire[key] = v.clone();
                }
            }
        } else if !value["laser_fire"].is_null() {
            return Err(bad("laser_fire must be an object".into()));
        }
        config.laser_fire = fire;
        config.validate()?;
        Ok(config)
    }
    pub fn load(path: &Path) -> Result<Self> {
        let bytes = std::fs::read(path).map_err(|_| {
            Error::invalid(
                "bad_site_config",
                format!("cannot open the site config file: {}", path.display()),
            )
        })?;
        let value = serde_json::from_slice(&bytes).map_err(|e| {
            Error::invalid(
                "bad_site_config",
                format!("invalid JSON in {}: {e}", path.display()),
            )
        })?;
        Self::from_value(value)
    }
    pub fn validate(&self) -> Result<()> {
        let bad = |s: String| Error::invalid("bad_site_config", s);
        if self.station_id.is_empty() {
            return Err(bad("station_id must not be empty".into()));
        }
        for (name, cfg) in [("laser", &self.laser), ("indicator", &self.indicator)] {
            if !["none", "even", "odd"].contains(&cfg.parity.as_str()) {
                return Err(bad(format!("{name}.parity must be one of none/even/odd")));
            }
            if cfg.enabled {
                if cfg.port.is_empty() {
                    return Err(bad(format!("{name} is enabled but has no port")));
                }
                if !cfg.port.starts_with("COM") && !cfg.port.starts_with(r"\\.\") {
                    return Err(bad(format!("{name}.port must look like COM3")));
                }
                if cfg.baud == 0 {
                    return Err(bad(format!("{name}.baud must be positive")));
                }
                if ![7, 8].contains(&cfg.data_bits) {
                    return Err(bad(format!("{name}.data_bits must be 7 or 8")));
                }
            }
        }
        let spec = &self.spectrometer;
        if spec.enabled && spec.sdk_dll.is_empty() {
            return Err(bad("spectrometer is enabled but sdk_dll is empty".into()));
        }
        if spec.enabled && !Path::new(&spec.sdk_dll).is_absolute() {
            return Err(bad("spectrometer.sdk_dll must be an absolute path".into()));
        }
        if spec.serials.iter().any(String::is_empty) {
            return Err(bad(
                "spectrometer.serials entries must be non-empty strings".into(),
            ));
        }
        if spec.synchronized && spec.serials.len() < 2 {
            return Err(bad(
                "spectrometer.synchronized needs at least two serials".into()
            ));
        }
        if !spec.master_serial.is_empty() && spec.serials.is_empty() {
            return Err(bad(
                "spectrometer.master_serial requires an explicit serials list".into(),
            ));
        }
        if self.indicator_address == 0 {
            return Err(bad("indicator_address must be 1..255".into()));
        }
        if self.laser_fire["commissioned"].as_bool() == Some(true) {
            for field in crate::parameters::LASER_FIELDS {
                if !crate::parameters::accepts(
                    "laser",
                    field,
                    self.laser_fire[field].as_f64().unwrap_or(f64::NAN),
                ) {
                    return Err(bad(format!("laser_fire.{field} exceeds allowed range")));
                }
            }
        }
        Ok(())
    }
}

fn zero_laser() -> Value {
    let mut value = serde_json::json!({"commissioned": false});
    for field in crate::parameters::LASER_FIELDS {
        value[field] = 0.into();
    }
    value["settle_timeout_ms"] = 20000.into();
    value["emission_hold_ms"] = 3000.into();
    value["min_discharges"] = 1.into();
    value
}
