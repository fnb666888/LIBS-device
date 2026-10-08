#pragma once

// 业务一：就绪判定（readiness）—— 预检编排。
//
// ── 这个服务回答一个问题：「现在能不能出光，灯该怎么亮」 ──────────────────
//
// 它是**唯一**允许把"门禁结论"变成"物理动作（写灯）"的地方。
// 全库没有第二处把判定结果下发到状态灯 —— 出光服务自己不刷灯，UI 也不刷灯。
//
// 依赖方向（严格单向，不得反向）：
//     设备事实(Laser B2) --poll--> evaluate() --结论--> Indicator::applyAll()
//
// **"预检"和"亮灯"不拆成两个**：它们的输入输出是同一个门禁结论。
// 拆开会出现"预检说过了、灯还没亮"这种无意义的中间态。

#include <string>
#include <vector>

#include "libs/business/readiness/lamp.hpp"
#include "libs/business/readiness/monitor.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/error.hpp"

namespace business::readiness {

// 预检结论。**不只是"过/不过"，还要能回答"为什么不过"**。
struct PrecheckReport {
    MachineState state = MachineState::offline;
    bool allowed = false;                    // 门禁是否放行
    std::vector<std::string> blocked_by;    // 不放行时逐条列出原因
    std::string detail;                     // 中文小结，可直接写日志

    // 灯显示意图（意图名，不是实际灯色）。给 status 与日志用。
    std::string lamp_intent;
    bool lamp_applied = false;              // 灯是否成功下发
    std::string lamp_error;                 // 下发失败原因（灯是辅助指示，不影响 allowed）

    // 读激光状态是否成功。false = 读不到，按"不安全"处理，不当作"没事"。
    bool laser_readable = false;
};

// 设备预检服务：读设备事实 -> 门禁判定 -> 按结论亮灯。
//
// 一个服务完成"预检 + 亮灯"，不拆成两个。
class PrecheckService {
public:
    explicit PrecheckService(ReadinessMonitor& monitor,
                             libs::devices::Indicator* indicator);

    // 执行一次完整预检：读事实 -> 判定 -> 亮灯。
    //
    // 返回值与 allowed 语义一致：失败 = 门禁不放行。
    // 但报告通过 last_report() 拿 —— 失败时也要知道是哪几条不满足、灯亮没亮。
    libs::VoidResult run();

    // 只下发灯，不重新判定。给"状态变了但判定不必重跑"的场合用
    // （例如收光后灯要从绿转黄，但门禁条件本身没变）。
    libs::VoidResult sync_lamp_only();

    // 强制全灭。**只给"关设备"这条路径用**：退出时门禁已经 disarm，
    // 状态停在 standby -> 本该刷黄灯，但软件都要关了，留一圈灯在
    // 假装系统还活着。下次开机前现场看到一片黄的机器，会误以为它还可用。
    libs::VoidResult force_offline();

    const PrecheckReport& last_report() const noexcept { return last_report_; }

    // 灯显示意图名（给 status 上报用）。
    const std::string& lamp_intent() const noexcept { return last_report_.lamp_intent; }

    // 把报告转成 JSON（status 命令用）。
    json::Value to_json(const PrecheckReport& report) const;

private:
    // 一次灯下发并把结果如实记进 report。**不改变 verdict、不阻断返回值。**
    void record_lamp(PrecheckReport& report, MachineState state);

    ReadinessMonitor& monitor_;
    libs::devices::Indicator* indicator_;
    PrecheckReport last_report_;
};

}  // namespace business::readiness