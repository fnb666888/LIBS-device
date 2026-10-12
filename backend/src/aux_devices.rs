use crate::{
    config::SerialConfig,
    error::{Error, Result},
    modbus,
    serial::{Link, Transaction},
};
use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
pub struct Register {
    pub address: u16,
    pub raw: u16,
    pub name: &'static str,
    pub unit: &'static str,
    pub valid: bool,
    pub scale: f64,
    pub value: f64,
    pub documented: bool,
    pub writable: bool,
    pub is_signed: bool,
}
pub fn range_register(address: u16, raw: u16) -> Register {
    let (name, unit, valid) = match address {
        0x0200 => ("distance_zero_on_out_of_range", "mm", raw != 0),
        0x0201 => ("distance_max_on_out_of_range", "mm", raw != 0xffff),
        0x0202 => ("advanced_distance_raw", "mm", true),
        0x0203 => ("reflection_peak_raw", "", true),
        0x0204 => ("confidence", "percent", raw <= 100),
        0x0205 => ("velocity_raw", "mm/s", true),
        0x0206 => ("temperature_raw", "degC", true),
        0x0207 => ("sample_counter_raw", "", true),
        0x0208 => ("slave_address_raw", "", true),
        0x0209 | 0x020c => ("reserved", "", true),
        0x020a => ("reflection_threshold_raw", "", true),
        0x020b => ("confidence_threshold_raw", "", true),
        0x020d => ("application_distance_lower_raw", "", true),
        0x020e => ("application_distance_upper_raw", "", true),
        0x020f => ("filter_coefficient_raw", "", true),
        0x0210 => ("filter_threshold_raw", "", true),
        _ => ("unknown_raw", "", true),
    };
    Register {
        address,
        raw,
        name,
        unit,
        valid,
        scale: 1.,
        value: raw as f64,
        documented: name != "unknown_raw",
        writable: false,
        is_signed: false,
    }
}
pub fn ac_register(address: u16, raw: u16) -> Register {
    let (name, unit, scale, signed, documented) = match address {
        30001 => ("indoor_temp", "degC", 0.1, true, true),
        30002 => ("evaporator_temp", "degC", 0.1, true, true),
        30003 => ("condenser_temp", "degC", 0.1, true, true),
        30004 => ("reserved_30004", "degC", 0.1, true, false),
        30005 => ("reserved_30005", "", 1., false, false),
        30006 => ("reserved_30006", "", 1., false, false),
        30009 => ("humidity", "%RH", 0.1, true, true),
        30010 => ("reserved_30010", "%RH", 0.1, true, false),
        30011 => ("reserved_30011", "%RH", 0.1, true, false),
        30014 => ("cooling_on_setpoint", "degC", 0.1, true, true),
        30015 => ("cooling_off_setpoint", "degC", 0.1, true, true),
        30016 => ("heating_on_setpoint", "degC", 0.1, true, true),
        30017 => ("heating_off_setpoint", "degC", 0.1, true, true),
        30018 => ("indoor_high_alarm_setpoint", "degC", 0.1, true, true),
        30019 => ("indoor_low_alarm_setpoint", "degC", 0.1, true, true),
        30020 => ("condenser_high_protect", "degC", 0.1, true, true),
        30021 => ("evaporator_freeze_protect", "degC", 0.1, true, true),
        30022 => ("dehumidify_on_humidity", "%RH", 0.1, true, true),
        30023 => ("dehumidify_off_humidity", "%RH", 0.1, true, true),
        30024 => ("humidity_alarm_limit", "%RH", 0.1, true, true),
        30025 => ("humidity_calibration", "", 1., true, true),
        30026 => ("mode", "", 1., false, true),
        30027 => ("baud_rate", "", 1., false, true),
        30028 => ("slave_address", "", 1., false, true),
        _ => ("unknown_raw", "", 1., false, false),
    };
    Register {
        address,
        raw,
        name,
        unit,
        valid: true,
        scale,
        value: if signed {
            raw as i16 as f64 * scale
        } else {
            raw as f64 * scale
        },
        documented,
        writable: (30014..=30028).contains(&address),
        is_signed: signed,
    }
}
fn read_registers(
    link: &mut dyn Transaction,
    address: u8,
    start: u16,
    count: u16,
    offset: u16,
) -> Result<Vec<u16>> {
    if count == 0 || count > 125 || start as u32 + count as u32 > 65536 {
        return Err(Error::invalid(
            "bad_request",
            "register range must hold 1..125 registers without wrapping",
        ));
    }
    let request = modbus::request(address, 3, offset, count)?;
    let reply = link.exchange(&request)?;
    Ok(modbus::data(&reply, &request, count as usize * 2)?
        .chunks_exact(2)
        .map(|b| u16::from_be_bytes([b[0], b[1]]))
        .collect())
}
pub struct Rangefinder {
    link: Box<dyn Transaction>,
    address: u8,
}
impl Rangefinder {
    pub fn open(config: &SerialConfig, address: u8) -> Result<Self> {
        Ok(Self::with_transport(Link::open(config)?, address))
    }
    pub fn with_transport(link: impl Transaction + 'static, address: u8) -> Self {
        Self {
            link: Box::new(link),
            address,
        }
    }
    pub fn read(&mut self, start: u16, count: u16) -> Result<Vec<Register>> {
        Ok(
            read_registers(self.link.as_mut(), self.address, start, count, start)?
                .into_iter()
                .enumerate()
                .map(|(i, raw)| range_register(start + i as u16, raw))
                .collect(),
        )
    }
}
const STATUS: [(u16, &str); 15] = [
    (34, "cooling"),
    (35, "heater"),
    (36, "indoor_fan"),
    (37, "outdoor_fan"),
    (41, "power"),
    (42, "indoor_sensor_fault"),
    (43, "evaporator_sensor_fault"),
    (44, "condenser_sensor_fault"),
    (46, "humidity_sensor_fault"),
    (47, "humidity_high_alarm"),
    (50, "condenser_high_alarm"),
    (51, "evaporator_freeze_alarm"),
    (52, "cooling_alarm"),
    (53, "high_temp_alarm"),
    (54, "low_temp_alarm"),
];
#[derive(Debug, Serialize)]
pub struct StatusBit {
    pub address: u16,
    pub name: &'static str,
    pub active: bool,
    pub is_alarm: bool,
}
#[derive(Debug, Serialize)]
pub struct Status {
    pub address: u8,
    pub start: u16,
    pub count: u16,
    pub raw_bytes: Vec<u8>,
    pub bits: Vec<StatusBit>,
}
impl Status {
    pub fn active_alarms(&self) -> impl Iterator<Item = &StatusBit> {
        self.bits.iter().filter(|b| b.is_alarm && b.active)
    }
}
#[repr(u16)]
#[derive(Clone, Copy)]
pub enum Mode {
    Standby = 0,
    Cooling = 1,
    Heating = 2,
    Dehumidify = 3,
}
pub struct AirConditioner {
    link: Box<dyn Transaction>,
    address: u8,
    held_mode: Option<Mode>,
    last_mode_sent: u64,
}
impl AirConditioner {
    pub fn open(config: &SerialConfig, address: u8) -> Result<Self> {
        Ok(Self::with_transport(Link::open(config)?, address))
    }
    pub fn with_transport(link: impl Transaction + 'static, address: u8) -> Self {
        Self {
            link: Box::new(link),
            address,
            held_mode: None,
            last_mode_sent: 0,
        }
    }
    pub fn read_status(&mut self, start: u16, count: u16) -> Result<Status> {
        if start < 10001 || count == 0 || start as u32 + count as u32 > 10059 {
            return Err(Error::invalid(
                "bad_request",
                "status count must keep the request within 10001..10058",
            ));
        }
        let base = start - 10001;
        let request = modbus::request(self.address, 1, base, count)?;
        let reply = self.link.exchange(&request)?;
        let raw = modbus::data(&reply, &request, (count as usize).div_ceil(8))?.to_vec();
        let bits = STATUS
            .iter()
            .filter(|(offset, _)| *offset >= base && *offset < base + count)
            .map(|(offset, name)| {
                let relative = (*offset - base) as usize;
                StatusBit {
                    address: 10001 + offset,
                    name,
                    active: raw[relative / 8] & (1 << (relative % 8)) != 0,
                    is_alarm: *offset >= 42,
                }
            })
            .collect();
        Ok(Status {
            address: self.address,
            start,
            count,
            raw_bytes: raw,
            bits,
        })
    }
    pub fn read_parameters(&mut self, start: u16, count: u16) -> Result<Vec<Register>> {
        if start < 30001 || count == 0 || count > 50 || start as u32 + count as u32 > 30036 {
            return Err(Error::invalid(
                "bad_request",
                "parameter count must be 1..50 and stay within 30001..30035",
            ));
        }
        Ok(read_registers(
            self.link.as_mut(),
            self.address,
            start,
            count,
            start - 30001,
        )?
        .into_iter()
        .enumerate()
        .map(|(i, raw)| ac_register(start + i as u16, raw))
        .collect())
    }
    pub fn set_power(&mut self, on: bool) -> Result<u16> {
        self.write(5, 41, if on { 0xff00 } else { 0 })
    }
    pub fn write_parameter(&mut self, register: u16, raw: u16) -> Result<u16> {
        if !ac_register(register, raw).writable {
            return Err(Error::invalid(
                "not_writable",
                format!("register {register} is not in the documented FC06 set"),
            ));
        }
        self.write(6, register - 30001, raw)
    }
    pub fn set_setpoint(&mut self, register: u16, value: f64) -> Result<u16> {
        let definition = ac_register(register, 0);
        if definition.name == "unknown_raw" {
            return Err(Error::invalid(
                "unknown_register",
                format!("register {register} is not in the parameter table"),
            ));
        }
        if !definition.writable {
            return Err(Error::invalid(
                "not_writable",
                format!("register {register} is read-only"),
            ));
        }
        let scaled = value / definition.scale;
        if !scaled.is_finite() || scaled.abs() > 65535. {
            return Err(Error::invalid(
                "out_of_range",
                "value does not fit a 16-bit register after scaling",
            ));
        }
        let raw = scaled.round() as i32 as u16;
        let range = match register {
            30014 => Some((200, 550)),
            30015 => Some((150, 350)),
            30016 => Some((-300, 150)),
            30017 => Some((0, 230)),
            30018 => Some((300, 600)),
            30019 => Some((-100, 100)),
            30022..=30024 => Some((0, 1000)),
            _ => None,
        };
        if range.is_some_and(|(lo, hi)| !(lo..=hi).contains(&(raw as i16 as i32))) {
            return Err(Error::invalid(
                "out_of_range",
                format!(
                    "value {value} {} is outside the documented range",
                    definition.unit
                ),
            ));
        }
        self.write_parameter(register, raw)
    }
    fn write(&mut self, function: u8, register: u16, value: u16) -> Result<u16> {
        let request = modbus::request(self.address, function, register, value)?;
        let reply = self.link.exchange(&request)?;
        modbus::write_reply(&reply, &request)
    }
    pub fn set_mode(&mut self, mode: Mode) -> Result<u16> {
        self.write_parameter(30026, mode as u16)
    }
    pub fn hold_mode(&mut self, mode: Option<Mode>) {
        self.held_mode = mode;
        self.last_mode_sent = 0;
    }
    pub fn tick_mode_hold(&mut self, now: u64, keepalive_ms: u64) -> Result<bool> {
        let Some(mode) = self.held_mode else {
            return Ok(false);
        };
        if self.last_mode_sent != 0 && now.saturating_sub(self.last_mode_sent) < keepalive_ms {
            return Ok(false);
        }
        self.set_mode(mode)?;
        self.last_mode_sent = now;
        Ok(true)
    }
}
