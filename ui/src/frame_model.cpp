#include "frame_model.hpp"

#include <QJsonArray>

namespace libs::ui {
namespace {

QVector<double> to_doubles(const QJsonValue& v) {
    QVector<double> out;
    const QJsonArray arr = v.toArray();
    out.reserve(arr.size());
    for (const QJsonValue& x : arr) out.push_back(x.toDouble());
    return out;
}

}  // namespace

double ChannelData::peak_counts() const {
    double peak = 0.0;
    for (const double c : counts) {
        if (c > peak) peak = c;
    }
    return peak;
}

bool FrameData::has_any_present_channel() const {
    for (const ChannelData& c : channels) {
        if (c.present) return true;
    }
    return false;
}

FrameData parse_frame(const QJsonObject& p) {
    FrameData f;
    f.ok = p.value("ok").toBool(false);
    f.frame_index = static_cast<qint64>(p.value("frame_index").toDouble());
    f.shot_id = static_cast<qint64>(p.value("shot_id").toDouble());
    f.wall_clock_ms = static_cast<qint64>(p.value("wall_clock_ms").toDouble());
    f.captured_monotonic_ms =
        static_cast<qint64>(p.value("captured_monotonic_ms").toDouble());
    // 逐字段原样搬运。hardware_synchronized 不做 "requested && hardware" 之类
    // 的"善意补全"——§3.10.1 说得很清楚：请求了不等于做到了。
    f.hardware_synchronized = p.value("hardware_synchronized").toBool(false);
    f.requested_sync = p.value("requested_sync").toBool(false);
    f.period_ms = p.value("period_ms").toDouble();
    f.capture_error_code = p.value("capture_error_code").toString();
    f.capture_error_message = p.value("capture_error_message").toString();

    const QJsonArray channels = p.value("channels").toArray();
    f.channels.reserve(channels.size());
    for (const QJsonValue& cv : channels) {
        const QJsonObject co = cv.toObject();
        ChannelData c;
        c.serial = co.value("serial").toString();
        // 三态的判据是**显式的 state 字段**，不是"counts 数组是不是空的"。
        // 业务层缺席时会写 state:"absent" + reason；UI 据此画缺席标记。
        c.present = (co.value("state").toString() == QLatin1String("present"));
        c.reason = co.value("reason").toString();
        c.wavelength_nm = to_doubles(co.value("wavelength_nm"));
        c.counts = to_doubles(co.value("counts"));
        f.channels.push_back(std::move(c));
    }
    return f;
}

}  // namespace libs::ui