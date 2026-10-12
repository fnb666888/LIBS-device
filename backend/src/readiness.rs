use crate::protocol::LaserActual;
use serde_json::{Value, json};

#[derive(Debug, Clone)]
pub struct Monitor {
    pub armed: bool,
    pub parameters_ready: bool,
    pub laser_enabled: bool,
    pub laser_commissioned: bool,
    pub devices_opened: bool,
    pub spectrometer_ready: bool,
    pub allowed: bool,
    pub blocked_by: Vec<&'static str>,
    pub detail: String,
    pub state: &'static str,
    pub laser_readable: bool,
}
impl Default for Monitor {
    fn default() -> Self {
        Self {
            armed: false,
            parameters_ready: false,
            laser_enabled: false,
            laser_commissioned: false,
            devices_opened: false,
            spectrometer_ready: false,
            allowed: false,
            blocked_by: vec![],
            detail: String::new(),
            state: "offline",
            laser_readable: false,
        }
    }
}
impl Monitor {
    pub fn evaluate(&mut self, actual: Option<&LaserActual>, emitting: bool) {
        let mut blocked = vec![];
        if !self.devices_opened || !self.laser_enabled || !self.spectrometer_ready {
            blocked.push("not_enabled");
        }
        if !self.parameters_ready {
            blocked.push("parameters_incomplete");
        }
        if !self.laser_commissioned {
            blocked.push("laser_not_commissioned");
        }
        self.laser_readable = actual.is_some();
        if let Some(a) = actual {
            if a.interlock_1 != 0 || a.interlock_2 != 0 {
                blocked.push("interlock_open");
            }
            if a.emergency_stop != 0 {
                blocked.push("emergency_stopped");
            }
            if a.q_gate != 0 && !emitting {
                blocked.push("q_gate_open");
            }
            if a.fault_bits != 0 {
                blocked.push("laser_fault");
            }
            if a.overtemperature != 0 {
                blocked.push("over_temperature");
            }
        } else {
            blocked.push("laser_unavailable");
        }
        self.allowed = blocked.is_empty() && self.armed;
        self.detail = if !self.armed {
            "未布防".into()
        } else if self.allowed {
            "允许：参数齐备且联锁全部安全".into()
        } else {
            format!("禁止出光：{}", blocked.join("、"))
        };
        self.state = if !self.devices_opened {
            "offline"
        } else if !self.armed {
            "standby"
        } else if self.allowed {
            "ready"
        } else if blocked == ["parameters_incomplete"] {
            "standby"
        } else {
            "fault"
        };
        self.blocked_by = blocked;
    }
    pub fn snapshot(&self) -> Value {
        json!({"allowed":self.allowed,"blocked_by":self.blocked_by,"detail":self.detail,"state":self.state,"armed":self.armed,"laser_readable":self.laser_readable})
    }
}
pub fn lamp_intent(state: &str) -> &'static str {
    match state {
        "ready" | "acquiring" => "ready_green",
        "standby" => "standby_yellow",
        "fault" | "recovering" => "fault_red",
        _ => "offline_all_off",
    }
}
