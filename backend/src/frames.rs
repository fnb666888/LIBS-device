use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::{BTreeMap, VecDeque},
    sync::Arc,
};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Channel {
    pub serial: String,
    pub state: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub reason: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pixels: Option<u16>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub wavelength_nm: Option<Arc<Vec<f64>>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub counts: Option<Vec<f64>>,
    #[serde(skip)]
    pub device_time_ticks: u32,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Frame {
    pub frame_index: u64,
    pub shot_id: u64,
    pub wall_clock_ms: u64,
    pub captured_monotonic_ms: u64,
    pub hardware_synchronized: bool,
    pub requested_sync: bool,
    pub laser_shot: bool,
    pub expected_groups: u32,
    pub received_groups: u32,
    pub period_ms: f64,
    pub timeout_ms: u32,
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capture_error_code: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub capture_error_message: Option<String>,
    pub channels: Vec<Channel>,
}
impl Frame {
    pub fn failed(
        index: u64,
        requested_sync: bool,
        timeout_ms: u32,
        error: &crate::error::Error,
    ) -> Self {
        Self {
            frame_index: index,
            shot_id: 0,
            wall_clock_ms: crate::wall_clock_ms(),
            captured_monotonic_ms: crate::monotonic_ms(),
            hardware_synchronized: false,
            requested_sync,
            laser_shot: true,
            expected_groups: 0,
            received_groups: 0,
            period_ms: 0.,
            timeout_ms,
            ok: false,
            capture_error_code: Some(error.code.clone()),
            capture_error_message: Some(error.message.clone()),
            channels: vec![],
        }
    }
}
#[derive(Default)]
struct BusState {
    next_id: u64,
    frame_index: u64,
    delivered: u64,
    dropped: u64,
    subscribers: BTreeMap<u64, VecDeque<Arc<Frame>>>,
}
pub struct FrameBus {
    state: Mutex<BusState>,
    capacity: usize,
}
impl Default for FrameBus {
    fn default() -> Self {
        Self::new(8)
    }
}
impl FrameBus {
    pub fn new(capacity: usize) -> Self {
        Self {
            state: Mutex::new(BusState {
                next_id: 1,
                ..Default::default()
            }),
            capacity: capacity.max(1),
        }
    }
    pub fn subscribe(&self) -> u64 {
        let mut s = self.state.lock();
        let id = s.next_id;
        s.next_id += 1;
        s.subscribers.insert(id, VecDeque::new());
        id
    }
    pub fn unsubscribe(&self, id: u64) {
        self.state.lock().subscribers.remove(&id);
    }
    pub fn pop(&self, id: u64) -> Option<Arc<Frame>> {
        self.state.lock().subscribers.get_mut(&id)?.pop_front()
    }
    pub fn pending(&self, id: u64) -> usize {
        self.state
            .lock()
            .subscribers
            .get(&id)
            .map_or(0, VecDeque::len)
    }
    pub fn clear(&self) {
        for queue in self.state.lock().subscribers.values_mut() {
            queue.clear();
        }
    }
    pub fn publish(&self, frame: Arc<Frame>) {
        let mut s = self.state.lock();
        s.frame_index += 1;
        s.delivered += 1;
        let mut dropped = 0;
        for queue in s.subscribers.values_mut() {
            if queue.len() >= self.capacity {
                queue.pop_front();
                dropped += 1;
            }
            queue.push_back(frame.clone());
        }
        s.dropped += dropped;
    }
    pub fn status(&self) -> Value {
        let s = self.state.lock();
        json!({"subscribers":s.subscribers.len(),"frame_index":s.frame_index,"delivered_frames":s.delivered,"dropped_frames":s.dropped,"spectrometer_open":null,
        "note":"帧队列只负责存帧与取帧，不采集、不碰设备；帧由测量流程产出。见 measurement_start / measurement_status"})
    }
}
