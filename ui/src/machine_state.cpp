#include "machine_state.hpp"

#include <QJsonArray>

namespace libs::ui {
namespace {

// QJsonValue 是 null 时 toDouble() 给 0。**必须显式区分**：
// 守护进程用 null 表示"这次没测到"，把它读成 0.0 等于凭空造一个读数。
std::optional<double> opt_double(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isNull() || !v.isDouble()) return std::nullopt;
    return v.toDouble();
}

QString text_of(const QJsonObject& o, const char* key) {
    return o.value(QLatin1String(key)).toString();
}

bool bool_of(const QJsonObject& o, const char* key, bool fallback = false) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool()) return v.toBool();
    if (v.isString()) return v.toString() == QLatin1String("true");
    return fallback;
}

DeviceHealth parse_device(const QString& name, const QJsonObject& d) {
    DeviceHealth h;
    h.name = name;
    h.configured = bool_of(d, "configured");
    h.opened = bool_of(d, "opened");
    h.endpoint = text_of(d, "endpoint");
    h.note = text_of(d, "note");
    return h;
}

}  // namespace

const DeviceHealth* MachineSnapshot::device(const QString& name) const {
    for (const DeviceHealth& d : devices) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

MachineSnapshot parse_status(const QJsonObject& p) {
    MachineSnapshot s;
    s.valid = true;
    s.wall_clock_ms = static_cast<qint64>(p.value("wall_clock_ms").toDouble());
    // running 是布尔；守护进程给 bool 时直接读，否则记原始文本。
    if (p.value("running").isBool()) {
        s.running = p.value("running").toBool() ? QStringLiteral("true") : QStringLiteral("false");
    } else {
        s.running = p.value("running").toString();
    }
    s.stage = text_of(p, "stage");
    s.target_belt = text_of(p, "target_belt");
    s.belt_sequence = p.value("belt_sequence").toInt(-1);
    s.log_records = static_cast<qint64>(p.value("log_records").toDouble());

    // ── health ────────────────────────────────────────────────────────
    const QJsonObject h = p.value("health").toObject();
    s.devices.reserve(5);
    s.devices.push_back(parse_device(QStringLiteral("激光器"), h.value("laser").toObject()));
    s.devices.push_back(parse_device(QStringLiteral("光谱仪"), h.value("spectrometer").toObject()));
    s.devices.push_back(parse_device(QStringLiteral("测距仪"), h.value("rangefinder").toObject()));
    s.devices.push_back(parse_device(QStringLiteral("状态灯"), h.value("indicator").toObject()));
    s.devices.push_back(parse_device(QStringLiteral("空调"), h.value("air_conditioner").toObject()));

    // ── gate ──────────────────────────────────────────────────────────
    const QJsonObject g = p.value("gate").toObject();
    s.gate.state = text_of(g, "state");
    s.gate.armed = bool_of(g, "armed");
    s.gate.allowed = bool_of(g, "allowed");
    s.gate.detail = text_of(g, "detail");
    s.gate.parameters_ready = bool_of(g, "parameters_ready");
    s.gate.laser_enabled = bool_of(g, "laser_enabled");
    s.gate.ac_interlock_ok = bool_of(g, "ac_interlock_ok");
    const QJsonArray blocked = g.value("blocked_by").toArray();
    for (const QJsonValue& v : blocked) s.gate.blocked_by.push_back(v.toString());

    s.laser_running = bool_of(p, "laser_running");

    // ── air conditioner ───────────────────────────────────────────────
    const QJsonObject ac = p.value("air_conditioner").toObject();
    s.ac.availability = text_of(ac, "availability");
    s.ac.known = (s.ac.availability == QLatin1String("available"));
    s.ac.indoor_temp_c = opt_double(ac, "indoor_temp_c");
    s.ac.evaporator_temp_c = opt_double(ac, "evaporator_temp_c");
    s.ac.condenser_temp_c = opt_double(ac, "condenser_temp_c");
    s.ac.humidity_rh = opt_double(ac, "humidity_rh");
    s.ac.mode = text_of(ac, "mode");
    s.ac.read_error = text_of(ac, "read_error");
    s.ac.note = text_of(ac, "note");
    const QJsonArray alarms = ac.value("active_alarms").toArray();
    for (const QJsonValue& v : alarms) {
        s.ac.active_alarms.push_back(v.toObject().value("name").toString());
    }
    s.ac_interlock_participates = bool_of(p, "ac_interlock_participates");

    // ── ore / rangefinder ─────────────────────────────────────────────
    const QJsonObject ore = p.value("ore").toObject();
    if (ore.contains("ore_present")) {
        if (ore.value("ore_present").isBool()) {
            s.ore.ore_present = ore.value("ore_present").toBool();
            s.ore.decision_known = true;
        }
    }
    s.ore.basis = text_of(ore, "basis");
    s.ore.has_observation = bool_of(ore, "has_observation");
    const QJsonObject params = ore.value("parameters_snapshot").toObject();
    s.ore.note = text_of(params, "basis");

    // ── motion ────────────────────────────────────────────────────────
    const QJsonObject m = p.value("motion").toObject();
    s.motion.calibrated = bool_of(m, "calibrated");
    s.motion.forward_time_s = opt_double(m, "forward_time_s");
    s.motion.reverse_time_s = opt_double(m, "reverse_time_s");
    s.motion.note = text_of(m, "note");

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

}  // namespace libs::ui
