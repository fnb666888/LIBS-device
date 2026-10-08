#pragma once

// 串口读循环的判定逻辑。**纯函数，不碰 Win32、不开句柄**——
// 抽出来的唯一理由就是它能被离线测试覆盖：`SerialLink` 本身必须持有真实
// COM 句柄才构造得出来，而本项目所有测试都刻意不碰真串口，
// 于是读循环的正确性在 2026-10-07 之前一直没有任何测试。

namespace libs::transport {

// 读循环的下一步。
enum class ReadStep {
    // 时间没用完、也没有完整的帧：继续轮询。
    keep_reading,
    // 已收到字节，且距最后一次收到字节已满 idle_ms：帧结束，事务应立即返回。
    frame_complete,
    // 时间预算用完仍未等到完整帧。对应 Reply::timed_out。
    timed_out,
};

const char* to_string(ReadStep step) noexcept;

// 决定一次串口事务的读循环该怎么走。
//
//   have_bytes  —— 本事务到目前为止是否收到过字节
//   idle_gap_ms —— 距最后一次收到字节过去了多久；have_bytes 为假时无意义
//   elapsed_ms  —— 事务开始至今
//   idle_ms     —— 帧结束所需的空闲间隔（来自站点配置，默认 80）
//   timeout_ms  —— 时间预算（来自站点配置，默认 650）
//
// 判定顺序有讲究：**帧完整优先于超时**。一帧恰好在 timeout 边界上完成时，
// 它确实是一帧完整应答，不该被报成"没等到响应"。
//
// `have_bytes` 为假时 idle_gap 再大也不得出帧：否则超时路径会退化成
// 凭空造一个空帧，把"设备干脆没应答"伪装成"收到了一帧空数据"。
ReadStep next_read_step(bool have_bytes,
                        unsigned long long idle_gap_ms,
                        unsigned long long elapsed_ms,
                        unsigned idle_ms,
                        unsigned timeout_ms);

}  // namespace libs::transport
