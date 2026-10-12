use crate::error::{Error, Result};

pub fn crc16(bytes: &[u8]) -> u16 {
    bytes.iter().fold(0xffff, |mut crc, byte| {
        crc ^= *byte as u16;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xa001
            } else {
                crc >> 1
            };
        }
        crc
    })
}
pub fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02X}"))
        .collect::<Vec<_>>()
        .join(" ")
}
pub fn laser_frame(command: u8, payload: &[u8]) -> Result<Vec<u8>> {
    if payload.len() > 251 {
        return Err(Error::invalid(
            "request_build_failed",
            "laser payload exceeds 251 bytes",
        ));
    }
    let mut frame = vec![0x5a, 0xa5, (payload.len() + 4) as u8, 0, command];
    frame.extend_from_slice(payload);
    frame.extend_from_slice(&crc16(&frame[3..]).to_le_bytes());
    Ok(frame)
}
pub fn laser_decode(bytes: &[u8], command: u8) -> Result<&[u8]> {
    let fail = |code, message| Error::new("protocol", code, message);
    let payload = match command {
        0xb1 => 18,
        0xb2 => 22,
        0xbc => 20,
        _ => {
            return Err(Error::new(
                "unsupported",
                "unsupported_command",
                "Only B1, B2 and BC read responses are decoded by this function",
            ));
        }
    };
    if bytes.len() < 7 {
        return Err(fail(
            "truncated",
            "A complete laser frame needs at least seven bytes",
        ));
    }
    if bytes[..2] != [0x5a, 0xa5] {
        return Err(fail("bad_header", "The response must begin with 5A A5"));
    }
    if bytes[2] < 4 || bytes.len() != bytes[2] as usize + 3 {
        return Err(fail(
            "bad_length",
            "The length byte must match the actual frame size exactly",
        ));
    }
    if bytes[3] != 0 {
        return Err(fail(
            "bad_address",
            "The transcribed protocol uses device address 00",
        ));
    }
    if bytes[4] != command {
        return Err(fail(
            "unexpected_command",
            "The response command does not match the pending request",
        ));
    }
    if crc16(&bytes[3..bytes.len() - 2])
        != u16::from_le_bytes(bytes[bytes.len() - 2..].try_into().unwrap())
    {
        return Err(fail(
            "crc_mismatch",
            "CRC must cover the address, command and payload",
        ));
    }
    if bytes.len() != payload + 7 {
        return Err(fail(
            "bad_payload_length",
            "Read responses have fixed payload lengths; an echo is not a response",
        ));
    }
    Ok(&bytes[5..bytes.len() - 2])
}
pub fn laser_reply(bytes: &[u8], command: u8) -> Result<&[u8]> {
    let mut found = None;
    for offset in 0..bytes.len().saturating_sub(6) {
        if bytes[offset..offset + 2] != [0x5a, 0xa5] {
            continue;
        }
        let end = offset + bytes[offset + 2] as usize + 3;
        if end > bytes.len() {
            continue;
        }
        if let Ok(payload) = laser_decode(&bytes[offset..end], command) {
            if found.is_some() {
                return Err(Error::new(
                    "protocol",
                    "ambiguous_reply",
                    "the reply contains more than one complete laser frame; it is not clear which one answers this request",
                ));
            }
            found = Some(payload);
        }
    }
    found.ok_or_else(|| {
        Error::new(
            "protocol",
            "no_frame",
            "no complete laser frame in the reply",
        )
    })
}

#[derive(Debug, Clone, Default)]
pub struct LaserActual {
    pub work_state: u8,
    pub fault_bits: u8,
    pub interlock_1: u8,
    pub interlock_2: u8,
    pub emergency_stop: u8,
    pub overtemperature: u8,
    pub q_gate: u8,
    pub work_enabled: u8,
    pub discharge_counter: u32,
}
impl LaserActual {
    pub fn decode(reply: &[u8]) -> Result<Self> {
        let p = laser_reply(reply, 0xb2)?;
        Ok(Self {
            work_state: p[0],
            fault_bits: p[2],
            interlock_1: p[3],
            interlock_2: p[4],
            emergency_stop: p[5],
            overtemperature: p[6],
            discharge_counter: u32::from_be_bytes(p[13..17].try_into().unwrap()),
            q_gate: p[19],
            work_enabled: p[20],
        })
    }
}
pub fn modbus_frame(address: u8, function: u8, register: u16, value: u16) -> Vec<u8> {
    let mut bytes = vec![address, function];
    bytes.extend_from_slice(&register.to_be_bytes());
    bytes.extend_from_slice(&value.to_be_bytes());
    bytes.extend_from_slice(&crc16(&bytes).to_le_bytes());
    bytes
}
pub fn lamp_register(channel: &str) -> u16 {
    match channel {
        "red" => 1,
        "green" => 2,
        "blue" => 3,
        "yellow" => 4,
        "white" => 5,
        _ => 6,
    }
}
#[derive(Clone, Copy)]
pub struct LampModel {
    pub k50: bool,
    pub rgb_only: bool,
    pub has_blue_white: bool,
}
pub fn lamp_model(model: &str) -> Result<LampModel> {
    let (k50, rgb_only, has_blue_white) = match model {
        "CL50S+K50-485" => (true, false, false),
        "RL30-24V" => (false, true, false),
        "CL50S-USB-5V" | "TL50-485" => (false, false, false),
        "CL60-485" | "485-10-30V" | "485-24V-WK" => (false, false, true),
        _ => {
            return Err(Error::new(
                "unsupported",
                "unknown_indicator_model",
                format!("未登记的状态灯型号：{model}"),
            ));
        }
    };
    Ok(LampModel {
        k50,
        rgb_only,
        has_blue_white,
    })
}
pub fn lamp_coil(model: LampModel, channel: &str) -> u16 {
    match channel {
        "red" => 1,
        "yellow" => {
            if model.k50 {
                3
            } else {
                2
            }
        }
        "green" => {
            if model.k50 {
                2
            } else {
                3
            }
        }
        "buzzer" => 4,
        "blue" => 0x11,
        _ => 0x12,
    }
}
pub fn lamp_decode(bytes: &[u8], address: u8, channel: &str) -> Result<u8> {
    let reg = lamp_register(channel) as u8;
    let mut found = None;
    for window in bytes.windows(8) {
        if window[..5] == [address, 1, 0, reg, 0]
            && [0, 0xff, 0x11, 0x22].contains(&window[5])
            && crc16(window) == 0
        {
            if found.is_some() {
                return Err(Error::new(
                    "protocol",
                    "ambiguous_reply",
                    "more than one lamp response",
                ));
            }
            found = Some(window[5]);
        }
    }
    found.ok_or_else(|| {
        Error::new(
            "protocol",
            "no_frame",
            "no complete Modbus reply in the capture",
        )
    })
}
