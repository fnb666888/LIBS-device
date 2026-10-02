#pragma once

// 机器状态快照（`ui/src/machine_state.hpp`）。
//
// 这是 UI 侧对 `status` / `health` / `gate` / `param_get` 四个命令返回的
// **只读投影**。要点：
//
//   1. **三态不是二态。** 每个可能"拿不到"的量都带一个
//      `xxx_known` 布尔（以及 sometimes 一个原因串）。"通信失败"绝不
//      被压成 0 / false / 空串——那是现场排障时最贵的一种撒谎。
//   2. **不推导业务结论。** 门禁能不能出光由 `gate.allowed` 说了算，
//      UI 只把 allowed/blocked_by 原样翻译成人话，不自己判断"应该可以"。
//   3. **不缓存到磁盘。** 断电重开要重新问守护进程，因为现场参数可能
//      在别的终端改过。

#include <optional>

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace libs::ui {

// 一台设备在 `health` 里的状态。
struct DeviceHealth {
    QString name;        // 展示用名，如"激光器"
    bool configured = false;
    bool opened = false;
    QString endpoint;    // "COM10" / "avaspecx64.dll: SN1234"
    QString note;        // 失败原因 / 补充说明

    // configured=false 是"这台机器本来就不接它"，属于正常；
    // configured=true && opened=false 才是真故障。两者的级别不同。
    bool ok() const { return !configured || opened; }
    bool relevant() const { return configured; }
};

// 门禁（能不能出光的唯一判据）。
struct GateState {
    QString state;            // standby / ready / acquiring / fault
    bool armed = false;
    bool allowed = false;     // **唯一的许可来源**
    QString detail;           // 业务层给的中文说明
    QStringList blocked_by;   // blocked_by 原因码
    bool parameters_ready = false;
    bool laser_enabled = false;
    bool ac_interlock_ok = false;
};

// 空调快照。
struct AcState {
    QString availability;     // available / stale / unavailable
    bool known = false;
    std::optional<double> indoor_temp_c;
    std::optional<double> evaporator_temp_c;
    std::optional<double> condenser_temp_c;
    std::optional<double> humidity_rh;
    QString mode;              // cooling / heating / ...，可能为空
    QString read_error;
    QStringList active_alarms;
    QString note;
};

// 测距 / 有矿判定。
struct OreState {
    bool decision_known = false;   // ore_present 是不是有结论
    std::optional<bool> ore_present;   // null = 未标定/未观测 = 未知
    QString basis;
    bool has_observation = false;
    std::optional<double> distance_raw;
    std::optional<int> distance_register;
    std::optional<double> distance_resolution_mm;
    std::optional<double> confidence_raw;
    bool device_valid = false;
    bool in_range = false;
    bool read_ok = false;
    QString read_error;
    QString note;
    // 后端按辅助读取节流周期报告的年龄。-1 表示后端还没有完成过一次读取。
    qint64 read_age_ms = -1;
};

// 运动标定状态。
struct MotionState {
    bool calibrated = false;
    std::optional<double> forward_time_s;
    std::optional<double> reverse_time_s;
    QString note;
};

// 整次状态快照。
struct MachineSnapshot {
    bool valid = false;           // 是否成功取到过 status
    qint64 wall_clock_ms = 0;
    QString running;              // "true"/"false" 文本，守护进程给什么记什么
    QString stage;                // idle / selected_belt / measuring / ...
    QString target_belt;
    int belt_sequence = -1;
    qint64 log_records = 0;
    QVector<DeviceHealth> devices;
    GateState gate;
    // 连续出光是否在跑（status.laser_running）。
    // 界面靠它决定「连续出光」与「停止出光」哪个可点 —— 这个状态由守护进程
    // 说了算，UI 不自己推测"我刚才点过开始所以应该在跑"。
    bool laser_running = false;
    AcState ac;
    OreState ore;
    MotionState motion;
    bool ac_interlock_participates = false;

    // 参数齐备（`param_get` 的 readiness）。门禁的 parameters_incomplete
    // 就来自它，所以必须一起取，否则 UI 只能说"参数不齐"却列不出缺什么。
    bool readiness_known = false;
    bool readiness_ready = false;
    QString readiness_summary;
    QStringList readiness_missing;
    QJsonObject param_values;     // {"laser": {...}, "spectrometer": {...}}

    // 一次 status 往返的错误（守护进程没起 / 串口错）。有值时 valid=false。
    QString error;

    const DeviceHealth* device(const QString& name) const;
};

// 从 `status` 的 payload 解析。缺字段一律走"未知"，不抛异常。
MachineSnapshot parse_status(const QJsonObject& payload);

// 从 `param_get` 的 payload 解析（并入已有快照的 param_* 字段）。
void merge_params(MachineSnapshot& snapshot, const QJsonObject& payload);

}  // namespace libs::ui

