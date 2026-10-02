#pragma once

// 站点配置与端口映射（能力 1）。
//
// 判定依据：范围报告 §3.1，三门全过 = A 类。
//
// 核心纪律：
//   1. 端口、波特率、超时、设备使能全部来自配置文件，**不硬编码进 src/**。
//      任务书 §2.2 与既有方案 §6.1 的端口表不一致时，改配置而不是改代码。
//   2. 配置缺失或字段非法一律返回 ErrorKind::invalid_argument，
//      **禁止静默回落默认值**——回落默认值意味着会开错设备、连错口。
//   3. "可加载、非硬编码"是本项的验收点：`config validate` 不开任何串口。

#include <cstdint>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/error.hpp"
#include "libs/transport/serial.hpp"

namespace business::config {

// 一个串口设备的站点配置。设备类型不写死在这里，由上层 session 决定用途。
struct SerialPortConfig {
    std::string port;                       // 如 COM10
    unsigned baud = 9600;
    libs::transport::Parity parity = libs::transport::Parity::none;
    unsigned data_bits = 8;
    unsigned timeout_ms = 650;
    unsigned idle_ms = 80;

    // false 表示本次运行不接这台设备。默认 true：配置里写了就是要用。
    bool enabled = true;
};

// 光谱仪配置。通道数量与序列号都来自配置，USB 口没有"端口名"概念。
struct SpectrometerConfig {
    std::string sdk_dll;                    // 绝对 UTF-8 路径的 avaspecx64.dll
    std::vector<std::string> serials;       // 为空表示"当前可用的全部设备"
    std::string master_serial;              // 可选：外触发源 / 出光设备
    bool synchronized = false;              // 多通道 SYNC；同步效果未验证，见范围报告 §3.10.1
    bool enabled = true;

    // ── 激光触发光谱仪（AvaSoft 手册 §4.9 的第二种接法）──────────────────
    //
    // true = 光谱仪进外触发模式，从 pin 6 等激光器的 TTL 输入，
    //        收到脉冲后延迟 integration_delay_ns 再开始积分。
    // false = 自由触发，采集随时可开始（默认，不等任何外部信号）。
    //
    // 置 true 之后没有外触发信号就永远等不到数据，采集会超时——
    // 这是物理事实，不是缺陷。现场接线必须确认激光器 TTL 确实接到 pin 6。
    bool wait_for_laser_trigger = false;

    // 收到激光触发后，延迟多久开始积分。**单位纳秒。**
    // 0 = 立刻积分。LIBS 场景一般要几十到几百微秒，
    // 等等离子体冷却后的自发光，而不是激光本身的散射。
    //
    // 手册 §4.9 给出的下限（激光控制时序时）：
    // AvaSpec-3648-USB2 最小 0.28 us，AvaSpec-2048-USB2 最小 1.28 us。
    double integration_delay_ns = 0.0;
};

// 网关/海关的候选地址。范围报告 §3.5.2：两个候选值都进配置，**不在代码里二选一**。
struct GatewayConfig {
    std::string candidate_ip;               // 候选 IPC 地址，如 192.168.101.100
    std::string candidate_adapter;          // 候选网卡名，如 "以太网2"
    std::string observed_ip;                // 快照中观察到的地址，如 192.168.100.100
    unsigned port = 0;                      // 0 = 协议未定，不猜
    bool enabled = false;                   // 无网络传输层，默认关闭（范围报告 §3.5.2 G2）

    // 海关智能设备接入管理边端的出站上报配置。附件文档中的 IP、token
    // 只是示例，绝不写死在代码里；enabled=false 时这些字段可以为空。
    std::string base_url;                   // 例如 http://edge-host:port
    std::string product_id;                 // 边端下发的产品 ID
    std::string device_id;                  // 边端下发的设备 ID
    std::string authorization_token;        // Bearer token；config_show 会脱敏
    std::string equipment_id;               // HEARTBEAT/EVENT/OPENINIT 使用
    std::string equipment_type;             // HEARTBEAT/EVENT/OPENINIT 使用
    std::string customs;                    // 所属直属海关，可选
    unsigned timeout_ms = 5000;             // 预留给 HTTP 客户端的请求超时
};

// 传送带是否由本软件控制（范围报告 §2.3：需要一次确认，暂列假设）。
struct ConveyorConfig {
    bool software_controlled = false;       // 未确认前一律 false
    bool running_assumed = true;            // 业务层只做"运行中"状态前提假设
};

// 空调联锁是否启用（范围报告 §3.9.2：阈值未定，未确认前不启用）。
struct AirConditionerInterlockConfig {
    bool enabled = false;                   // 未确认前一律 false
    double trigger_indoor_temp_c = 0.0;     // 0 = 未标定，不猜
    bool calibrated = false;
};

// ── 激光出光的标定参数（能力 10 的执行侧）────────────────────────────────────
//
// 为什么必须来自配置、且必须带 commissioned 门闩：
//
// 这些数值过去是硬编码在 laser_fire.cpp 里的"协议量程下限"。对**限制类**
// 参数，量程下限意味着"禁止工作"而不是"最小能量"，实测后果是整台设备不出光：
//   0x17 电压限值取 0 V   -> 输出钳死在 0，设备一使能就报 bit5 过压故障
//   0x16 Q 延时取 2 us    -> 等于不调 Q，没有巨脉冲
//   0x11 放电电流取 15 A  -> 泵浦能量只有正常的五分之一，低于出等离子体的阈值
// 可用工作点只能来自厂家标定或现场工艺，因此一律来自配置，不硬编码。
//
// **机器配置类参数刻意不在本组**：0x17 电压限值、0x18 Q 分频、0x15 温度保护
// 是整机标定值，存在设备 EEPROM 里被所有工具共享（厂家工具从不写它们）。
// 本程序对这些参数**只读不写**——写进去会从别的工具脚下抽掉地板，
// 现场真实发生过一次，连厂家工具都点不着了。
struct LaserFireConfig {
    // false = 本机出光参数未经标定。此时拒绝出光，**一条写指令都不发**。
    // 与 ac_interlock.calibrated 同一套纪律：未标定不得启用。
    bool commissioned = false;

    unsigned discharge_current_a = 0;       // 0x11  协议量程 15~100 A
    unsigned pulse_width_us = 0;            // 0x12  协议量程 50~300 us
    unsigned frequency_hz = 0;              // 0x13  协议量程 1~10 Hz
    unsigned discharge_delay_us = 0;        // 0x14  协议量程 0~500 us
    unsigned q_delay_us = 0;                // 0x16  协议量程 2~500 us
    unsigned q_warmup_s = 0;                // 0x19  未标量程

    // 反向联合触发使用的 0x22 模式：1=外时钟/内 Q，2=外时钟/外 Q。
    // 激光器触发光谱仪时不下发此字段；保留默认 1 只为反向模式提供安全候选值。
    unsigned laser_clock_mode = 1;

    // 设备使能后要从 02「自适应中」走到 01「正在工作且放电」才开始出光。
    // 厂家日志里这个过渡约 5~6 秒且不是常数，所以只能轮询状态位、不能固定 sleep。
    unsigned settle_timeout_ms = 20000;

    // 光闸保持打开的时间窗口 = 连续出光的时长。按设定频率算够打的发数。
    unsigned emission_hold_ms = 3000;

    // 至少要有这么多次放电才算"真的出光了"。计数器不涨 = 没放电 = 失败。
    unsigned min_discharges = 1;
};

struct SiteConfig {
    std::string station_id;                 // 站号

    SerialPortConfig laser;                 // 端口表：COM10
    SerialPortConfig rangefinder;           // 端口表：COM7
    SerialPortConfig indicator;             // 端口表：COM8
    SerialPortConfig air_conditioner;       // 端口表：COM12
    SpectrometerConfig spectrometer;        // 4×光谱 USB
    LaserFireConfig laser_fire;             // 出光标定参数（含 commissioned 门闩）
    GatewayConfig gateway;
    ConveyorConfig conveyor;
    AirConditionerInterlockConfig ac_interlock;

    // 状态灯型号：写命令需要型号才能构造（科隆威 K50 等）。留空则不可写。
    std::string indicator_model;
    std::uint8_t indicator_address = 1;

    // 设备地址
    std::uint8_t laser_address = 1;
    std::uint8_t rangefinder_address = 1;
    std::uint8_t air_conditioner_address = 1;

    // 采集与联动的默认超时
    unsigned capture_timeout_ms = 3000;
    // 光谱仪积分时间（ms）。**浮点**：厂家 LIBS 预设用的是 1.05 ms，
    // 整数会把它截成 1 ms，窄窗就白设了。
    double integration_ms = 10.0;
};

// 从 JSON 文本解析。任一字段非法都返回 invalid_argument，不回落默认值。
libs::Result<SiteConfig> from_json(const json::Value& value);

// 序列化为 JSON。用于 `config validate` 的回显与远端查询。
json::Value to_json(const SiteConfig& config);

// 与 to_json 相同，但不回显网关 Bearer token。用于 config_show、状态和日志。
json::Value to_json_redacted(const SiteConfig& config);

// 加载并解析一个配置文件（UTF-8）。
//   * 文件不存在 -> ErrorKind::invalid_argument（不是 transport：这是配置问题）
//   * JSON 语法错 -> ErrorKind::invalid_argument
//   * 字段缺失/非法 -> ErrorKind::invalid_argument
libs::Result<SiteConfig> load_file(const std::string& path);

// 生成一份「全部设备未接」的骨架。
//
// 为什么不直接导出 SiteConfig{}：空 SiteConfig 的所有设备
// enabled=true（见上方默认值说明），但没串口、没 SDK 路径，
// validate() 会正确地拒绝它。那个拒绝是对的（禁止静默回落），
// 但“打印默认配置”必须给出一份能通过校验的文本。
// 这个函数就是那份文本。
libs::Result<SiteConfig> default_template();

// 序列化到字符串，供落盘使用。
libs::Result<std::string> save_string(const SiteConfig& config);

// 全量校验。load 成功即代表基础校验通过；本函数额外检查跨字段一致性
// （例如使能的设备必须有端口、采集参数必须为正）。
libs::VoidResult validate(const SiteConfig& config);

// 单独校验出光参数（协议量程）。
//
// 抽出来是因为它有两个调用点：站点配置加载时，以及**出光请求带覆盖参数时**。
// 界面上调了参数就必须走同一套量程校验，否则"界面上限 100 A、实际能传 500 A"
// 这种破口会在两个地方各开一次。
// 未标定（commissioned=false）时不校验 —— 那些数根本不会被下发。
libs::VoidResult validate_laser_fire(const LaserFireConfig& laser_fire);

}  // namespace business::config

