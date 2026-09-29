#pragma once

// 光谱采集协调与节奏映射（能力 10）。
//
// 判定依据：范围报告 §3.10
//   3.10.1 光谱采集协调（多通道 + 外触发）[B]
//   3.10.2 工作频率 -> 采集节奏映射 [B]
//
// 三条纪律：
//   1. **hardware_synchronized 必须原样上送**（范围报告 §3.10.1）。
//      `ScanGroup::hardware_synchronized` 是 SDK 报的事实，业务层
//      **不得**由"我同时请求了 4 个通道"推断为"物理上已同步"。
//      业务层只能表达"软件认为同步"。
//   2. **工作频率值缺失时不猜**（范围报告 §3.10.2）。映射函数接受空频率
//      必须返回"未配置"，**不得回落某个 Hz**。
//   3. 联合触发的"主从方向、接线细节、同步效果尚未验证"（任务书 §2.2），
//      因此多通道同步验收留待 D 阶段；本类只做参数构造与编排，不做接线判断。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "libs/business/config/site_config.hpp"
#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/devices/spectrometer.hpp"
#include "libs/error.hpp"

namespace business::services {

// 工作频率。**value 是 optional：未配置就是没有，不是 0 Hz，也不是默认某个值。**
//
// 卡点（范围报告 §3.10.2，任务书 §4.1）：工作频率数值留空，
// 且**不与结果发布周期等同**。单位与换算同样未确认，因此 frequency_hz 也带标记。
struct WorkFrequency {
    std::optional<double> frequency_hz;
    bool unit_confirmed = false;      // 单位未确认前不参与任何换算
    bool calibrated = false;          // 占位 vs 标定值

    static WorkFrequency unconfigured() { return WorkFrequency{}; }
    bool configured() const noexcept { return frequency_hz.has_value(); }
};

// 采集节奏。由工作频率映射而来，或由配置直接给出。
//
// **每个字段都可能"未配置"**。本结构刻意没有"默认值"字段——
// 一旦有默认扫描次数/超时，回落就会把"频率没填"这件事吃掉。
struct CapturePace {
    bool configured = false;                 // 是否可用
    std::uint32_t scans = 1;                // 本次采集的扫描次数
    std::optional<double> period_ms;         // 期望周期；未配置 = nullopt
    std::optional<unsigned> timeout_ms;      // 采集超时；未配置 = nullopt
    // 本次采集的积分时间；未配置 = nullopt（用站点配置的 integration_ms）。
    //
    // 出光流程会把它设成**界面上显示的那个值**。界面能调、实际不生效，
    // 是比界面上根本没有这个控件更坏的事——这个项目已经栽过一次
    // （激光器参数写在界面上、出光却读的是另一份配置）。
    std::optional<double> integration_ms;
    std::string source;                      // "work_frequency" / "config" / "unconfigured"
    std::string note;                        // 中文说明，日志用
};

// 采集协调。职责边界：
//   * 在**已经**被门禁放行的前提下，按配置构造 CaptureRequest 并驱动 scans 循环；
//   * 把驱动层的 ScanGroup 转成业务采集数据组（含原样的 hardware_synchronized）。
//
// **它不判断门禁**（那是能力 4），**不填成分结果**（那是能力 11，算法 C 类）。
class CaptureCoordinator {
public:
    explicit CaptureCoordinator(const config::SiteConfig& site);

    // 由工作频率映射采集节奏。频率缺失/未标定时返回 configured=false，
    // **不猜任何默认值**（范围报告 §3.10.2 纪律）。
    static CapturePace pace_from_frequency(const WorkFrequency& frequency,
                                           const config::SiteConfig& site);

    // 由配置直接给节奏（不经工作频率）。capture_timeout_ms / integration_ms
    // 来自站点配置，是已确认的值；scans 缺省 1 也是保守值。
    static CapturePace pace_from_config(const config::SiteConfig& site);

    // 构造一次采集请求。**多通道同步与外触发组合仍会构造成功**——
    // 但 hardware_synchronized 的判定权在 SDK/硬件，业务层不干预。
    libs::Result<libs::devices::CaptureRequest> build_request(const CapturePace& pace) const;

    // 驱动一次有界采集。device 为空返回 unsupported。
    //
    // 关键：即使 SDK 返回 complete=false（部分通道缺失），
    // **也要把拿到的 groups 交给调用方**，并在业务侧保留原样标志。
    // 光谱通道缺失是"第三态"，不是"采集失败"也不是"正常"（范围报告 §7 纪律 2）。
    libs::Result<libs::devices::CaptureResult> capture(libs::devices::Spectrometer* device,
                                                      const CapturePace& pace) const;

    // 同上，但**显式覆盖触发方式**。用于出光流程（AvaSoft 手册 §4.9 接法二）：
    // 那里的触发源就是激光器，光谱仪必须从 pin 6 等它的 TTL，
    // 因此不能只依赖站点配置里那个"是否等触发"的开关——
    // 出光流程下它是**必然为真**的物理事实，配置没填也要等。
    //
    // integration_delay_ns 仍来自站点配置，业务层不猜这个数值。
    libs::Result<libs::devices::CaptureResult> capture(libs::devices::Spectrometer* device,
                                                      const CapturePace& pace,
                                                      bool wait_for_laser_trigger,
                                                      double integration_delay_ns) const;

    // 把一次 ScanGroup 抽成"软件视角的同步判断"——原样透传 SDK 标志，
    // 额外记录"业务层是否请求过 synchronized"，但**不合并成一个布尔**。
    struct SyncView {
        bool hardware_synchronized = false;   // 原样：SDK 报的是什么就是什么
        bool requested_sync = false;         // 业务层请求了什么
        std::vector<std::string> arm_order;
        bool complete = false;
    };
    static SyncView sync_view(const config::SiteConfig& site,
                              const libs::devices::ScanGroup& group);

    // 把采集结果序列化成**可落盘的载荷**——能力 6.3.1 本地交付出口要保存的东西。
    //
    // 刻意含完整 wavelength_nm + counts：本地交付存的是**原始光谱**，
    // 只存 shot_id / 通道名一类的元数据等于存了个空壳。UI 的显示口径
    // （归一化、基线扣除）不在这里做，那是前端的事。
    //
    // 缺失通道**不补零、不占位**：它压根不出现在 channels 里，与实时流的
    // 纪律一致（范围报告 §7 纪律 2：补零画出来是"这条谱是平的"）。
    static json::Value to_payload(const std::vector<libs::devices::ScanGroup>& groups);

    const config::SiteConfig& site() const noexcept { return site_; }

private:
    config::SiteConfig site_;
};

}  // namespace business::services
