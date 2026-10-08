#include "libs/transport/serial_timing.hpp"

namespace libs::transport {

const char* to_string(ReadStep step) noexcept {
    switch (step) {
        case ReadStep::keep_reading:   return "keep_reading";
        case ReadStep::frame_complete: return "frame_complete";
        case ReadStep::timed_out:      return "timed_out";
    }
    return "unknown";
}

ReadStep next_read_step(bool have_bytes,
                        unsigned long long idle_gap_ms,
                        unsigned long long elapsed_ms,
                        unsigned idle_ms,
                        unsigned timeout_ms) {
    // 帧完整优先于超时：一帧恰好在 timeout 边界上完成时，它确实是一帧完整应答，
    // 不该被报成"没等到响应"。反过来先判超时就会把这种边界帧误判掉。
    //
    // have_bytes 的与运算同时承担另一条纪律：**一个字节都没收到时，空闲多久
    // 都不算帧**。少了它，超时路径就会凭空造出一个空帧，
    // 把"设备干脆没应答"伪装成"收到了一帧空数据"。
    if (have_bytes && idle_gap_ms >= idle_ms) return ReadStep::frame_complete;
    if (elapsed_ms >= timeout_ms) return ReadStep::timed_out;
    return ReadStep::keep_reading;
}

}  // namespace libs::transport
