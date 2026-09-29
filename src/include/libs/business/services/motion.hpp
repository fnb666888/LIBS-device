#pragma once

// 运动与位置切换（能力 12）。
//
// 判定依据：范围报告 §3.12
//   12.1 正反转运动时间参数项 [B]
//   12.2 位置切换（三相电机 + 限位机构）[C]
//
// **本文件最重要的纪律是"子项不得互相代偿"**（范围报告 §7 纪律 4）：
// 12.1 的 1 s 占位行程时间**不得**被当成 12.2 电机已可用的证据。
// 所以 MotionTiming 里 `calibrated=false` 是硬性表达，而 12.2 只有一个
// 返回 unsupported 的接口。
//
// 前置条件（范围报告 §3.12.2）：
//   ① 电机控制方式（继电器/接触器/变频器）② 限位开关是干接点还是总线
//   ③ 接线图核实 ④ 正反转行程时间标定
// 驱动层**完全没有**电机/继电器/限位入口（`src/` 内无 motor/relay/position 代码），
// 任务书 §2.2 明确"不复用旧 MCC4 步进电机绑定"。

#include <cstdint>
#include <optional>
#include <string>

#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"

namespace business::services {

// 正反转运动时间参数项。两个独立项（12.1）。
//
// 当前是**占位值 1 s**（任务书 R01：未标定，占位 1 s）。
// 纪律：占位值必须带 calibrated 标记，任何输出/日志/上报都不得省略。
// **计时完成 != 已证实到位**（任务书 §3 明示）。
struct MotionTiming {
    std::optional<double> forward_time_s = 1.0;   // 正转行程时间；占位 1 s
    std::optional<double> reverse_time_s = 1.0;   // 反转行程时间；占位 1 s
    bool calibrated = false;                       // 当前恒为 false

    static MotionTiming placeholder() {
        MotionTiming t;
        t.forward_time_s = 1.0;
        t.reverse_time_s = 1.0;
        t.calibrated = false;
        return t;
    }
};

json::Value to_json(const MotionTiming& timing);

// ── 12.2 位置切换 [C]───────────────────────────────────────────────────────
//
// 返回 unsupported。**绝不返回“已到位”**——那会让上层以为电机可控，
// 而驱动层根本没有电机入口。计时完成只说明时间到了，不说明实物到位。
class PositionSwitch {
public:
    virtual ~PositionSwitch() = default;

    virtual bool available() const noexcept = 0;

    // 请求切换到某位置。**当前唯一实现返回 unsupported。**
    virtual libs::VoidResult switch_to(int position) = 0;

    // 查询当前位置。**没有限位反馈来源**（前置条件②未确认），返回 unsupported。
    virtual libs::Result<int> current_position() = 0;

    virtual std::string name() const = 0;
};

// C 类实现：明确 unsupported，并把四条前置条件写进错误信息。
class UnavailablePositionSwitch final : public PositionSwitch {
public:
    bool available() const noexcept override { return false; }
    libs::VoidResult switch_to(int position) override;
    libs::Result<int> current_position() override;
    std::string name() const override { return "unavailable"; }

    static std::string prerequisites();
};

}  // namespace business::services
