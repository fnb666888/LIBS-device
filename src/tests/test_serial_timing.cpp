// 传输层读循环判定测试：纯逻辑，不碰真实串口、不打开任何 COM 口。
//
// 背景（2026-10-07 现场缺陷）：`SerialLink::exchange()` 的读循环唯一出口是
// `elapsed >= timeout_ms`，`idle_ms` 与 `last_receive_tick` 都被接收/记录
// 却从不参与判定。后果是**每次串口事务都耗满整个 timeout_ms**（实测
// `elapsed_ms=655`、`timed_out=true`，而 response_hex 是一帧完整合法应答），
// 进而把 status 拖到每 3 秒有 2 秒、把出光前的使能等待拖到十几秒，
// 并让把 timed_out 当错误的 SU300 在真机上每次事务都判失败。
//
// 这里把"读循环该不该结束"抽成纯函数，用真实时间参数覆盖边界。
#include "check.hpp"
#include "libs/transport/serial_timing.hpp"

using libs::transport::next_read_step;
using libs::transport::ReadStep;

namespace {

constexpr unsigned kTimeoutMs = 650;   // 与 site.json 的 timeout_ms 一致
constexpr unsigned kIdleMs = 80;       // 与 site.json 的 idle_ms 一致

}  // namespace

TEST_MAIN_BEGIN()

{
    // 一个字节都没收到，时间还没用完：继续等第一个字节。
    CASE(no_bytes_yet_keeps_reading);
    CHECK_EQ(next_read_step(false, 0, 10, kIdleMs, kTimeoutMs), ReadStep::keep_reading);
    CHECK_EQ(next_read_step(false, 0, 640, kIdleMs, kTimeoutMs), ReadStep::keep_reading);
}

{
    // 一个字节都没收到且时间用完：这才是真正的"没等到响应"。
    CASE(no_bytes_at_timeout_is_timed_out);
    CHECK_EQ(next_read_step(false, 0, kTimeoutMs, kIdleMs, kTimeoutMs), ReadStep::timed_out);
    CHECK_EQ(next_read_step(false, 0, 900, kIdleMs, kTimeoutMs), ReadStep::timed_out);
}

{
    // 刚收到字节、空闲还没到 idle_ms：可能还有后续字节，继续读。
    CASE(fresh_bytes_below_idle_gap_keeps_reading);
    CHECK_EQ(next_read_step(true, 0, 30, kIdleMs, kTimeoutMs), ReadStep::keep_reading);
    CHECK_EQ(next_read_step(true, 79, 120, kIdleMs, kTimeoutMs), ReadStep::keep_reading);
}

{
    // 这就是本次修复的核心：收到字节后空闲满 idle_ms，帧就算结束，
    // 事务立刻返回，不再白等到 timeout_ms。
    CASE(bytes_settled_for_idle_gap_completes_frame);
    CHECK_EQ(next_read_step(true, kIdleMs, 100, kIdleMs, kTimeoutMs), ReadStep::frame_complete);
    CHECK_EQ(next_read_step(true, 200, 300, kIdleMs, kTimeoutMs), ReadStep::frame_complete);
}

{
    // 帧完整优先于超时：即使时间预算已经用完，只要空闲够了就是完整帧。
    // （否则一帧恰好在 timeout 边界完成时会被误判成超时。）
    CASE(complete_frame_wins_over_timeout);
    CHECK_EQ(next_read_step(true, kIdleMs, kTimeoutMs, kIdleMs, kTimeoutMs), ReadStep::frame_complete);
    CHECK_EQ(next_read_step(true, 300, 5000, kIdleMs, kTimeoutMs), ReadStep::frame_complete);
}

{
    // 收到过字节但帧不完整（空闲不够就用完预算）：仍算超时，
    // 语义与 serial.hpp 对 timed_out 的注释一致——"在收到完整帧之前就用完了时间预算"。
    CASE(incomplete_frame_at_timeout_is_timed_out);
    CHECK_EQ(next_read_step(true, 10, kTimeoutMs, kIdleMs, kTimeoutMs), ReadStep::timed_out);
    CHECK_EQ(next_read_step(true, 79, 1000, kIdleMs, kTimeoutMs), ReadStep::timed_out);
}

{
    // 没收到字节时 idle_gap 没有意义，无论多大都不能据此宣称帧完整——
    // 否则超时路径会退化成"凭空造出一个空帧"。
    CASE(no_bytes_never_completes_a_frame);
    CHECK_EQ(next_read_step(false, 100000, 10, kIdleMs, kTimeoutMs), ReadStep::keep_reading);
    CHECK_EQ(next_read_step(false, 100000, kTimeoutMs, kIdleMs, kTimeoutMs), ReadStep::timed_out);
}

{
    // idle_ms=0 表示"收到字节即认为帧结束"。语义上合法，但要验证它
    // 确实不再白等——这正是慢事务的极端形式。
    CASE(zero_idle_gap_completes_as_soon_as_bytes_arrive);
    CHECK_EQ(next_read_step(true, 0, 20, 0, kTimeoutMs), ReadStep::frame_complete);
}

TEST_MAIN_RETURN()
