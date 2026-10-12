use crate::error::{Error, Result};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{collections::BTreeMap, path::PathBuf};

pub const LASER_FIELDS: [&str; 9] = [
    "discharge_current_a",
    "pulse_width_us",
    "frequency_hz",
    "discharge_delay_us",
    "q_delay_us",
    "q_warmup_s",
    "settle_timeout_ms",
    "emission_hold_ms",
    "min_discharges",
];
pub const SPECTRUM_FIELDS: [&str; 3] = [
    "integration_ms",
    "capture_timeout_ms",
    "integration_delay_ns",
];
pub fn fields(group: &str) -> &[&str] {
    match group {
        "laser" => &LASER_FIELDS,
        "spectrometer" => &SPECTRUM_FIELDS,
        _ => &[],
    }
}
pub fn range(group: &str, field: &str) -> Option<(f64, f64, bool)> {
    Some(match (group, field) {
        ("laser", "discharge_current_a") => (15., 100., true),
        ("laser", "pulse_width_us") => (50., 300., true),
        ("laser", "frequency_hz") => (1., 10., true),
        ("laser", "discharge_delay_us") => (0., 500., true),
        ("laser", "q_delay_us") => (2., 500., true),
        ("laser", "q_warmup_s") => (0., 3600., true),
        ("laser", "settle_timeout_ms") | ("spectrometer", "capture_timeout_ms") => {
            (1., 600000., true)
        }
        ("laser", "emission_hold_ms") => (0., 600000., true),
        ("laser", "min_discharges") => (1., 100000., true),
        ("spectrometer", "integration_ms") => (0.002, 60000., false),
        ("spectrometer", "integration_delay_ns") => (0., 1000000., false),
        _ => return None,
    })
}
pub fn accepts(group: &str, field: &str, value: f64) -> bool {
    range(group, field).is_some_and(|(lo, hi, integer)| {
        value >= lo && value <= hi && (!integer || value.fract() == 0.)
    })
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Group {
    pub values: BTreeMap<String, f64>,
    #[serde(default)]
    pub complete: bool,
}
pub struct Parameters {
    pub path: PathBuf,
    pub groups: BTreeMap<String, Group>,
}
impl Parameters {
    pub fn open(path: PathBuf) -> Self {
        let groups = std::fs::read(&path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
            .and_then(|v| serde_json::from_value(v["groups"].clone()).ok())
            .unwrap_or_default();
        let mut store = Self { path, groups };
        for (name, group) in &mut store.groups {
            group.complete = complete(name, &group.values);
        }
        store
    }
    pub fn seed(&mut self) -> Result<()> {
        for (group, values) in [
            ("spectrometer", vec![1.05, 10000., 1280.]),
            (
                "laser",
                vec![80., 200., 1., 0., 210., 0., 20000., 3000., 1.],
            ),
        ] {
            let g = self.groups.entry(group.into()).or_default();
            for (field, value) in fields(group).iter().zip(values) {
                g.values.entry((*field).into()).or_insert(value);
            }
            g.complete = complete(group, &g.values);
        }
        self.persist()
    }
    pub fn persist(&self) -> Result<()> {
        let tmp = self.path.with_extension("json.tmp");
        let text = serde_json::to_vec_pretty(&json!({"version":1,"groups":self.groups})).unwrap();
        std::fs::write(&tmp, text).map_err(|_| {
            Error::new(
                "internal",
                "persist_open_failed",
                format!("无法写入临时文件：{}", tmp.display()),
            )
        })?;
        crate::runtime::replace_file(&tmp, &self.path).map_err(|_| {
            Error::new(
                "internal",
                "persist_rename_failed",
                format!("无法把临时文件替换为参数文件：{}", self.path.display()),
            )
        })
    }
    pub fn apply(&mut self, group: &str, values: &BTreeMap<String, f64>) -> Value {
        let mut result = json!({"group":group,"state":"rejected","message":"","failed_fields":[]});
        if group.is_empty() {
            result["message"] = "参数分组名不能为空".into();
            return result;
        }
        if values.is_empty() {
            result["message"] = "没有任何字段被设置".into();
            return result;
        }
        for (field, value) in values {
            let msg = if field.is_empty() {
                "字段名不能为空".into()
            } else if !value.is_finite() {
                format!("字段 {field} 的值不是有限数（NaN/Inf 不允许作为参数）")
            } else if range(group, field).is_some() && !accepts(group, field, *value) {
                format!("参数 {group}.{field} 超出允许范围或不是要求的整数")
            } else {
                continue;
            };
            result["message"] = msg.into();
            result["failed_fields"] = json!([if field.is_empty() { "<empty>" } else { field }]);
            return result;
        }
        let target = self.groups.entry(group.into()).or_default();
        target.values.extend(values.clone());
        target.complete = complete(group, &target.values);
        match self.persist() {
            Ok(()) => {
                result["state"] = "completed".into();
                result["message"] = "参数设置已完成".into();
            }
            Err(e) => {
                result["state"] = "failed".into();
                result["message"] =
                    format!("参数已改但落盘失败，不能应答已完成：{}", e.message).into();
            }
        }
        result
    }
    pub fn readiness(&self) -> Value {
        let mut groups = vec![];
        let mut missing = vec![];
        for name in ["spectrometer", "laser"] {
            let start = missing.len();
            for field in fields(name) {
                if !self
                    .groups
                    .get(name)
                    .and_then(|g| g.values.get(*field))
                    .is_some_and(|v| accepts(name, field, *v))
                {
                    missing.push(format!("{name}.{field}"));
                }
            }
            if missing.len() != start {
                groups.push(name);
            }
        }
        let ready = missing.is_empty();
        let summary = if ready {
            "核心参数齐备".into()
        } else {
            format!(
                "核心参数缺失或越界：涉及 {} 个分组、{} 个字段",
                groups.len(),
                missing.len()
            )
        };
        json!({"ready":ready,"missing_groups":groups,"missing_fields":missing,"summary":summary})
    }
    pub fn get(&self, group: &str, field: &str, fallback: f64) -> f64 {
        self.groups
            .get(group)
            .and_then(|g| g.values.get(field))
            .copied()
            .unwrap_or(fallback)
    }
    pub fn fire(&self, args: &Value) -> Result<FireParameters> {
        let mut laser = BTreeMap::new();
        for field in LASER_FIELDS {
            let value = self.get("laser", field, f64::NAN);
            if !accepts("laser", field, value) {
                return Err(Error::invalid(
                    "parameter_field_bad",
                    format!("laser.{field} 缺失或不是非负整数"),
                ));
            }
            laser.insert(field.to_string(), value);
        }
        if args.get("laser").is_some() {
            let obj = args["laser"]
                .as_object()
                .ok_or_else(|| Error::invalid("laser_overrides_bad", "args.laser 必须是对象"))?;
            for field in LASER_FIELDS {
                if let Some(v) = obj.get(field) {
                    let v = v
                        .as_f64()
                        .filter(|v| accepts("laser", field, *v))
                        .ok_or_else(|| {
                            Error::invalid(
                                "laser_overrides_bad",
                                "args.laser 的字段必须是 0 或正整数",
                            )
                        })?;
                    laser.insert(field.into(), v);
                }
            }
        }
        let mut spectrum = self.spectrum();
        let mut overrides = [false; 3];
        if args.get("spectrometer").is_some() {
            let obj = args["spectrometer"].as_object().ok_or_else(|| {
                Error::invalid("spectrometer_overrides_bad", "args.spectrometer 必须是对象")
            })?;
            let mut values = [
                spectrum.integration_ms,
                spectrum.timeout_ms as f64,
                spectrum.integration_delay_ns,
            ];
            for (i, field) in SPECTRUM_FIELDS.iter().enumerate() {
                if let Some(v) = obj.get(*field) {
                    values[i] = v
                        .as_f64()
                        .filter(|v| accepts("spectrometer", field, *v))
                        .ok_or_else(|| {
                            Error::invalid(
                                "spectrometer_overrides_bad",
                                format!("spectrometer.{field} 超出允许范围或不是数字"),
                            )
                        })?;
                    overrides[i] = true;
                }
            }
            spectrum = SpectrumMeasurement {
                integration_ms: values[0],
                timeout_ms: values[1] as u32,
                integration_delay_ns: values[2],
            };
        }
        Ok(FireParameters {
            laser,
            spectrum,
            overrides,
        })
    }
    pub fn spectrum(&self) -> SpectrumMeasurement {
        SpectrumMeasurement {
            integration_ms: self.get("spectrometer", "integration_ms", 1.05),
            timeout_ms: self.get("spectrometer", "capture_timeout_ms", 10000.) as u32,
            integration_delay_ns: self.get("spectrometer", "integration_delay_ns", 1280.),
        }
    }
}
fn complete(group: &str, values: &BTreeMap<String, f64>) -> bool {
    fields(group).iter().all(|field| {
        values
            .get(*field)
            .is_some_and(|v| accepts(group, field, *v))
    })
}
#[derive(Debug, Clone, Copy)]
pub struct SpectrumMeasurement {
    pub integration_ms: f64,
    pub timeout_ms: u32,
    pub integration_delay_ns: f64,
}
impl SpectrumMeasurement {
    pub fn validate_rate(&self, hz: u32) -> Result<()> {
        if !accepts("laser", "frequency_hz", hz as f64)
            || !accepts("spectrometer", "integration_ms", self.integration_ms)
            || !accepts(
                "spectrometer",
                "integration_delay_ns",
                self.integration_delay_ns,
            )
            || !accepts("spectrometer", "capture_timeout_ms", self.timeout_ms as f64)
        {
            return Err(Error::invalid("capture_parameters_invalid", "采集参数无效"));
        }
        if self.integration_ms + self.integration_delay_ns / 1e6 >= 1000. / hz as f64 {
            return Err(Error::invalid(
                "laser_spectrometer_rate_mismatch",
                "积分延时与积分时间超过激光周期",
            ));
        }
        Ok(())
    }
}
#[derive(Debug, Clone)]
pub struct FireParameters {
    pub laser: BTreeMap<String, f64>,
    pub spectrum: SpectrumMeasurement,
    pub overrides: [bool; 3],
}
