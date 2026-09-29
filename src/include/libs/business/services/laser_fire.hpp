#pragma once

// 激光出光与光谱采集编排（能力 10 的执行侧，本次新增）。
//
// 判定依据：范围报告 §3.10.2「触发与采集」——原方案只写了"采集协调"，
// 但 §3.4.2 明确把出光执行排除在门禁之外。缺口就是：
// **没有任何一层负责"让激光真的出一次光，并且让光谱仪真的采到这次光"**。
// 本文件补上这一段，且严格遵守原方案的三条纪律：
//
//   1. **门禁是唯一判定入口**。本类不自己判断"能不能出光"，
//      只调用 Gate::evaluate() 并服从它。任一条 GateReason 不满足就中止。
//   2. **触发方向已定：激光器触发光谱仪**（AvaSoft 手册 §4.9 接法二）。
//      激光器的 TTL 输出接到光谱仪 pin 6，光谱仪跑外触发模式等这个脉冲，
//      收到后再等 integration_delay 才开始积分。
//      因此本流程**不允许**设置光谱仪的 laser_width/laser_delay——
//      那两个字段是"光谱仪发 TTL 出去点火激光器"（接法一），方向反了会导致
//      激光永远不被点火、采集一直等触发直到超时。
//
//      **出光方式：连续出光，不用 0x34 单次触发。**（2026-09-29 按厂家工具
//      的实测日志订正。）协议文本把 0x34 写成"单次触发"，但厂家工具
//      整个日志历史里一次都没用过它；它跑的是
//          0x21=1 工作 -> 等设备进入「正在工作且放电」-> 0x24=1 光闸开
//          -> 保持若干发 -> 0x24=0 光闸关 -> 0x21=0 停机
//      本项目原先照协议文本围绕 0x34 设计整个流程，真机上一次都没出过光。
//      协议文本描述设备能做什么，厂家日志描述它实际被怎么用；冲突时以日志为准。
//   3. **失败必须中止到底**，不允许"部分成功"被当成出光成功。
//      出光是一个物理动作，Python 侧和日志里必须能区分"发了"和"成了"。
//   4. **绝不写机器配置。** 0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护是
//      整机标定值，存在设备 EEPROM 里被所有工具共享（厂家工具从不写它们）。
//      本项目写过一次 0x17=0V，后果是整机不出光、连厂家工具都点不着。
//   5. **绝不把 0x31 保存 EEPROM 当作出光流程的一步。** 每打一发就往激光器
//      EEPROM 写一次参数，任何一个写列表的错误都会被永久固化。保存是独立的、
//      由操作员显式发起的动作，不属于出光。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/business/config/site_config.hpp"
#include "libs/business/policy/gate.hpp"
#include "libs/error.hpp"
#include "libs/hex.hpp"
#include "libs/devices/laser.hpp"
#include "libs/devices/spectrometer.hpp"

namespace business::app {
// 前向声明：本服务由 Runtime 持有，Runtime 不能反过来 include 本文件。
class Runtime;
}

namespace business::services {

using ::business::app::Runtime;

// 一次出光要用的**全部**参数。
//
// 为什么不直接读 runtime_.config()：界面上能调的参数必须真的生效，
// 否则就又是一处"看着能调、实际不生效"—— 这个项目已经为此栽过一次
// （界面上的激光器参数写在业务参数库里，出光读的却是另一份站点配置）。
// 调用方负责把配置值与请求里的覆盖值合并好再交进来，本服务只管照单执行。
struct FireParameters {
    config::LaserFireConfig laser;      // 会话参数（调用方已按协议量程校验）
    double integration_ms = 10.0;       // 光谱仪积分时间
    unsigned capture_timeout_ms = 3000; // 等 pin6 触发的超时
};

// 一次出光流程的完整记录。**任何失败都要有记录**——现场排障靠它，
// 而不是靠"当时屏幕上显示了什么"。
struct FireRecord {
    bool ok = false;
    std::string failed_step;      // 失败在哪一步（空 = 全过）
    std::string error_code;       // libs::Error::code
    std::string error_message;    // 中文，可直接写日志

    // 实际下发的指令序列（十六进制帧），逐步记录。
    // 出光问题 90% 是"发出去的顺序不对"，没有这个就无从复盘。
    std::vector<std::string> sent_frames;

    // 最终读回的状态（0xB2），用于确认而不是猜测。
    bool confirmed_work_state = false;
    bool confirmed_q_gate_open = false;      // 收光后应为 false：读回 0=光闸关
    std::uint32_t discharge_counter_before = 0;   // 放电计数器起点
    std::uint32_t discharge_counter_after = 0;    // 出光后读回，用于确认真的放电过
    std::uint32_t discharges = 0;           // 本次实际放电次数（计数器增量）
    unsigned settle_ms = 0;                 // 从工作使能到"正在工作且放电"用了多久

    bool capture_attempted = false;         // 光谱仪是否参与了本次流程
    unsigned spectrum_groups = 0;           // 采到的光谱组数

    // 本次采到的**原始光谱**。必须一路带出来：能力 6.3.1 的本地交付出口
    // 要落盘的就是它。只报一个组数、把数据留在函数里，等于这一发的光谱
    // 只在流水线上活过一次，客户端不自己存就永久丢了。
    std::vector<libs::devices::ScanGroup> spectra;

    std::string note;                       // 中文小结，可直接写日志
};

// 一次**连续出光**的记录（对应 run_continuous / stop_continuous 这一对）。
struct RunRecord {
    bool ok = false;
    std::string failed_step;
    std::string error_code;
    std::string error_message;
    std::vector<std::string> sent_frames;   // 逐步留证，出问题靠它复盘
    unsigned settle_ms = 0;                 // 从工作使能到"正在工作且放电"
    std::uint32_t discharge_counter_at_start = 0;
    std::string note;
};

// 激光出光服务。
class LaserFireService {
public:
    explicit LaserFireService(Runtime& runtime) : runtime_(runtime) {}

    // 执行一次完整的"出光 -> 采集"流程。
    //
    // 流程（任一步失败立即中止，不继续往下发指令）：
    //   1. 设备在位
    //   2. 门禁判定（含"出光参数是否已标定"）——不允许就中止，**一条写指令都不发**
    //   3. 读 B2 拿基线（含放电计数器起点）
    //   4. 写会话参数（电流/脉宽/频率/放电延时/Q 延时/Q 预热），值来自配置
    //   5. 0x21 工作状态 = 1
    //   6. 轮询 B2 直到设备进入「正在工作且放电」（自适应过程不是常数时长）
    //   7. 光谱仪后台线程 arm 到"等 pin 6 的 TTL"
    //   8. 0x24 光闸 = 1，保持 emission_hold_ms 的连续出光窗口
    //   9. 0x24 光闸 = 0 收光、0x21 工作状态 = 0 停机（无条件执行）
    //  10. 读 B2 确认：计数器确实涨够、光闸确实关、工作确实停
    //
    // arm 在第 7 步而不是第 1 步：光谱仪等 TTL 有超时，而设备从使能到放电
    // 要 5~6 秒。早 arm 会让采集超时在激光出光之前就到期（真机踩过）。
    //
    // 机器配置（0x17/0x18/0x15）与 0x31 保存 EEPROM 都**不在本流程内**。
    libs::Result<FireRecord> fire_and_capture();
    // 同上，但用**显式给出的参数**（配置值 + 请求覆盖值合并后的结果）。
    libs::Result<FireRecord> fire_and_capture(const FireParameters& params);

    // ── 连续出光 ────────────────────────────────────────────────────────────
    //
    // 与 fire_and_capture 的区别：
    //   fire_and_capture = **有界测量**：出光窗口 + 采集 + 落盘，一次一发。
    //   run_continuous   = **一直打**：门禁 → 写参数 → 工作使能 → 等设备真的
    //                      放电 → 开光闸，然后**返回**。激光按设定频率持续
    //                      出光，直到 stop_continuous()。
    //
    // 为什么不做成"把 emission_hold_ms 设得很大"：那样一次同步调用会挂住
    // 十几分钟，期间停不下来、还一直占着 device_mutex。开始/停止必须分开。
    libs::Result<RunRecord> run_continuous(const FireParameters& params);

    // 停止连续出光：光闸关 → 停机 → **读回确认**。
    //
    // 这个函数必须随时可调用、且无条件执行：哪怕 run_continuous 从没成功过、
    // 哪怕中途哪一步失败，调用它都要尽力把光闸关掉。守护进程退出时也靠它收光 ——
    // **关串口本身不会让激光停下来**，它自己保持着工作状态。
    libs::VoidResult stop_continuous();

    // 只检查门禁，不发任何指令。用于 UI 预检。
    libs::VoidResult precheck();

    // 覆盖本次要用的激光器设备。**仅供离线测试注入替身**：
    // 正常路径一律用 Runtime 持有的真实设备，构造后不调用本方法。
    // 存在的原因见 tests/test_laser_fire.cpp：DeviceSession 按配置独占打开，
    // 没有注入点，端到端就只能靠真实串口，那不是测试。
    void set_laser_for_test(libs::devices::Laser* laser) noexcept { laser_override_ = laser; }

private:
    // 发一条指令并记录到 record。失败时把错误原样上抛。
    libs::VoidResult step(libs::devices::Laser* laser,
                          std::uint8_t command,
                          const libs::Bytes& payload,
                          FireRecord& record,
                          const char* step_name);

    // 读一次 B2 并解析安全位。
    libs::Result<policy::LaserSafetyFacts> read_facts(libs::devices::Laser* laser, FireRecord& record);

    Runtime& runtime_;
    libs::devices::Laser* laser_override_ = nullptr;   // 仅测试注入用

    libs::devices::Laser* laser() const;
};

}  // namespace business::services
