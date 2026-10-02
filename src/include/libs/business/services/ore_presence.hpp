#pragma once

// 测距与物料判定（能力 8）。
//
// 判定依据：范围报告 §3.8
//   3.8.1 测距观测采集 [A]
//   3.8.2 物料判断（有矿判定）[B]
//
// 两条纪律：
//   1. **先记录、后判定。** 3.8.1 要求"raw 距离与 confidence 原样记录"，
//      3.8.2 消费这份观测。判定**不得回写修改原始观测**。
//   2. **协议层的 valid=true 默认是"本层不判断"**，不是"这一读有效"。
//      `libs::rangefinder::Register::valid` 默认 true，业务层必须自己处理
//      哨兵值/超量程。范围报告 §3.8.1 明确点名了这一点。
//
// 3.8.2 的卡点（任务书 R03）：**绝对距离还是距离变化尚未明确**，
// 3-5 cm 是暂定范围。因此本模块**不填任何阈值**，
// 判定结构支持"绝对量"和"变化量"两种 basis，但默认 unconfigured。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"
#include "libs/protocols/rangefinder.hpp"

namespace business::services {

// 一次测距观测。**原样保留 raw 与哨兵标记**，不做工程量换算的判定。
//
// 0200/0201 不是应用距离的上下限：它们是同一实时距离的两种兼容表示，
// 分别使用 0 和 0xffff 表示超量程。0202 是高级版本的 0.1 mm 分辨率读数。
// 因此 in_range 只表示设备读数没有命中相应的超量程哨兵，不能把 0200/0201
// 的 raw 值拿来组成一个 [下限, 上限] 区间。
struct DistanceObservation {
    Milliseconds wall_clock_ms = 0;
    std::uint16_t distance_raw = 0;
    // 实际选作 distance_raw 的寄存器地址；0 表示本次没有读到距离寄存器。
    std::uint16_t distance_register = 0;
    // 仅描述协议分辨率，不代表实际精度；0202=0.1 mm，0200/0201=1 mm。
    double distance_resolution_mm = 0.0;
    std::uint16_t confidence_raw = 0;
    std::uint16_t sample_counter = 0;

    // 按所选距离寄存器的哨兵语义整理出的有效性；协议层 valid 不能单独代替它。
    bool device_valid = false;
    bool in_range = false;

    // 本次读取是否通信成功。false 时 distance_raw 等字段无意义，
    // 读失败**不是**"距离为 0"。
    bool read_ok = false;
    std::string read_error;

    // 设备温度/速度等辅助寄存器，原样带上，不做换算。
    std::map<std::string, std::uint16_t> auxiliary_raw;
};

json::Value to_json(const DistanceObservation& obs);

// 从一次驱动读结果构造观测。**不判断是否有矿**，只整理事实。
DistanceObservation observe(const libs::rangefinder::Reading& reading,
                            const libs::Error& error_or_none);

// ── 3.8.2 物料判断结构 [B]───────────────────────────────────────────────────

// 判定依据的种类。**两者都未确认**（任务书 R03：绝对距离还是距离变化未明确）。
enum class DecisionBasis {
    unconfigured,   // 默认：没有可用规则
    absolute,       // 绝对距离落在某区间（"离传感器很近 = 有矿"）
    delta,          // 距离**变化量**（矿石不均匀，不能把连续有料理解为距离恒定）
};
const char* to_string(DecisionBasis basis) noexcept;

// 判定参数。**全部是 optional：未标定就是 nullopt，不是 0。**
//
// 卡点（范围报告 §3.8.2）：阈值与判定方向都未定，3-5 cm 是暂定范围不填入。
// 纪律：占位值必须带标记（范围报告 §7 纪律 3）——本结构用 optional 天然实现。
struct OrePresencePolicy {
    DecisionBasis basis = DecisionBasis::unconfigured;
    std::optional<double> threshold;      // 阈值，cm；未标定 = nullopt
    std::optional<double> hysteresis;      // 滞回，cm；未标定 = nullopt
    int consecutive_confirmations = 0;    // 连续 N 次确认；0 = 未配置
    bool calibrated = false;              // 占位 vs 标定值的可辨别标记

    // 单位未确认前一律 cm，且本字段把单位显式化，避免上层默认当成 mm。
    std::string distance_unit = "cm(未确认)";

    static OrePresencePolicy uncalibrated() { return OrePresencePolicy{}; }
};

// 一次判定结果。
//
// 关键：ore_present 是 optional。**没有观测、没有规则、通信失败，
// 三种情况都不是"无矿"，而是"不知道"**（范围报告 §7 纪律 2：未知 ≠ 正常）。
struct OreDecision {
    std::optional<bool> ore_present;      // nullopt = 未知
    std::string basis;                    // 判定依据摘要
    OrePresencePolicy parameters_snapshot;  // 判定时用的参数快照
    Milliseconds decided_at_ms = 0;
    bool has_observation = false;         // 是否有可用观测
};

json::Value to_json(const OreDecision& decision);

// 物料判定器。
//
// 当前**不做任何判定**：阈值与方向都未标定，任何 observe+decide 都返回
// ore_present = nullopt（未知），而不是"无矿"。
// 结构先立住，规则定了之后填进去即可，上层不需要改。
class OrePresenceJudge {
public:
    explicit OrePresenceJudge(OrePresencePolicy policy = OrePresencePolicy::uncalibrated());

    // 记一次观测。**观测原样保存，判定不改它**（先记录、后判定）。
    void record(const DistanceObservation& observation);

    // 取最近一次观测（只读）。没有观测返回 nullopt。
    const std::optional<DistanceObservation>& last_observation() const noexcept { return last_; }

    // 基于最近观测做判定。未配置规则 / 无观测 / 通信失败 / 超量程
    // 都返回 nullopt（未知），绝不返回 false（无矿）。
    OreDecision decide() const;

    const OrePresencePolicy& policy() const noexcept { return policy_; }
    bool calibrated() const noexcept { return policy_.calibrated; }

    static std::string prerequisites();

private:
    OrePresencePolicy policy_;
    std::optional<DistanceObservation> last_;
};

}  // namespace business::services
