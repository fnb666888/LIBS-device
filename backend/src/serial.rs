use crate::{
    config::SerialConfig,
    error::{Error, Result},
    parameters::FireParameters,
    protocol::{self, LaserActual},
};
use parking_lot::Mutex;
use serialport::{ClearBuffer, DataBits, Parity, SerialPort};
use std::{
    io::{Read, Write},
    time::{Duration, Instant},
};

pub struct Link {
    port: Box<dyn SerialPort>,
    timeout: Duration,
    idle: Duration,
}
pub trait Transaction: Send {
    fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>>;
}
impl Link {
    pub fn open(config: &SerialConfig) -> Result<Self> {
        let port = serialport::new(&config.port, config.baud)
            .data_bits(if config.data_bits == 7 {
                DataBits::Seven
            } else {
                DataBits::Eight
            })
            .parity(match config.parity.as_str() {
                "even" => Parity::Even,
                "odd" => Parity::Odd,
                _ => Parity::None,
            })
            .timeout(Duration::from_millis(5))
            .open()
            .map_err(|e| {
                Error::new(
                    "transport",
                    "open_failed",
                    format!("cannot open {}: {e}", config.port),
                )
            })?;
        Ok(Self {
            port,
            timeout: Duration::from_millis(config.timeout_ms as u64),
            idle: Duration::from_millis(80),
        })
    }
    pub fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        self.port
            .clear(ClearBuffer::Input)
            .map_err(|e| Error::new("transport", "purge_failed", e.to_string()))?;
        self.port
            .write_all(request)
            .map_err(|e| Error::new("transport", "write_failed", e.to_string()))?;
        let started = Instant::now();
        let mut last = None;
        let mut reply = vec![];
        let mut buffer = [0u8; 4096];
        while started.elapsed() < self.timeout {
            match self.port.read(&mut buffer) {
                Ok(n) if n > 0 => {
                    reply.extend_from_slice(&buffer[..n]);
                    last = Some(Instant::now());
                }
                Ok(_) => (),
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => (),
                Err(e) => return Err(Error::new("transport", "read_failed", e.to_string())),
            }
            if last.is_some_and(|t| t.elapsed() >= self.idle) {
                break;
            }
            if reply.len() > 1024 * 1024 {
                return Err(Error::new(
                    "transport",
                    "receive_too_large",
                    "serial reply exceeds 1 MB",
                ));
            }
        }
        Ok(reply)
    }
}
impl Transaction for Link {
    fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        Link::exchange(self, request)
    }
}
#[derive(Debug, Clone, Default)]
pub struct LaserTrace {
    pub sent_frames: Vec<String>,
    pub settle_ms: u64,
    pub counter_before: u32,
}
pub struct Laser {
    link: Mutex<Box<dyn Transaction>>,
}
impl Laser {
    pub fn open(config: &SerialConfig) -> Result<Self> {
        Ok(Self::with_transport(Link::open(config)?))
    }
    pub fn with_transport(link: impl Transaction + 'static) -> Self {
        Self {
            link: Mutex::new(Box::new(link)),
        }
    }
    fn read_locked(link: &mut dyn Transaction, trace: &mut LaserTrace) -> Result<LaserActual> {
        let request = protocol::laser_frame(0xb2, &[])?;
        let reply = link.exchange(&request)?;
        let actual = LaserActual::decode(&reply)?;
        trace.sent_frames.push(protocol::hex(&request));
        Ok(actual)
    }
    fn write_locked(
        link: &mut dyn Transaction,
        command: u8,
        bytes: &[u8],
        trace: &mut LaserTrace,
    ) -> Result<()> {
        let request = protocol::laser_frame(command, bytes)?;
        link.exchange(&request)?;
        trace.sent_frames.push(protocol::hex(&request));
        Ok(())
    }
    pub fn actual(&self) -> Result<LaserActual> {
        Self::read_locked(self.link.lock().as_mut(), &mut LaserTrace::default())
    }
    pub fn prepare(&self, params: &FireParameters) -> Result<LaserTrace> {
        let mut guard = self.link.lock();
        let link = guard.as_mut();
        let mut trace = LaserTrace::default();
        let baseline = Self::read_locked(link, &mut trace)?;
        trace.counter_before = baseline.discharge_counter;
        if baseline.fault_bits != 0 {
            return Err(Error::new(
                "device",
                "laser_fault",
                "激光器 0xB2 故障位非零，拒绝写入会话参数",
            ));
        }
        for (command, field) in [
            (0x11, "discharge_current_a"),
            (0x12, "pulse_width_us"),
            (0x13, "frequency_hz"),
            (0x14, "discharge_delay_us"),
            (0x16, "q_delay_us"),
            (0x19, "q_warmup_s"),
        ] {
            Self::write_locked(
                link,
                command,
                &(params.laser[field] as u16).to_be_bytes(),
                &mut trace,
            )?;
        }
        Self::write_locked(link, 0x22, &[0], &mut trace)?;
        let startup = (|| {
            Self::write_locked(link, 0x21, &[1], &mut trace)?;
            let started = Instant::now();
            while started.elapsed().as_millis() < params.laser["settle_timeout_ms"] as u128 {
                let actual = Self::read_locked(link, &mut trace)?;
                if actual.fault_bits != 0 {
                    return Err(Error::new(
                        "device",
                        "laser_fault_during_settle",
                        "等待放电期间激光器报了故障位",
                    ));
                }
                if actual.work_state == 1 {
                    trace.settle_ms = started.elapsed().as_millis() as u64;
                    return Ok(());
                }
                std::thread::sleep(Duration::from_millis(100));
            }
            Err(Error::new(
                "device",
                "laser_not_discharging",
                "工作使能后设备没有进入「正在工作且放电」，拒绝开光闸",
            ))
        })();
        if let Err(error) = startup {
            let _ = Self::stop_locked(link);
            return Err(error);
        }
        Ok(trace)
    }
    pub fn open_gate(&self, trace: &mut LaserTrace) -> Result<()> {
        let mut guard = self.link.lock();
        let link = guard.as_mut();
        if let Err(e) = Self::write_locked(link, 0x24, &[1], trace) {
            let _ = Self::stop_locked(link);
            return Err(e);
        }
        Ok(())
    }
    fn stop_locked(link: &mut dyn Transaction) -> Result<()> {
        let mut trace = LaserTrace::default();
        let mut first = None;
        for (command, prefix) in [(0x24, "收光(0x24=0)失败："), (0x21, "停机(0x21=0)失败：")]
        {
            if let Err(e) = Self::write_locked(link, command, &[0], &mut trace) {
                first.get_or_insert(format!("{prefix}{}", e.message));
            }
        }
        match Self::read_locked(link, &mut trace) {
            Err(e) => {
                first.get_or_insert(format!("读回确认失败：{}", e.message));
            }
            Ok(actual) if actual.q_gate != 0 => {
                first.get_or_insert("已下发收光，但读回仍显示光闸开着；请现场人工确认".into());
            }
            Ok(actual) if actual.work_state != 0 => {
                first.get_or_insert("已下发停机，但读回仍显示正在工作；请现场人工确认".into());
            }
            _ => (),
        }
        match first {
            Some(e) => Err(Error::new("device", "laser_stop_incomplete", e)),
            None => Ok(()),
        }
    }
    pub fn stop(&self) -> Result<()> {
        Self::stop_locked(self.link.lock().as_mut())
    }
}

pub struct Indicator {
    link: Link,
    model: String,
    address: u8,
    gap: Duration,
    last: Option<Instant>,
}
impl Indicator {
    pub fn open(config: &SerialConfig, model: String, address: u8) -> Result<Self> {
        Ok(Self {
            link: Link::open(config)?,
            model,
            address,
            gap: Duration::from_millis(config.idle_ms.max(50) as u64),
            last: None,
        })
    }
    fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        if let Some(last) = self.last {
            std::thread::sleep(self.gap.saturating_sub(last.elapsed()));
        }
        self.last = Some(Instant::now());
        self.link.exchange(request)
    }
    pub fn apply(&mut self, state: &str) -> Result<()> {
        let model = protocol::lamp_model(&self.model)?;
        if self.address == 0 || self.address > 247 {
            return Err(Error::invalid(
                "bad_address",
                "Modbus address must be 1..247",
            ));
        }
        self.exchange(&protocol::modbus_frame(self.address, 5, 0, 0))?;
        let active = match state {
            "ready" | "acquiring" => "green",
            "standby" => "yellow",
            "fault" | "recovering" => "red",
            _ => "",
        };
        if !active.is_empty() {
            self.exchange(&protocol::modbus_frame(
                self.address,
                5,
                protocol::lamp_coil(model, active),
                0xff00,
            ))?;
        }
        for channel in ["red", "yellow", "green", "blue", "white", "buzzer"] {
            if ["blue", "white"].contains(&channel) && !model.has_blue_white
                || channel == "buzzer" && model.rgb_only
            {
                continue;
            }
            let reply = self.exchange(&protocol::modbus_frame(
                self.address,
                1,
                protocol::lamp_register(channel),
                1,
            ))?;
            let raw = protocol::lamp_decode(&reply, self.address, channel)?;
            if raw != if channel == active { 0xff } else { 0 } {
                return Err(Error::new(
                    "device",
                    "indicator_state_mismatch",
                    "状态灯写后读回的状态与请求不一致",
                ));
            }
        }
        Ok(())
    }
}
