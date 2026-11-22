#include "device_status_page.hpp"

#include <QJsonArray>
#include <algorithm>
#include <limits>

namespace libs::ui {
namespace {

QString value_text(const QJsonObject& object, const char* key, const QString& unit = {}) {
    const auto value = object.value(QLatin1String(key));
    return value.isDouble() ? QString::number(value.toDouble()) + unit : QStringLiteral("—");
}

DevicePresentation present_motor(const MachineSnapshot& snapshot) {
    const auto& data = snapshot.motor;
    const auto* health = snapshot.device(QStringLiteral("motor"));
    if (!data.value("configured").toBool(health && health->configured))
        return {Level::Idle, QStringLiteral("未启用"), {}, {}};
    const auto detail = data.value("error").toString();
    if (!data.value("opened").toBool(health && health->opened))
        return {Level::Danger, QStringLiteral("未连接"), {}, detail};
    const bool forward = data.value("forward_limit").toBool();
    const bool reverse = data.value("reverse_limit").toBool();
    const auto limit = forward && reverse ? QStringLiteral("双向触发") : forward ? QStringLiteral("正向触发")
        : reverse ? QStringLiteral("反向触发") : data.value("forward_limit").isBool() && data.value("reverse_limit").isBool()
        ? QStringLiteral("未触发") : QStringLiteral("—");
    QStringList readings{QStringLiteral("输出频率  %1").arg(value_text(data, "frequency_hz", QStringLiteral(" Hz"))),
                         QStringLiteral("行程限位  %1").arg(limit)};
    if (data.value("input_word").isDouble()) {
        const int inputs = data.value("input_word").toInt();
        readings << QStringLiteral("DI0 %1 / DI1 %2 / DI2 %3 / DI3 %4")
            .arg(inputs & 1).arg((inputs >> 1) & 1).arg((inputs >> 2) & 1).arg((inputs >> 3) & 1);
    }
    const auto direction = data.value("direction").toString();
    if (direction == QLatin1String("forward")) readings << QStringLiteral("运行方向  正向");
    if (direction == QLatin1String("reverse")) readings << QStringLiteral("运行方向  反向");
    const auto jog_error = data.value("jog_error").toString();
    if (!jog_error.isEmpty()) readings << jog_error;
    const int fault = data.value("fault_code").toInt();
    if (fault) {
        readings << QStringLiteral("故障码  %1").arg(fault);
        return {Level::Danger, QStringLiteral("变频器故障"), readings, detail};
    }
    if (data.value("stop_pending").toBool()) return {Level::Warn, QStringLiteral("等待停机回读"), readings, detail};
    if (data.value("jog_requested").toBool())
        return {Level::Warn, data.value("jogging").toBool() ? QStringLiteral("点动中") :
            QStringLiteral("等待点动回读"), readings, detail};
    if (!data.value("stopped").toBool()) return {Level::Warn, QStringLiteral("停机未确认"), readings, detail};
    if (!jog_error.isEmpty()) return {Level::Warn, QStringLiteral("点动结束，已停机"), readings, jog_error};
    if (forward || reverse) return {Level::Warn, QStringLiteral("限位触发"), readings, detail};
    return {Level::Ok, QStringLiteral("停机已回读"), readings, detail};
}

QStringList spectrum_readings(const QJsonArray& channels) {
    if (channels.isEmpty()) return {QStringLiteral("可用通道  —")};
    int count = 0;
    double lowest = std::numeric_limits<double>::infinity();
    double highest = -lowest;
    for (const auto& value : channels) {
        const auto channel = value.toObject();
        if (!channel.value("pixels").isDouble()) continue;
        ++count;
        if (channel.value("wavelength_min_nm").isDouble())
            lowest = std::min(lowest, channel.value("wavelength_min_nm").toDouble());
        if (channel.value("wavelength_max_nm").isDouble())
            highest = std::max(highest, channel.value("wavelength_max_nm").toDouble());
    }
    QStringList result{QStringLiteral("可用通道  %1 路").arg(count)};
    if (lowest <= highest) result << QStringLiteral("波段范围  %1–%2 nm").arg(lowest, 0, 'f', 1).arg(highest, 0, 'f', 1);
    return result;
}

}  // namespace

QString air_mode_name(int code) {
    switch (code) {
        case 0: return QStringLiteral("待机／送风");
        case 1: return QStringLiteral("制冷");
        case 2: return QStringLiteral("加热");
        case 3: return QStringLiteral("除湿");
        default: return QStringLiteral("未知");
    }
}

DevicePresentation present_device(const QString& key, const MachineSnapshot& snapshot,
                                  const QJsonObject& readings, const QString& error) {
    if (!snapshot.valid || !snapshot.error.isEmpty())
        return {Level::Idle, QStringLiteral("状态未知"), {}, snapshot.error};
    if (key == QLatin1String("motor")) return present_motor(snapshot);
    const auto* health = snapshot.device(key);
    if (!health || !health->configured) return {Level::Idle, QStringLiteral("未启用"), {}, {}};
    if (!health->opened) return {Level::Danger, QStringLiteral("未连接"), {}, health->note};
    const auto data = readings.value(key).toObject();
    const auto reading_error = data.value("error").toString();
    if (!error.isEmpty() || !reading_error.isEmpty())
        return {Level::Warn, QStringLiteral("读取失败"), {}, reading_error.isEmpty() ? error : reading_error};

    DevicePresentation result{Level::Ok, QStringLiteral("在线"), {}, {}};
    if (key == QLatin1String("laser")) {
        const auto shutter = data.value("q_gate_open");
        const auto first = data.value("interlock_1");
        const auto second = data.value("interlock_2");
        const bool interlocked = (first.isBool() && !first.toBool()) || (second.isBool() && !second.toBool());
        result.readings << QStringLiteral("光闸  %1").arg(!shutter.isBool() ? QStringLiteral("—") :
            shutter.toBool() ? QStringLiteral("开启") : QStringLiteral("关闭"));
        result.readings << QStringLiteral("安全联锁  %1").arg(interlocked ? QStringLiteral("断开") :
            first.isBool() && second.isBool() ? QStringLiteral("已闭合") : QStringLiteral("—"));
        if (interlocked || data.value("emergency_stop").toBool() || data.value("overtemperature").toBool() ||
            data.value("fault_bits").toInt() != 0 || snapshot.gate.state == QLatin1String("fault")) {
            result.level = Level::Danger;
            result.status = interlocked ? QStringLiteral("联锁断开") : data.value("emergency_stop").toBool()
                ? QStringLiteral("急停触发") : data.value("overtemperature").toBool()
                ? QStringLiteral("激光器超温") : QStringLiteral("设备告警");
            result.detail = snapshot.gate.detail;
        }
    } else if (key == QLatin1String("spectrometer")) {
        result.readings = spectrum_readings(readings.value("spectrometer_channels").toArray());
    } else if (key == QLatin1String("air_conditioner")) {
        result.readings << QStringLiteral("模式回读  %1").arg(air_mode_name(data.value("mode_code").toInt(-1)));
        const auto fan_state = [&data](const char* key) {
            const auto state = data.value(QLatin1String(key));
            if (!state.isBool()) return QStringLiteral("未知");
            return state.toBool() ? QStringLiteral("运行") : QStringLiteral("停止");
        };
        result.readings << QStringLiteral("内风机 %1 / 外风机 %2")
            .arg(fan_state("indoor_fan_on"), fan_state("outdoor_fan_on"));
        if (data.value("cooling").toBool()) result.readings << QStringLiteral("制冷负载运行中");
        if (data.value("heating").toBool()) result.readings << QStringLiteral("加热负载运行中");
        result.readings << QStringLiteral("柜内温度  %1").arg(value_text(data, "temperature_c", QStringLiteral(" °C")));
        result.readings << QStringLiteral("相对湿度  %1").arg(value_text(data, "humidity_percent", QStringLiteral(" %RH")));
        QStringList alarms;
        for (const auto& alarm : data.value("alarms").toArray()) alarms << alarm.toString();
        const auto status_error = data.value("status_error").toString();
        const auto parameters_error = data.value("parameters_error").toString();
        if (data.value("fault").toBool() || !alarms.isEmpty() || !status_error.isEmpty()) {
            result.level = data.value("fault").toBool() ? Level::Danger : Level::Warn;
            result.status = result.level == Level::Danger ? QStringLiteral("设备故障")
                : !alarms.isEmpty() ? QStringLiteral("设备告警") : QStringLiteral("读取失败");
            result.detail = alarms.isEmpty() ? status_error : alarms.join(QStringLiteral("、"));
        }
        if (result.detail.isEmpty()) result.detail = parameters_error;
    } else if (key == QLatin1String("indicator") && !snapshot.lamp_error.isEmpty()) {
        result.level = Level::Warn;
        result.status = QStringLiteral("灯态未确认");
        result.detail = snapshot.lamp_error;
    }
    return result;
}

}  // namespace libs::ui
