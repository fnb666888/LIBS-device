#include "machine_state.hpp"

#include <QJsonArray>

namespace libs::ui {
namespace {

QString text_of(const QJsonObject& o, const char* key) {
    return o.value(QLatin1String(key)).toString();
}

bool bool_of(const QJsonObject& o, const char* key, bool fallback = false) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) return v.toBool();
    if (v.isString()) return v.toString() == QLatin1String("true");
    return fallback;
}

DeviceHealth parse_device(const QString& key, const QString& name, const QJsonObject& d) {
    DeviceHealth h;
    h.key = key;
    h.name = name;
    h.configured = bool_of(d, "configured");
    h.opened = bool_of(d, "opened");
    h.endpoint = text_of(d, "endpoint");
    h.note = text_of(d, "note");
    return h;
}

}  // namespace

const DeviceHealth* MachineSnapshot::device(const QString& key) const {
    for (const DeviceHealth& d : devices) {
        if (d.key == key) return &d;
    }
    return nullptr;
}

MachineSnapshot parse_status(const QJsonObject& p) {
    MachineSnapshot s;
    s.valid = true;
    s.wall_clock_ms = static_cast<qint64>(p.value("wall_clock_ms").toDouble());
    s.running = bool_of(p, "running");
    s.log_records = static_cast<qint64>(p.value("log_records").toDouble());

    // ── health ────────────────────────────────────────────────────────
    //
    // **只有这三台。** DeviceSession::health() 只账户 laser / indicator /
    // spectrometer；测距仪与空调已随重构删除，界面上不再给它们留位置 ——
    // 留一个永远"未配置"的状态点，只会让现场以为那台机器坏了。
    const QJsonObject h = p.value("health").toObject();
    s.devices.reserve(3);
    s.devices.push_back(parse_device(QStringLiteral("laser"), QStringLiteral("激光器"),
                                     h.value("laser").toObject()));
    s.devices.push_back(parse_device(QStringLiteral("indicator"), QStringLiteral("状态灯"),
                                     h.value("indicator").toObject()));
    s.devices.push_back(parse_device(QStringLiteral("spectrometer"), QStringLiteral("光谱仪"),
                                     h.value("spectrometer").toObject()));

    // ── gate ──────────────────────────────────────────────────────────
    const QJsonObject g = p.value("gate").toObject();
    if (!g.isEmpty()) {
        s.gate.known = true;
        s.gate.state = text_of(g, "state");
        s.gate.armed = bool_of(g, "armed");
        s.gate.allowed = bool_of(g, "allowed");
        s.gate.laser_readable = bool_of(g, "laser_readable");
        s.gate.detail = text_of(g, "detail");
        const QJsonArray blocked = g.value("blocked_by").toArray();
        for (const QJsonValue& v : blocked) s.gate.blocked_by.push_back(v.toString());
    }

    s.laser_running = bool_of(p, "laser_running");
    return s;
}

void merge_params(MachineSnapshot& s, const QJsonObject& payload) {
    const QJsonObject readiness = payload.value("readiness").toObject();
    if (!readiness.isEmpty()) {
        s.readiness_known = true;
        s.readiness_ready = readiness.value("ready").toBool();
        s.readiness_summary = readiness.value("summary").toString();
    }
    // groups 形态：{"laser":{"values":{...},"complete":bool}, ...}
    const QJsonObject groups = payload.value("groups").toObject();
    if (!groups.isEmpty()) s.param_values = groups;
}

void merge_measurement(MachineSnapshot& s, const QJsonObject& p) {
    // available=false 表示守护进程还没装配完测量作业。
    // 这不是故障，也不是"没计数" —— 就是**未知**。
    if (!p.value("available").toBool(false)) {
        s.measurement_known = false;
        return;
    }
    s.measurement_known = true;
    s.measurement_running = bool_of(p, "running");
    s.shots_captured = static_cast<qint64>(p.value("shots_captured").toDouble(0));
    s.shots_failed = static_cast<qint64>(p.value("shots_failed").toDouble(0));
    s.disk_writes_ok = static_cast<qint64>(p.value("disk_writes_ok").toDouble(0));
    s.disk_writes_failed = static_cast<qint64>(p.value("disk_writes_failed").toDouble(0));
    s.disk_backlog = static_cast<qint64>(p.value("disk_backlog").toDouble(0));
    s.measurement_error_code = text_of(p, "last_error_code");
    s.measurement_error_message = text_of(p, "last_error_message");
}

}  // namespace libs::ui

