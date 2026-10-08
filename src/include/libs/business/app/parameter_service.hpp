#pragma once

// 出光参数管理 —— **测量参数的唯一来源**。
//
// ── 为什么单独一个服务 ────────────────────────────────────────────────────
//
// 这个项目栽过两次同一个坑：
//
//   1. 界面上的激光器参数写进业务参数库，出光读的却是另一份站点配置
//      （site.json 的 laser_fire）。操作员把电流改成 50 A，出光还是 80 A。
//   2. 光谱仪的 integration_delay_ns 同期在站点配置和参数库里各存一份。
//
// 根因都是**同一件事有两个来源**。所以本服务确立一条纪律：
//
//   **测量参数只有一个来源，就是 ParameterStore（parameters.json）。**
//   SiteConfig 只负责"装机配置"（端口、序列号、接法、门闩），
//   它提供的是激光器出光参数的**首次开机种子值**，一旦参数库里有值，
//   就以参数库为准。光谱侧的三个参数出厂默认在本服务里（kDefault*），
//   装机配置不再提供 —— 免得又出现第二份来源。
//
//   测量参数（归本服务 / ParameterStore）：
//       激光器：discharge_current_a / pulse_width_us / frequency_hz /
//               discharge_delay_us / q_delay_us / q_warmup_s /
//               settle_timeout_ms / emission_hold_ms / min_discharges
//       光谱仪：integration_ms / capture_timeout_ms / integration_delay_ns
//
//   装机配置（归 SiteConfig）：
//       端口号、sdk_dll、serials / master_serial / synchronized、
//       wait_for_laser_trigger、indicator_model、设备地址、commissioned
//
// 判据：**换一块硬件要改的**是装机配置；**调一次测量要改的**是测量参数。
// 前者是硬件事实（错了机器接不上），后者是工艺事实（错了只是这次测不准）。
//
// ── 三条纪律 ───────────────────────────────────────────────────────────────
//
//   1. **只做搬运与校验，不做判定**。本服务不决定"能不能出光"——那是门禁的事。
//      它只回答"这次用哪些参数"，并保证这些参数合法。
//
//   2. **量程校验只有一套**。走 config::validate_laser_fire()，与站点配置
//      校验同一个函数。不接受第二套规则，否则"界面限 100 A、请求能传 500 A"
//      这种破口会在两处各开一次。
//
//   3. **参数库是权威，种子只来自真机记录**。seed_defaults() 只补缺失的字段，
//      绝不覆盖操作员已经调过的值 —— 否则每次重启都把现场调好的参数打回
//      出厂设置。

#include <map>
#include <string>
#include <vector>

#include "libs/business/shared/config/site_config.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/business/shared/config/parameter_store.hpp"
#include "libs/business/measurement/job.hpp"
#include "libs/business/measurement/frame.hpp"
#include "libs/error.hpp"

namespace business::app {
class Runtime;
}

namespace business::app {

// 出光参数管理服务。
//
// 持有 ParameterStore 的读访问，并把"参数库 -> 出光参数"这条搬运路径收在一处。
// 界面改参数（param_set）走 ParameterStore 直写；出光读参数走 fire_parameters()。
// 两条路径**共用同一个 store**，所以不存在"改了不生效"。
class ParameterService {
public:
    explicit ParameterService(app::Runtime& runtime) : runtime_(runtime) {}

    // 判定"核心参数齐备"所必需的分组。
    //
    // **不含 trigger**：触发分组描述的是接法（激光器 TTL 打到光谱仪哪个脚），
    // 那属于装机配置，不是操作员每次要调的测量参数。把它算进必填，
    // 门禁会因为一个跟测量无关的组卡在 parameters_incomplete。
    static std::vector<std::string> required_groups();

    // 各分组的必填字段。字段名与 LaserFireConfig 成员名一一对应，
    // 避免出现"参数库里的名字"和"代码里的名字"两套。
    static std::map<std::string, std::vector<std::string>> required_fields();

    // 启动时把**真机默认参数**补进参数库（激光 9 个 + 光谱 3 个），只补缺失的。
    //
    // 默认值全部取自 docs/real-device-parameters-20260929.md —— 那是本机
    // 2026-09-29 真机联调确认过的工作点，不是通用占位值。于是进程启动即可
    // 检测，界面第一次 param_get 拿到的也就是这些真机参数。
    //
    // **必须先 load() 再 seed**：反了会拿默认值盖掉操作员已经调好的参数。
    // seed 只补**缺失**字段：操作员在参数页调过的值永远优先，重启不会打回。
    //
    // 注意：**不再从 site.json 的 laser_fire 抄** —— 那条路会把不带 --config
    // 启动时的一串 0 种进参数库。默认值只来自上面那份真机记录。
    libs::VoidResult seed_defaults();

    // "本机已标定"不再来自 site.json 的 commissioned 字段，而是按参数库
    // 实际内容算：laser 组 9 个字段齐备且全部落在协议量程内。
    // 参数管理页面因此成了唯一的标定入口 —— 填齐合法值即视为已标定。
    bool commissioned_from_store() const;

    // 组装本次出光要用的全部参数。
    //
    // 基准 = 参数库当前值；args 里的 laser / spectrometer 对象作为**本次覆盖**。
    // 覆盖只影响这一次，不写回参数库 —— 需要长期生效的改动走 param_set。
    //
    // 校验在这里做，与站点配置共用 validate_laser_fire()。参数不合法时
    // 返回 invalid_argument，**不会**带着坏参数去动设备。
    libs::Result<measurement::FireParameters> fire_parameters(const json::Value& args) const;

    // 参数库是否齐备。门禁的 parameters_ready 位由它驱动。
    config::ReadinessReport readiness() const;

    // 光谱侧测量参数，直接给实时光谱流用。
    //
    // 与 fire_parameters() 取的是同一份参数库，**不是两套**：
    // 预览的积分时间和出光的积分时间必然一致。参数库为空时回落到
    // 参数管理服务自己的出厂默认值，装机配置不再提供种子。
    measurement::SpectrumMeasurement spectrometer_measurement() const;

private:
    // 读一个测量参数。字段名不存在返回 invalid_argument。
    libs::Result<double> get(const std::string& group, const std::string& field) const;

    // 读一个测量参数，缺失时回落给调用方的默认值。
    //
    // 为什么需要它：实时光谱预览要的只是光谱侧三个数（积分时间/超时/
    // 积分延时），与激光器标定无关。走 fire_parameters() 会顺带校验
    // laser.commissioned，于是"激光器还没标定"会让**预览也起不来** ——
    // 而预览只是拍照，和能不能出光无关。
    double value_or(const std::string& group, const std::string& field,
                    double fallback) const;

    // 从参数库读出全部 9 个出光参数。**缺失即报错**，不回落装机配置 ——
    // 出光参数只有参数库一个来源，操作员在参数管理页面改什么就是什么。
    libs::VoidResult collect_laser(measurement::FireParameters& out) const;

    libs::VoidResult collect_spectrometer(measurement::FireParameters& out) const;

    app::Runtime& runtime_;
};

}  // namespace business::services
