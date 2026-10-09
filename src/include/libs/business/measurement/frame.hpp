#pragma once

// 业务二：激光触发光谱检测（measurement）—— 帧数据结构。
//
// ── 这个文件只定义"一帧长什么样"，不做任何事 ────────────────────────────
//
// 它是纯数据结构 + 序列化，没有任何行为：没有线程、没有设备、没有队列。
// 依赖方向也刻意收到最窄：只 include json 与时间工具这两个纯函数。
// 这样任何一边（采集方 / 订阅方）都能包含它而不会顺带引入别的东西。

#include <cstdint>
#include <string>
#include <vector>

#include "libs/business/shared/json/json.hpp"

namespace business::measurement {

// 一次采集要用的**全部测量参数**，由参数管理服务（ParameterService）从
// 参数库取出后交过来。
//
// 为什么不直接让各服务去读参数库：那样每个服务都要认参数文件名、
// 认字段名、认单位，字段一改要改一片，而且"这个字段归谁管"就再也说不清了。
// 参数库只被 ParameterService 认，其它服务只认本结构 —— **字段归属集中在一处**。
struct SpectrumMeasurement {
    // 0 表示尚未由 ParameterService 填入，校验会拒绝，不能作为工作默认值。
    double integration_ms = 0.0;         // 积分时间
    unsigned timeout_ms = 0;             // 采集超时（含等激光 TTL）
    // 收到激光 TTL 后延迟多久开始积分。自由触发模式下不用。
    //
    // 曾经放在 SpectrometerConfig 里，与参数库形成**两份来源** ——
    // 界面改了参数库的积分延时，实际生效的是配置里的另一个值。
    // 现已归并到参数库，此处只有一份。
    double integration_delay_ns = 0.0;
    // 期望帧周期。0 = 不限速，按设备节奏。
    double period_ms = 0.0;
    unsigned scans = 1;
};

// 一个通道在某一帧里的状态。三态，不与"数值 0"混。
enum class ChannelState {
    present,     // 这一帧采到了
    absent,      // 这次没采到；reason 里说明原因
};
const char* to_string(ChannelState state) noexcept;

// 单个通道的一帧数据。
struct ChannelFrame {
    std::string serial;
    ChannelState state = ChannelState::absent;
    std::string reason;            // state=absent 时的说明；present 时为空
    std::uint16_t pixels = 0;
    unsigned device_time_ticks = 0;
    std::vector<double> wavelength_nm;   // 长度 == pixels
    std::vector<double> counts;          // 长度 == pixels
};

// 一次采集产出的一帧。
//
// ── 搬过来的纪律（一条都不能丢）──────────────────────────────────────────
//
//  1. **通道缺失是第三态，不是"采集失败"**。
//     某个通道没回来，帧里那一路就是 `absent` + 原因，**不**整帧判失败，
//     **不**用零值填充。UI 据此把那条曲线标成"缺"，而不是画一条假的平线。
//
//  2. **`hardware_synchronized` 原样上送**。
//     本结构只搬运生产者给的事实，不判断"物理上同步了没有"，
//     `requested_sync` 单列一个字段，两件事不合并。
//
//  3. **帧里不做显示加工**。没有归一化、没有基线扣除、没有累加平均。
//     UI 想显示归一化曲线自己算 —— 把显示口径写死在业务层，改口径就得改库。
struct SpectrumFrame {
    std::int64_t frame_index = 0;
    std::uint64_t shot_id = 0;
    std::uint64_t wall_clock_ms = 0;
    std::uint64_t captured_monotonic_ms = 0;

    // 同步事实：由采集结果原样上送，与 requested_sync 合并不推断。
    bool hardware_synchronized = false;
    bool requested_sync = false;
    std::uint32_t expected_groups = 0;
    std::uint32_t received_groups = 0;

    // **本帧是否由激光触发采集而来**。
    //
    // 这是把"测量帧"和"空闲预览帧"分开的关键字段，也是本流程存在的理由。
    // true  = 这一帧对应一次真实出光，是 LIBS 测量谱。
    // false = 自由触发拍到的背景谱 / 杂散光，是诚实的观测值但**不是测量值**。
    //
    // 单看 wavelength_nm + counts 分不出这两者，所以它必须随帧携带。
    bool laser_shot = false;

    // 采集节奏，UI 可据此显示"当前多少 ms 一帧"。
    double period_ms = 0.0;
    unsigned timeout_ms = 0;

    std::vector<ChannelFrame> channels;

    // 采集层面的失败（如整次 capture 超时）。**空 = 这次采集本身成功**。
    // 通道缺失不走这里，走上面各 channel 的 state。
    std::string capture_error_code;
    std::string capture_error_message;
    bool ok() const noexcept { return capture_error_code.empty(); }
};

json::Value to_json(const ChannelFrame& channel);
json::Value to_json(const SpectrumFrame& frame);

}  // namespace business::measurement
