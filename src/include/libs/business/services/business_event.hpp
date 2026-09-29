#pragma once

// 业务事件模型（能力 5）。
//
// 判定依据：范围报告 §3.5
//   3.5.1 "开始检测"业务事件 [A]
//   3.5.2 网关信号接入 -> 业务事件 [B]  —— 只放类型、适配器接口、转换契约
//   3.5.3 料流段划分 / 换带 / 料流结束 [C] —— 只放 unsupported 桩
//
// **本文件最重要的纪律是"业务信号 != 物理触发"**（任务书 §2.1、§3 步骤 6）。
// 领域事件 `DetectionStartRequested` 表示"业务上要求开始检测"，
// 它**不携带任何"激光已出光"或"硬件已触发"的含义**。
// 谁把它变成物理触发，是采集协调（能力 10）和门禁（能力 4）的事，不是这里。
// 如果日后有人在本文件里加一个 `bool laser_fired`，那就已经破坏了这条边界。

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"

namespace business::events {

// 检测阶段。顺序即流程顺序，用于日志与恢复上下文的可读性。
// 阶段名本身是业务语义，不与任何设备状态绑定。
enum class DetectionStage {
    idle,            // 未开始
    selected_belt,   // 已选目标带
    approaching,     // 接近目标带
    measuring,       // 采集中
    evaluating,      // 判定/分析中
    reporting,       // 交付/上报中
    done,            // 本带完成
};
const char* to_string(DetectionStage stage) noexcept;

// ── 3.5.1 开始检测业务事件 [A]─────────────────────────────────────────────

// "业务上请求开始检测"。注意它**只是请求**：
// 门禁（能力 4）可能拒绝它，采集协调（能力 10）可能拒绝执行它。
// 事件被接受 != 出光许可 != 采集完成。
struct DetectionStartRequested {
    std::string target_belt;        // 目标带号；空串表示"未指定"，不是"任意带"
    DetectionStage stage = DetectionStage::idle;
    std::string requested_by;       // 谁发起的（CLI / 上位机 / 自动）
    Milliseconds wall_clock_ms = 0;
    std::map<std::string, std::string> attributes;  // 附加上下文，原样带过
};

// 事件的统一信封。加字段时**只加可选字段**，不要改已有字段的含义。
struct EventEnvelope {
    // 事件名，取值来自 to_string()：detection_start_requested /
    // target_belt_changed / ore_present。
    std::string name;
    Milliseconds wall_clock_ms = 0;
    std::int64_t sequence = 0;      // 进程内单调递增；重启后重新计数，日志里靠时间戳区分
    json::Value payload = json::Value();  // 与 name 对应的具体事件体
};

json::Value to_json(const DetectionStartRequested& event);
json::Value to_json(const EventEnvelope& envelope);

// ── 3.5.2 网关信号接入 -> 业务事件 [B]──────────────────────────────────────

// 目标带切换。带号语义（连续编号？带号是标签还是位置？）未确认，
// 因此这里**只承载一个不透明字符串**，不做任何解释或换算。
struct TargetBeltChanged {
    std::string belt;               // 原样带号；空串表示"网关报了个空带号"，不是"没变化"
    std::string previous_belt;      // 业务层自己记的上一次；首次为 null
    Milliseconds wall_clock_ms = 0;
    std::string raw_message;        // 原始报文，原样留存便于复盘
};

// 物料在场。来源可以是测距观测（能力 8）也可以是外部信号。
// **两者都不确定时 ore_present 不得为 true**：见 OrePresenceState::unknown。
enum class OrePresenceState { absent, present, unknown };
const char* to_string(OrePresenceState state) noexcept;

struct OrePresent {
    OrePresenceState state = OrePresenceState::unknown;   // 默认未知，不是"无矿"
    std::string source;              // "rangefinder" / "gateway" / ...
    std::string basis;               // 判定依据摘要（阈值/观测/报文）
    Milliseconds wall_clock_ms = 0;
};

json::Value to_json(const TargetBeltChanged& event);
json::Value to_json(const OrePresent& event);

// 网关适配器：**只有契约，没有传输实现**。
//
// 卡点 G2（范围报告 §3.5.2）：`src/` 内没有任何 TCP/UDP 客户端，
// 网关协议、报文格式、接收地址都未确认。因此本接口**不允许自带 socket**，
// 也不提供任何"默认连 192.168.101.100"的实现。
// 协议定下来之后，新增一个实现类即可，调用方不需要改。
class GatewayAdapter {
public:
    virtual ~GatewayAdapter() = default;

    // 适配器是否就绪。未就绪时 poll() 必须返回 unsupported 而不是伪造事件。
    virtual bool available() const noexcept = 0;

    // 轮询一次，把收到的报文转换成业务事件。返回的事件是**已经判读过的业务事件**，
    // 不是原始字节——判读规则属于适配器实现，协议定了才知道。
    // 没有新消息返回空数组，这是正常情况，不算错误。
    virtual libs::VoidResult poll(std::vector<EventEnvelope>& out) = 0;

    // 适配器名字，用于日志与 `businessd status` 展示。
    virtual std::string name() const = 0;
};

// 没有网关时的实现：available() 为 false，poll() 返回 unsupported。
// 这不是"桩函数糊过去"，而是**诚实地表达"这条链路不存在"**：
// 范围报告 §3.5.2 的卡点就是没有网络层，调用方应当看到 unsupported 并去要协议。
class UnavailableGatewayAdapter final : public GatewayAdapter {
public:
    bool available() const noexcept override { return false; }
    libs::VoidResult poll(std::vector<EventEnvelope>& out) override;
    std::string name() const override { return "unavailable"; }
};

// ── 3.5.3 料流段划分 / 换带 / 料流结束 [C]───────────────────────────────────
//
// 前置条件（全部未确认，任务书 §3 末段明示"本版未补设规则"）：
//   1. 料流段如何划分：按时间？按带号？按总量？
//   2. 换带的触发条件是什么？
//   3. 料流中断时是暂停采集还是继续？
//
// 因此本模块**只提供规则接口与 unsupported 实现**。
// 特别地：**不提供**"按固定秒数切段"这类默认值——那会让下游以为规则已定。
class FeedSegmentRules {
public:
    virtual ~FeedSegmentRules() = default;

    // 当前是否处于一个料流段内。未配置规则时返回 unsupported。
    virtual libs::Result<bool> in_feed_segment() const = 0;

    // 换带是否应当触发。未配置规则时返回 unsupported。
    virtual libs::Result<bool> should_change_belt() const = 0;

    virtual std::string describe() const = 0;
};

// C 类桩：明确 unsupported，并把三条前置条件写进错误信息。
// 纪律（范围报告 §4）：**不返回 false / 0 / 空**——那会被当成"规则判定为否"。
class UnspecifiedFeedSegmentRules final : public FeedSegmentRules {
public:
    libs::Result<bool> in_feed_segment() const override;
    libs::Result<bool> should_change_belt() const override;
    std::string describe() const override { return "unspecified"; }
};

}  // namespace business::events
