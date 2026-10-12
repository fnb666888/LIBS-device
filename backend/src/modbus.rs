use crate::{
    error::{Error, Result},
    protocol::{crc16, modbus_frame},
};

pub fn request(address: u8, function: u8, register: u16, value: u16) -> Result<Vec<u8>> {
    if !(1..=247).contains(&address) {
        return Err(Error::invalid(
            "bad_address",
            "Modbus address must be 1..247",
        ));
    }
    Ok(modbus_frame(address, function, register, value))
}
pub fn locate<'a>(
    capture: &'a [u8],
    request: &[u8],
    length: usize,
    valid: impl Fn(&[u8]) -> bool,
    reject_echo: bool,
) -> Result<&'a [u8]> {
    if request.len() != 8 || crc16(request) != 0 || !(5..=260).contains(&length) {
        return Err(Error::invalid(
            "bad_request",
            "Expected a complete RTU request and a valid response length",
        ));
    }
    if capture.len() > 65536 {
        return Err(Error::new(
            "transport",
            "capture_too_large",
            "Serial capture exceeds 64 KiB; the frame is not trustworthy",
        ));
    }
    let mut found = None;
    let mut index = 0;
    while index + 2 <= capture.len() {
        if reject_echo && capture[index..].starts_with(request) {
            index += request.len();
            continue;
        }
        let exception = capture[index + 1] == (request[1] | 0x80);
        if capture[index] != request[0] || (!exception && capture[index + 1] != request[1]) {
            index += 1;
            continue;
        }
        let size = if exception { 5 } else { length };
        if index + size > capture.len() {
            index += 1;
            continue;
        }
        let frame = &capture[index..index + size];
        if crc16(frame) != 0
            || (if exception {
                frame[2] == 0
            } else {
                !valid(frame)
            })
        {
            index += 1;
            continue;
        }
        if found.is_some() {
            return Err(Error::new(
                "protocol",
                "ambiguous_reply",
                "Multiple CRC-valid matching responses in one transaction; the response is ambiguous",
            ));
        }
        found = Some(frame);
        index += size;
    }
    found.ok_or_else(||Error::new("protocol","no_matching_reply","No CRC-valid matching response: incomplete/corrupt frame, wrong address/function/count, or request echo only"))
}
pub fn data<'a>(capture: &'a [u8], request: &[u8], bytes: usize) -> Result<&'a [u8]> {
    let frame = locate(
        capture,
        request,
        bytes + 5,
        |p| p[2] as usize == bytes,
        true,
    )?;
    check_exception(frame)?;
    Ok(&frame[3..3 + bytes])
}
pub fn write_reply(capture: &[u8], request: &[u8]) -> Result<u16> {
    let frame = locate(capture, request, 8, |p| p[2..4] == request[2..4], false)?;
    check_exception(frame)?;
    Ok(u16::from_be_bytes(frame[4..6].try_into().unwrap()))
}
fn check_exception(frame: &[u8]) -> Result<()> {
    if frame[1] & 0x80 != 0 {
        return Err(Error::new(
            "device",
            "modbus_exception",
            format!("Modbus exception (code {})", frame[2]),
        ));
    }
    Ok(())
}
