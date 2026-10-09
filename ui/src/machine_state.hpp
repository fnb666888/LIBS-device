#pragma once

// 机器状态快照（`ui/src/machine_state.hpp`）。
//
// 这是 UI 侧对 `status` / `param_get` / `measurement_status` 三个命令返回的
// **只读投影**。要点：
//
//   1. **三态不是二态。** 每个可能"拿不到"的量都带一个
//      `xxx_known` 布尔。"通信失败"绝不被压成 0 / false / 空串 ——
//      那是现场排障时最贵的一种撒谎。
//   2. **不推导业务结论。** 门禁能不能出光由 `gate.allowed` 说了算，
//      UI 只把 allowed/blocked_by 原样翻译成人话，不自己判断"应该可以"。
//   3. **不缓存到磁盘。** 断电重开要重新问守护进程，因为现场参数可能
//      在别的终端改过。
//   4. **只投影后端真的返回的字段。** 空调、测距仪、传送带、选矿这些
//      业务已随重构全部删除，`DeviceSession::health()` 现在只账户
//      laser / indicator / spectrometer 三台。曾经这里留着
//      `AcState` / `OreState` / `MotionState` 和一串 belt 字段，
//      它们解析的 JSON 键后端早就没有了 —— 于是界面上常年挂着一排
//      "空调 未配置 / 测距 未配置"的假状态点，而真正的三台设备挤在里面。
//      这就是"UI 还停留在旧架构"的典型症状。

#include <optional>

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace libs::ui {

// 一台设备在 `health` 里的状态。
struct DeviceHealth {
    QString name;        // 展示用名，如"激光器"
    QString key;         // 后端键名，如 "laser"
    bool configured = false;
    bool opened = false;
    QString endpoint;    // "COM10" / "avaspecx64.dll: SN1234"
    QString note;        // 失败原因 / 补充说明

    // 三态判定（UI 底部设备条直接用，别自己再判一遍）：
    //   configured=false      -> 未配置，**正常**，灰
    //   configured=true 开    -> 已打开，**正常**，绿
    //   configured=true 没开  -> 打开失败，**真故障**，红
    bool ok() const { return !configured || opened; }
    bool relevant() const { return configured; }
};

// 门禁（能不能出光的唯一判据）。
struct GateState {
    bool known = false;          // status 这一轮没取到时为 false
    QString state;               // standby / ready / acquiring / fault
    bool armed = false;
    bool allowed = false;        // **唯一的许可来源**
    bool laser_readable = false; // false = 没读到激光状态，按不安全处理
    QString detail;              // 业务层给的中文说明
    QStringList blocked_by;      // blocked_by 原因码
};

// 整次状态快照。
struct MachineSnapshot {
    bool valid = false;           // 是否成功取到过 status
    qint64 wall_clock_ms = 0;
    bool running = false;         // 守护进程是否在跑
    qint64 log_records = 0;

    QVector<DeviceHealth> devices;
    GateState gate;

    // 出光会话是否在进行（status.laser_running）。
    // **这是按钮可用性的唯一来源**：界面不靠"我刚才点过开始所以应该在跑"
    // 这种本地推理去猜。
    bool laser_running = false;

    // 测量作业的计数。**它才是"落盘成功了没有"的唯一凭证**：
    // 曾经有过那种"屏幕上有曲线 = 数据保存了"的情况。显示是内存里的事，
    // 落盘是另一条线程的事实 —— 两者可以不一致，屏幕必须如实分开说。
    bool measurement_known = false;
    bool measurement_running = false;
    qint64 shots_captured = 0;
    qint64 shots_failed = 0;
    qint64 disk_writes_ok = 0;
    qint64 disk_writes_failed = 0;
    qint64 disk_backlog = 0;
    QString measurement_error_code;
    QString measurement_error_message;

    // 参数齐备（`param_get` 的 readiness）。门禁的 parameters_incomplete
    // 就来自它，所以必须一起取，否则 UI 只能说"参数不齐"却列不出缺什么。
    bool readiness_known = false;
    bool readiness_ready = false;
    QString readiness_summary;
    // {"laser": {"values": {...}, "complete": bool}, ...}
    QJsonObject param_values;
    QJsonObject prediction;  // Asynchronous model state and correlated result.

    // 一次 status 往返的错误（守护进程没起 / 管道错）。有值时 valid=false，
    // 但**上面那些字段仍保留上一帧的好数据**，UI 据此显示"数据已过期"。
    QString error;

    const DeviceHealth* device(const QString& key) const;
};

// 从 `status` 的 payload 解析。缺字段一律走"未知"，不抛异常。
MachineSnapshot parse_status(const QJsonObject& payload);

// 从 `param_get` 的 payload 解析（并入已有快照的 param_* 字段）。
void merge_params(MachineSnapshot& snapshot, const QJsonObject& payload);

// 从 `measurement_status` 的 payload 并入测量计数。
void merge_measurement(MachineSnapshot& snapshot, const QJsonObject& payload);

}  // namespace libs::ui
