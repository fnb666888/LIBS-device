#include "device_status_page.hpp"

#include <algorithm>

#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "widgets.hpp"

namespace libs::ui {
namespace {

QString reading(const QJsonObject& data, const char* key, const QString& unit = {}) {
    const auto value = data.value(QLatin1String(key));
    return value.isDouble() ? QString::number(value.toDouble(), 'g', 10) + unit : QStringLiteral("—");
}

QString state(const QJsonObject& data, const char* key, const QString& yes, const QString& no) {
    const auto value = data.value(QLatin1String(key));
    return value.isBool() ? (value.toBool() ? yes : no) : QStringLiteral("—");
}

QString lamp_color(const QString& intent) {
    if (intent == QLatin1String("ready_green")) return QStringLiteral("绿灯");
    if (intent == QLatin1String("standby_yellow")) return QStringLiteral("黄灯");
    if (intent == QLatin1String("fault_red")) return QStringLiteral("红灯");
    if (intent == QLatin1String("offline_all_off")) return QStringLiteral("全灭");
    return QStringLiteral("—");
}

}  // namespace

DeviceStatusPage::DeviceStatusPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 22, 24, 22);
    outer->setSpacing(16);
    auto* bar = new QHBoxLayout;
    setAccessibleName(QStringLiteral("设备状态"));
    bar->addStretch();
    updated_ = new QLabel(QStringLiteral("等待读取设备"), this);
    updated_->setObjectName(QStringLiteral("caption"));
    bar->addWidget(updated_);
    refresh_ = new QPushButton(QStringLiteral("刷新状态"), this);
    refresh_->setObjectName(QStringLiteral("ghost"));
    bar->addWidget(refresh_);
    outer->addLayout(bar);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* host = new QWidget(scroll);
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 4, 0);
    grid->setSpacing(16);
    const char* titles[] = {"激光器", "光谱仪", "三色状态灯", "测距仪", "空调"};
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
        auto& view = cards_[i];
        auto* card = new Card(QString::fromUtf8(titles[i]), host);
        view.frame = card;
        card->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        view.dot = new StatusDot(card);
        card->add_header_widget(view.dot);
        view.detail = new QLabel(card);
        view.detail->setObjectName(QStringLiteral("dim"));
        view.detail->setTextFormat(Qt::PlainText);
        view.detail->setWordWrap(true);
        view.detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
        card->body()->addWidget(view.detail);
        card->body()->addStretch();
        grid->addWidget(card, i / 3, i % 3);
    }
    for (int column = 0; column < 3; ++column) grid->setColumnStretch(column, 1);
    grid->setRowStretch(2, 1);
    scroll->setWidget(host);
    outer->addWidget(scroll, 1);
    render();
}

void DeviceStatusPage::apply_status(const MachineSnapshot& snapshot) {
    snapshot_ = snapshot;
    render();
}

void DeviceStatusPage::set_reading_pending(bool pending) {
    refresh_->setEnabled(!pending);
    if (pending && readings_.isEmpty()) updated_->setText(QStringLiteral("正在读取设备…"));
}

void DeviceStatusPage::apply_readings(const QJsonObject& readings, const QString& error) {
    readings_ = readings;
    reading_error_ = error;
    updated_->setText(error.isEmpty() ? QStringLiteral("最近读取 %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                     : QStringLiteral("设备读数刷新失败"));
    render();
}

void DeviceStatusPage::render() {
    const bool connected = snapshot_.valid && snapshot_.error.isEmpty();
    for (auto& card : cards_) {
        const auto key = QLatin1String(card.key);
        const auto* health = snapshot_.device(key);
        QStringList lines;
        if (!connected || !health) {
            card.dot->set_state(Level::Idle, QStringLiteral("未知"));
            card.detail->setText(connected ? QStringLiteral("服务未返回设备状态") : QStringLiteral("等待读取服务状态"));
            continue;
        }
        if (!health->configured || !health->opened) {
            card.dot->set_state(health->configured ? Level::Danger : Level::Idle,
                                health->configured ? QStringLiteral("打开失败") : QStringLiteral("未启用"));
            card.detail->setText(health->note.isEmpty() ? QStringLiteral("站点未启用设备") : health->note);
            continue;
        }
        card.dot->set_state(Level::Ok, QStringLiteral("已打开"));
        if (!health->endpoint.isEmpty()) lines << QStringLiteral("连接  %1").arg(health->endpoint);
        if (!health->model.isEmpty()) lines << QStringLiteral("型号  %1").arg(health->model);

        if (key == QLatin1String("indicator")) {
            lines << QStringLiteral("目标灯态  %1").arg(lamp_color(snapshot_.lamp_intent));
            if (!snapshot_.lamp_intent.isEmpty()) {
                lines << (snapshot_.lamp_applied ? QStringLiteral("下发并读回成功") : QStringLiteral("灯态未确认"));
                if (!snapshot_.lamp_applied) card.dot->set_state(Level::Warn, QStringLiteral("灯态未确认"));
            }
            if (!snapshot_.lamp_error.isEmpty()) lines << snapshot_.lamp_error;
        } else if (!reading_error_.isEmpty()) {
            card.dot->set_state(Level::Warn, QStringLiteral("读取失败"));
            lines << reading_error_;
        } else if (key == QLatin1String("laser")) {
            const auto data = readings_.value(key).toObject();
            if (data.value("error").isString()) {
                card.dot->set_state(Level::Warn, QStringLiteral("读取失败"));
                lines << data.value("error").toString();
            } else {
                lines << QStringLiteral("工作状态码  %1    光闸  %2").arg(reading(data, "work_state"), state(data, "q_gate_open", "开", "关"));
                lines << QStringLiteral("联锁 1  %1    联锁 2  %2").arg(state(data, "interlock_1", "闭合", "断开"), state(data, "interlock_2", "闭合", "断开"));
                lines << QStringLiteral("急停  %1    过温  %2").arg(state(data, "emergency_stop", "触发", "正常"), state(data, "overtemperature", "触发", "正常"));
                lines << QStringLiteral("故障码  %1    放电计数  %2").arg(reading(data, "fault_bits"), reading(data, "discharge_counter"));
            }
        } else if (key == QLatin1String("spectrometer")) {
            const auto channels = readings_.value("spectrometer_channels").toArray();
            for (const auto& value : channels) {
                const auto channel = value.toObject();
                lines << (channel.value("pixels").isDouble()
                    ? QStringLiteral("%1  %2 像素  %3–%4 nm").arg(channel.value("serial").toString(),
                        reading(channel, "pixels"), reading(channel, "wavelength_min_nm"), reading(channel, "wavelength_max_nm"))
                    : QStringLiteral("%1  通道不可用").arg(channel.value("serial").toString()));
            }
            if (!channels.isEmpty()) lines << QStringLiteral("通道信息来自最近一次空闲枚举");
            if (snapshot_.measurement_known) lines << QStringLiteral("采集 %1  失败 %2").arg(snapshot_.shots_captured).arg(snapshot_.shots_failed);
        } else if (key == QLatin1String("rangefinder")) {
            const auto data = readings_.value(key).toObject();
            if (data.value("error").isString()) {
                card.dot->set_state(Level::Warn, QStringLiteral("读取失败"));
                lines << data.value("error").toString();
            } else {
                if (!data.isEmpty() && !data.value("distance_mm").isDouble())
                    card.dot->set_state(Level::Warn, QStringLiteral("距离无效"));
                lines << QStringLiteral("距离  %1    置信度  %2").arg(reading(data, "distance_mm", " mm"), reading(data, "confidence_percent", "%"));
                lines << QStringLiteral("采样计数  %1").arg(reading(data, "sample_counter"));
            }
        } else if (key == QLatin1String("air_conditioner")) {
            const auto data = readings_.value(key).toObject();
            lines << QStringLiteral("柜内温度  %1    湿度  %2").arg(reading(data, "temperature_c", " °C"), reading(data, "humidity_percent", " %RH"));
            lines << QStringLiteral("电源  %1    模式码  %2").arg(state(data, "power_on", "开", "关"), reading(data, "mode_code"));
            lines << QStringLiteral("制冷  %1    加热  %2").arg(state(data, "cooling", "运行", "停止"), state(data, "heating", "运行", "停止"));
            QStringList alarms;
            for (const auto& alarm : data.value("alarms").toArray()) alarms << alarm.toString();
            const auto status_error = data.value("status_error").toString();
            const auto parameters_error = data.value("parameters_error").toString();
            lines << QStringLiteral("告警  %1").arg(!alarms.isEmpty() ? alarms.join(QStringLiteral("、")) :
                data.value("alarms").isArray() && status_error.isEmpty() ? QStringLiteral("无") : QStringLiteral("—"));
            if (!alarms.isEmpty()) card.dot->set_state(Level::Warn, QStringLiteral("设备告警"));
            if (!status_error.isEmpty() || !parameters_error.isEmpty()) {
                card.dot->set_state(Level::Warn, QStringLiteral("读数不完整"));
                if (!status_error.isEmpty()) lines << status_error;
                if (!parameters_error.isEmpty()) lines << parameters_error;
            }
            if (data.value("fault").toBool()) card.dot->set_state(Level::Danger, QStringLiteral("设备故障"));
        }
        card.detail->setText(lines.isEmpty() ? QStringLiteral("等待读取设备") : lines.join('\n'));
    }
    int height = 0;
    for (const auto& card : cards_) height = std::max(height, card.frame->sizeHint().height());
    for (const auto& card : cards_) card.frame->setMinimumHeight(height);
}

}  // namespace libs::ui
