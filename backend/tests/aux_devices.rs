use libs_backend::{
    aux_devices::{AirConditioner, Rangefinder, ac_register, range_register},
    error::Result,
    modbus,
    protocol::crc16,
    serial::Transaction,
};
use std::{
    collections::VecDeque,
    sync::{Arc, Mutex},
};
struct Link {
    requests: Arc<Mutex<Vec<Vec<u8>>>>,
    replies: VecDeque<Vec<u8>>,
}
impl Transaction for Link {
    fn exchange(&mut self, request: &[u8]) -> Result<Vec<u8>> {
        self.requests.lock().unwrap().push(request.into());
        Ok(self.replies.pop_front().unwrap_or_else(|| request.into()))
    }
}
fn frame(mut bytes: Vec<u8>) -> Vec<u8> {
    bytes.extend_from_slice(&crc16(&bytes).to_le_bytes());
    bytes
}
#[test]
fn modbus_noise_echo_exceptions_and_ambiguity() {
    let request = modbus::request(1, 3, 0x0200, 2).unwrap();
    let reply = frame(vec![1, 3, 4, 0, 0, 0xff, 0xff]);
    let mut capture = vec![0xaa, 0xbb];
    capture.extend_from_slice(&request);
    capture.extend_from_slice(&reply);
    assert_eq!(
        modbus::data(&capture, &request, 4).unwrap(),
        &[0, 0, 0xff, 0xff]
    );
    capture.extend_from_slice(&reply);
    assert_eq!(
        modbus::data(&capture, &request, 4).unwrap_err().code,
        "ambiguous_reply"
    );
    assert_eq!(
        modbus::data(&frame(vec![1, 0x83, 2]), &request, 4)
            .unwrap_err()
            .code,
        "modbus_exception"
    );
    assert_eq!(
        modbus::data(&request, &request, 4).unwrap_err().code,
        "no_matching_reply"
    );
}
#[test]
fn range_sentinels_and_air_conditioner_signed_values_offsets_and_limits() {
    assert!(!range_register(0x200, 0).valid);
    assert!(!range_register(0x201, 0xffff).valid);
    assert!(!range_register(0x204, 101).valid);
    assert_eq!(ac_register(30025, 0xfffc).value, -4.);
    assert_eq!(ac_register(30016, 0xff9c).value, -10.);
    let requests = Arc::new(Mutex::new(vec![]));
    let mut range = Rangefinder::with_transport(
        Link {
            requests: requests.clone(),
            replies: vec![frame(vec![1, 3, 4, 0, 1, 0, 2])].into(),
        },
        1,
    );
    assert_eq!(range.read(0x200, 2).unwrap()[1].raw, 2);
    assert!(range.read(65535, 2).is_err());
    let mut ac = AirConditioner::with_transport(
        Link {
            requests: requests.clone(),
            replies: vec![frame(vec![1, 1, 1, 0x81])].into(),
        },
        1,
    );
    let status = ac.read_status(10035, 8).unwrap();
    assert!(
        status
            .bits
            .iter()
            .find(|b| b.name == "cooling")
            .unwrap()
            .active
    );
    assert!(
        status
            .bits
            .iter()
            .find(|b| b.name == "power")
            .unwrap()
            .active
    );
    assert_eq!(ac.set_setpoint(30016, -10.).unwrap(), 0xff9c);
    assert!(ac.set_setpoint(30016, -30.1).is_err());
    assert!(ac.set_setpoint(30001, 25.).is_err());
    assert!(ac.set_setpoint(30014, f64::NAN).is_err());
    ac.set_power(true).unwrap();
    let requests = requests.lock().unwrap();
    assert_eq!(&requests[1][2..6], &[0, 34, 0, 8]);
    assert_eq!(&requests[2][2..6], &[0, 15, 0xff, 0x9c]);
    assert_eq!(&requests[3][2..6], &[0, 41, 0xff, 0]);
}
