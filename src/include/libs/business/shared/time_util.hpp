#pragma once

// 业务时间戳（能力 5/7 共用的最小工具）。
//
// 为什么不用 std::chrono 直接写在结构体里：事件要跨进程序列化给 CLI，
// 落到 JSON 上必须是一个确定的数字。这里统一用**毫秒整数**。
//
// 两种时间，分开不混：
//   wall_clock_ms  墙钟（可与日志时间对齐，会被校时改动）
//   monotonic_ms   单调钟（只用于算间隔，不受校时影响）

#include <cstdint>

namespace business {

using Milliseconds = std::int64_t;

// 墙钟毫秒（自 Unix 纪元）。用于日志与报告里的"什么时候发生的"。
Milliseconds wall_clock_ms() noexcept;

// 单调钟毫秒（任意原点）。用于算"距上次多久了"，不与墙钟混用。
Milliseconds monotonic_ms() noexcept;

}  // namespace business
