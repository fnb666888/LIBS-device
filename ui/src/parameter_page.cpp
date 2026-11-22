#include "parameter_page.hpp"

#include <algorithm>
#include <QApplication>
#include <QDateTime>
#include <QGridLayout>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "device_status_page.hpp"
#include "widgets.hpp"
#include "libs/measurement_limits.hpp"

namespace libs::ui {
namespace {

void set_control_enabled(QWidget* control, bool enabled) {
    if (!enabled && control->isEnabled()) {
        auto* focused = QApplication::focusWidget();
        // Disabling the focused widget would make Qt focus and scroll to the next control.
        if (focused && (focused == control || control->isAncestorOf(focused)))
            focused->clearFocus();
    }
    control->setEnabled(enabled);
}

// ── 字段表（唯一真值来源）───────────────────────────────────────────────
//
// 字段名逐条抄自 `src/src/business/app/parameter_service.cpp` 里的
// kField* 常量。**以前这份表抄的是旧架构的字段名**
//（`discharge_current` 而不是 `discharge_current_a`），
// 于是 param_set 每次都被后端判 parameter_field_bad ——
// 保存按钮坏了一整个重构周期，根因就是这两份名字表没有对齐。
//
// 量程也抄自同一处（take_uint 只要求非负整数；光谱仪三个字段有硬边界），
// 不抄设计文档：文档里的 0~50 A / 200 Hz 是当年那份 0x17 电压限值取 0 V
// 时的产物，照抄会把整台设备配置到不出光。

// 后端启动时补齐缺失工艺值。Qt 仅提交界面上修改过的字段，保留测量配置中
// 的历史字段和值，例如旧配置中的 min_discharges。

// spectrometer 组 3 字段，**全部 required**。
//
// 刻意不放 integration_delay_cycles / laser_delay_cycles / laser_width_cycles：
// 单位是 FPGA 计数而不是 μs/ns，属于驱动内部换算；而且后两个在本项目的
// 触发方向（**激光器触发光谱仪**，AvaSoft 手册 §4.9 接法二）下**必须恒为 0**，
// 摆到界面上等于给现场准备了一个"一调就永远不出光"的开关。
// 那两个恒 0 的字段曾经放在一个叫 `trigger` 的第三组里，现在整组已删除。

QString fmt_range(double lo, double hi, int decimals, const char* unit) {
    const auto format = [decimals](double value) {
        QString result = QLocale(QLocale::English).toString(value, 'f', decimals);
        if (decimals > 0) {
            while (result.endsWith(QLatin1Char('0'))) result.chop(1);
            if (result.endsWith(QLatin1Char('.'))) result.chop(1);
        }
        return result;
    };
    QString text = QStringLiteral("%1 – %2 %3")
                       .arg(format(lo))
                       .arg(format(hi))
                       .arg(QString::fromUtf8(unit));
    return text;
}

}  // namespace

// 表定义在**成员函数内部**是有意的：FieldSpec 是 ParameterPage 的 private
// 嵌套类型，类外的匿名命名空间拿不到它的限定名。放进成员函数里既不用把
// private 放开成 public，表也仍然只有这一份 —— 不存在"两份字段名表"这个
// 老问题。

const ParameterPage::FieldSpec* ParameterPage::laser_specs(std::size_t& count) {
    // 量程来自共用的 measurement_limits.hpp，不在 UI 复制数值。
    // 同一份规则用于参数保存、业务校验和驱动。
    // 以前这里下限全是 0.0，于是界面允许输入 0，后端当场拒绝 ——
    // "界面能填但存不进去"比直接禁掉更难查。
    //
    // 上下限收紧还有一层用意：未标定框会被推到本字段下限（见 apply_values），
    // 下限非 0 的字段因此一眼就能看出"这里要填数"，而不会是 0。
    static const FieldSpec kLaserFields[] = {
        {"discharge_current_a", "放电电流", 1, "A",
         "协议 0x11：量程 15~100 A。真机默认 80 A。超过 30 A 属高功率区，提交后二次确认。"},
        {"pulse_width_us", "放电脉宽", 0, "μs",
         "协议 0x12：量程 50~300 μs。真机默认 200 μs。"},
        {"frequency_hz", "放电频率", 0, "Hz",
         "协议 0x13：量程 1~10 Hz。真机默认 1 Hz。"},
        {"discharge_delay_us", "放电延时", 0, "μs",
         "协议 0x14：量程 0~500 μs。真机默认 0 μs。"},
        {"q_delay_us", "Q 延时", 0, "μs",
         "协议 0x16：量程 2~500 μs。调节 Q 开关打开的时刻，影响激光脉冲形成。"},
        {"q_warmup_s", "Q 预热", 0, "s",
         "协议 0x19 未标厂家量程，软件允许 0~3600 s。真机默认 0 s。"},
        {"settle_timeout_ms", "稳定等待超时", 0, "ms",
         "设备从 02「自适应中」走到 01「正在工作」的等待上限。真机默认 20000 ms。"},
    };
    count = sizeof(kLaserFields) / sizeof(FieldSpec);
    return kLaserFields;
}

const ParameterPage::FieldSpec* ParameterPage::spectrometer_specs(std::size_t& count) {
    // 刻意不放 integration_delay_cycles / laser_delay_cycles / laser_width_cycles：
    // 单位是 FPGA 计数而不是 μs/ns，属于驱动内部换算；而且后两个在本项目的
    // 触发方向（**激光器触发光谱仪**，AvaSoft 手册 §4.9 接法二）下**必须恒为 0**，
    // 摆到界面上等于给现场准备了一个"一调就永远不出光"的开关。
    // 那两个恒 0 的字段曾经放在一个叫 `trigger` 的第三组里，现在整组已删除。
    static const FieldSpec kSpectrometerFields[] = {
        {"integration_ms", "积分时间", 3, "ms",
         "软件允许 0.002~60000 ms；具体探测器最小积分时间另由厂家 SDK 校验。真机默认 1.05 ms。"},
        {"integration_delay_ns", "积分延时", 0, "ns",
         "收到激光 TTL 后延迟多久开始积分。真机默认 1280 ns；每个样品要重新标定。"},
        {"capture_timeout_ms", "采集超时", 0, "ms",
         "每帧等待 pin6 触发的超时。真机默认 10000 ms。"},
    };
    count = sizeof(kSpectrometerFields) / sizeof(FieldSpec);
    return kSpectrometerFields;
}

const ParameterPage::FieldSpec* ParameterPage::air_specs(std::size_t& count) {
    static const FieldSpec fields[] = {
        {"cooling_on_c", "制冷开启", 1, "°C", "柜内达到此温度时开启制冷。", 20, 55},
        {"cooling_off_c", "制冷停止", 1, "°C", "柜内降至此温度时停止制冷。", 15, 35},
        {"heating_on_c", "加热开启", 1, "°C", "柜内降至此温度时开启加热。", -30, 15},
        {"heating_off_c", "加热停止", 1, "°C", "柜内达到此温度时停止加热。", 0, 23},
        {"dehumidify_on_percent", "除湿开启", 1, "%RH", "湿度达到此设定时开启除湿。", 0, 100},
        {"dehumidify_off_percent", "除湿停止", 1, "%RH", "湿度达到此设定时停止除湿。", 0, 100},
    };
    count = sizeof(fields) / sizeof(FieldSpec);
    return fields;
}



namespace {

class ParameterSpinBox final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;
    QLineEdit* editor() const { return lineEdit(); }

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

}  // namespace

const ParameterPage::FieldSpec* ParameterPage::range_specs(std::size_t& count) {
    static const FieldSpec fields[] = {
        {"empty_belt_mm", "空带基准", 0, "mm", "空载皮带到探头的距离。", 0, 65534},
        {"material_delta_mm", "来料阈值", 0, "mm", "距离相比空带减少达到此值时判定有料。", 0, 65534},
        {"valid_min_mm", "可测距离下限", 0, "mm", "有效窗口的最小距离。", 0, 65534},
        {"valid_max_mm", "可测距离上限", 0, "mm", "有效窗口的最大距离。", 0, 65534},
    };
    count = sizeof(fields) / sizeof(fields[0]);
    return fields;
}

ParameterPage::ParameterPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("parameterPage"));
    setAccessibleName(QStringLiteral("仪器参数"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 18, 24, 22);
    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget(make_hint(QStringLiteral("真机调试操作会下发到设备。正式检测使用已保存的激光和光谱参数。")), 1);
    refreshed_ = make_hint(QStringLiteral("等待读取设备"));
    toolbar->addWidget(refreshed_);
    refresh_ = new QPushButton(QStringLiteral("刷新状态"), this);
    refresh_->setObjectName(QStringLiteral("ghost"));
    toolbar->addWidget(refresh_);
    outer->addLayout(toolbar);
    connect(refresh_, &QPushButton::clicked, this, &ParameterPage::refresh_requested);
    auto* scroll = new QScrollArea(this);
    scroll_ = scroll;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* host = new QWidget(scroll);
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 4, 0);
    grid->setSpacing(16);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(3, 1);

    std::size_t count = 0;
    laser_.name = QStringLiteral("laser");
    auto* specs = laser_specs(count);
    build_group(laser_, QStringLiteral("激光器"), specs, count);
    spectrometer_.name = QStringLiteral("spectrometer");
    specs = spectrometer_specs(count);
    build_group(spectrometer_, QStringLiteral("光谱仪"), specs, count);
    air_.name = QStringLiteral("air_conditioner");
    specs = air_specs(count);
    build_group(air_, QStringLiteral("空调"), specs, count);
    range_.name = QStringLiteral("rangefinder");
    specs = range_specs(count);
    build_group(range_, QStringLiteral("测距仪"), specs, count);
    motor_.name = QStringLiteral("motor");
    static const FieldSpec jog[] = {
        {"jog_s", "点动时长", 3, "s", "点动后发出停止指令的时间。",
         libs::measurement_limits::kMotorJogMinMs / 1000.0, libs::measurement_limits::kMotorJogMaxMs / 1000.0},
        {"jog_hz", "正反向点动频率", 1, "Hz", "保存后写入变频器，正反向采用同一频率。",
         libs::measurement_limits::kMotorJogMinTenthsHz / 10.0, libs::measurement_limits::kMotorJogMaxTenthsHz / 10.0},
    };
    build_group(motor_, QStringLiteral("检测头移位电机"), jog, sizeof(jog) / sizeof(jog[0]));
    build_lamp_card();
    build_manual_controls();

    grid->addWidget(laser_.card, 0, 0, Qt::AlignTop);
    grid->addWidget(air_.card, 0, 1, Qt::AlignTop);
    grid->addWidget(spectrometer_.card, 1, 0, Qt::AlignTop);
    grid->addWidget(range_.card, 1, 1, Qt::AlignTop);
    grid->addWidget(lamp_card_, 2, 0, Qt::AlignTop);
    grid->addWidget(motor_.card, 2, 1, Qt::AlignTop);
    scroll->setWidget(host);
    outer->addWidget(scroll);

    for (auto* group : {&laser_, &spectrometer_}) {
        connect(group->save, &QPushButton::clicked, this, [this, group] { save_measurement(*group); });
        connect(group->read, &QPushButton::clicked, this, [this, group] {
            group->pending = true;
            update_measurement_controls(laser_);
            update_measurement_controls(spectrometer_);
            group->feedback->setText(QStringLiteral("正在重新读取…"));
            emit reload_requested(group->name);
        });
        update_measurement_controls(*group);
    }
    load_air_settings();
    connect(air_.read, &QPushButton::clicked, this, [this] {
        const auto* health = snapshot_.device(air_.name);
        if (!snapshot_.valid || !health || !health->opened) {
            load_air_settings();
            air_.feedback->setText(QStringLiteral("已重新读取本机设定。"));
            return;
        }
        set_air_busy(true);
        air_.feedback->setText(QStringLiteral("正在读取空调设定…"));
        emit air_read_requested();
    });
    connect(air_.save, &QPushButton::clicked, this, [this] {
        const auto values = draft_values(air_);
        const auto n = [&values](const char* key) { return values.value(QLatin1String(key)).toDouble(); };
        if (n("cooling_on_c") <= n("cooling_off_c") || n("heating_on_c") >= n("heating_off_c") ||
            n("dehumidify_on_percent") <= n("dehumidify_off_percent")) {
            air_.feedback->setText(QStringLiteral("请检查制冷、加热和除湿的启停阈值。"));
            return;
        }
        const auto* health = snapshot_.device(air_.name);
        if (!snapshot_.valid || !health || !health->opened) {
            air_.feedback->setText(save_air_settings(values) ? QStringLiteral("已保存到本机。")
                                                            : QStringLiteral("本机保存失败。"));
            return;
        }
        set_air_busy(true);
        air_.feedback->setText(QStringLiteral("正在保存并核对…"));
        emit air_apply_requested(values);
    });
    set_air_busy(false);

    connect(range_.read, &QPushButton::clicked, this, [this] {
        load_range_rules();
        render_range();
    });
    connect(range_.save, &QPushButton::clicked, this, &ParameterPage::save_range_rules);
    load_range_rules();
    connect(motor_.read, &QPushButton::clicked, this, [this] {
        load_motor_settings();
        request_motor_profile();
    });
    connect(motor_.save, &QPushButton::clicked, this, &ParameterPage::save_motor_settings);
    load_motor_settings();
    render_status();
    update_manual_controls();
}

void ParameterPage::build_group(Group& group, const QString& title, const FieldSpec* specs, std::size_t count) {
    group.card = new Card(title, this);
    group.card->setProperty("deviceGroup", group.name);
    build_status(group.card, group.name);
    build_section(group.card, QStringLiteral("参数调节"), group.name == QLatin1String("air_conditioner")
                  ? QStringLiteral("air") : group.name);
    auto* grid = new QGridLayout;
    grid->setSpacing(10);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& spec = specs[i];
        const auto* limits = libs::measurement_limits::find(group.name.toStdString(), spec.key);
        const auto lo = limits ? limits->lo : spec.minimum;
        const auto hi = limits ? limits->hi : spec.maximum;
        const int decimals = limits && limits->integer ? 0 : spec.decimals;
        auto* tile = new QWidget(group.card);
        auto* layout = new QVBoxLayout(tile);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        auto* label = new QLabel(QString::fromUtf8(spec.label), tile);
        label->setObjectName(QStringLiteral("parameterLabel"));
        layout->addWidget(label);

        Field field;
        field.key = QString::fromUtf8(spec.key);
        auto* box = new ParameterSpinBox(tile);
        field.box = box;
        box->setObjectName(field.key);
        box->setAccessibleName(label->text());
        box->setDecimals(decimals);
        box->setRange(lo, hi);
        box->setSuffix(QStringLiteral(" ") + QString::fromUtf8(spec.unit));
        box->setSingleStep(decimals == 0 ? 1.0 : decimals == 1 ? 0.1 : 0.001);
        box->setKeyboardTracking(false);
        box->setToolTip(QString::fromUtf8(spec.tooltip));
        box->setMinimumWidth(95);
        if (group.name == QLatin1String("rangefinder")) box->setSpecialValueText(QStringLiteral("—"));
        layout->addWidget(box);

        auto* caption = make_hint(group.name == QLatin1String("rangefinder")
            ? QString::fromUtf8(spec.tooltip) : fmt_range(lo, hi, decimals, spec.unit));
        layout->addWidget(caption);
        field.error = make_hint({});
        field.error->setObjectName(QStringLiteral("parameterError"));
        field.error->hide();
        layout->addWidget(field.error);
        grid->addWidget(tile, static_cast<int>(i / 2), static_cast<int>(i % 2));
        group.fields.push_back(field);
        const auto mark_edited = [this, &group, i] {
            auto& changed = group.fields[static_cast<int>(i)];
            changed.edited = true;
            if (changed.known) changed.error->hide();
            if (&group == &laser_ || &group == &spectrometer_)
                group.feedback->setText(QStringLiteral("有未保存的改动。"));
            if (&group == &motor_ && changed.key == QLatin1String("jog_hz") && changed.loaded) {
                group.feedback->setText(QStringLiteral("频率已修改，保存并回读后可点动。"));
                update_manual_controls();
            }
        };
        connect(box, &QDoubleSpinBox::valueChanged, this, mark_edited);
        connect(box->editor(), &QLineEdit::textEdited, this, [box, mark_edited] {
            if (box->hasAcceptableInput()) mark_edited();
        });
    }
    group.card->body()->addLayout(grid);
    auto* footer = new QHBoxLayout;
    group.feedback = make_hint({});
    group.feedback->setTextFormat(Qt::PlainText);
    footer->addWidget(group.feedback, 1);
    group.read = new QPushButton(QStringLiteral("重新读取"), group.card);
    group.read->setObjectName(QStringLiteral("ghost"));
    group.read->setAccessibleName(QStringLiteral("重新读取") + title + QStringLiteral("参数"));
    footer->addWidget(group.read);
    group.save = new QPushButton(QStringLiteral("保存参数"), group.card);
    group.save->setObjectName(QStringLiteral("primary"));
    group.save->setAccessibleName(QStringLiteral("保存") + title + QStringLiteral("参数"));
    footer->addWidget(group.save);
    group.card->body()->addLayout(footer);
}

void ParameterPage::build_section(Card* card, const QString& title, const QString& topic) {
    card->body()->addWidget(make_divider());
    auto* row = new QHBoxLayout;
    auto* caption = new QLabel(title, card);
    caption->setObjectName(QStringLiteral("parameterLabel"));
    row->addWidget(caption);
    row->addStretch();
    if (!topic.isEmpty()) {
        auto* help = new QPushButton(QStringLiteral("参数说明"), card);
        help->setObjectName(QStringLiteral("parameterHelp"));
        row->addWidget(help);
        connect(help, &QPushButton::clicked, this, [this, topic] { emit help_requested(topic); });
    }
    card->body()->addLayout(row);
}

void ParameterPage::build_status(Card* card, const QString& key) {
    card->body()->addWidget(make_divider());
    auto* row = new QHBoxLayout;
    auto* heading = new QLabel(QStringLiteral("状态"), card);
    heading->setObjectName(QStringLiteral("parameterLabel"));
    row->addWidget(heading);
    auto* badge = new StatusDot(card);
    row->addWidget(badge);
    row->addStretch();
    card->body()->addLayout(row);
    auto* readings = make_hint({});
    readings->setTextFormat(Qt::PlainText);
    readings->setObjectName(QStringLiteral("deviceReadings"));
    card->body()->addWidget(readings);
    status_views_.insert(key, {badge, readings});
}

void ParameterPage::show_device(const QString& topic) {
    const auto key = topic == QLatin1String("air") ? QStringLiteral("air_conditioner") : topic;
    for (auto* card : scroll_->widget()->findChildren<Card*>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (card->property("deviceGroup").toString() == key) {
            scroll_->verticalScrollBar()->setValue(card->y());
            return;
        }
    }
}

void ParameterPage::set_reading_pending(bool pending) {
    set_control_enabled(refresh_, !pending);
    if (pending && readings_.isEmpty()) refreshed_->setText(QStringLiteral("正在读取设备…"));
}

void ParameterPage::apply_readings(const QJsonObject& readings, const QString& error) {
    readings_ = readings;
    reading_error_ = error;
    range_reading_ = readings.value("rangefinder").toObject();
    refreshed_->setText(error.isEmpty() ? QStringLiteral("最近读取 %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                        : QStringLiteral("设备读数刷新失败"));
    render_status();
}

void ParameterPage::render_status() {
    for (auto it = status_views_.begin(); it != status_views_.end(); ++it) {
        const auto value = present_device(it.key(), snapshot_, readings_, reading_error_);
        it->badge->set_state(value.level, value.status);
        it->badge->setToolTip(value.detail);
        it->readings->setText(value.readings.join(QStringLiteral("    ·    ")));
        it->readings->setVisible(!value.readings.isEmpty());
    }
    render_range();
    render_lamp();
}

QJsonObject ParameterPage::draft_values(const Group& group) const {
    QJsonObject values;
    for (const auto& field : group.fields) {
        field.box->interpretText();
        values.insert(field.key, field.box->value());
    }
    return values;
}

void ParameterPage::load_air_settings() {
    static constexpr double defaults[] = {32, 27, 5, 10, 65, 55};
    QSettings settings;
    settings.beginGroup(QStringLiteral("airSetpoints"));
    for (qsizetype i = 0; i < air_.fields.size(); ++i) {
        const auto& field = air_.fields[i];
        bool ok = false;
        const auto value = settings.value(field.key).toDouble(&ok);
        field.box->setValue(ok && value >= field.box->minimum() && value <= field.box->maximum() ? value : defaults[i]);
    }
}

bool ParameterPage::save_air_settings(const QJsonObject& values) {
    QSettings settings;
    settings.beginGroup(QStringLiteral("airSetpoints"));
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) settings.setValue(it.key(), it.value().toDouble());
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void ParameterPage::load_motor_settings() {
    const auto value = QSettings().value(QStringLiteral("motorTrial/jogMs"));
    const int site_value = snapshot_.motor.value("jog_ms").toInt(500);
    const int duration = value.isValid() ? value.toInt() : site_value;
    const QSignalBlocker blocker(motor_.fields.front().box);
    motor_.fields.front().box->setValue((duration >= static_cast<int>(libs::measurement_limits::kMotorJogMinMs) &&
        duration <= static_cast<int>(libs::measurement_limits::kMotorJogMaxMs) ? duration : 500) / 1000.0);
    motor_.fields.front().edited = false;
}

void ParameterPage::save_motor_settings() {
    auto& frequency = motor_.fields.back();
    frequency.box->interpretText();
    if (frequency.loaded && frequency.edited) {
        motor_pending_ = true;
        motor_.feedback->setText(QStringLiteral("正在写入并回读点动频率…"));
        update_manual_controls();
        emit motor_frequency_requested(frequency.box->value());
        return;
    }
    motor_.feedback->setText(persist_motor_duration() ? QStringLiteral("点动时长已保存到本机。")
                                                    : QStringLiteral("点动时长本机保存失败。"));
}

bool ParameterPage::persist_motor_duration() const {
    auto* duration = motor_.fields.front().box;
    duration->interpretText();
    QSettings settings;
    settings.setValue(QStringLiteral("motorTrial/jogMs"), qRound(duration->value() * 1000.0));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void ParameterPage::request_motor_profile() {
    motor_pending_ = true;
    motor_.feedback->setText(QStringLiteral("正在读取变频器参数…"));
    update_manual_controls();
    emit motor_requested(QStringLiteral("profile"), 0);
}

void ParameterPage::build_manual_controls() {
    const auto add_section = [this](Group& group) {
        build_section(group.card, QStringLiteral("手动测试"));
        auto* row = new QHBoxLayout;
        row->addStretch();
        group.card->body()->addLayout(row);
        group.manual_feedback = make_hint({});
        group.manual_feedback->setTextFormat(Qt::PlainText);
        group.card->body()->addWidget(group.manual_feedback);
        return row;
    };
    const auto button = [](QHBoxLayout* row, const QString& text, bool stop = false) {
        auto* action = new QPushButton(text);
        action->setFocusPolicy(Qt::TabFocus);
        if (stop) action->setObjectName(QStringLiteral("danger"));
        row->insertWidget(row->count() - 1, action);
        return action;
    };
    auto* laser_row = add_section(laser_);
    hardware_start_ = button(laser_row, QStringLiteral("开始真机连续检测"));
    hardware_stop_ = button(laser_row, QStringLiteral("停止真机检测"), true);
    connect(hardware_start_, &QPushButton::clicked, this, &ParameterPage::hardware_start_requested);
    connect(hardware_stop_, &QPushButton::clicked, this, &ParameterPage::hardware_stop_requested);

    spectrum_trial_ = button(add_section(spectrometer_), QStringLiteral("真机单帧采集"));
    spectrum_trial_->setToolTip(QStringLiteral("按已保存的积分时间独立采集，使用光谱仪内部触发。"));
    connect(spectrum_trial_, &QPushButton::clicked, this, [this] {
        spectrum_pending_ = true;
        spectrometer_.manual_feedback->setText(QStringLiteral("正在采集真机光谱…"));
        update_manual_controls();
        emit capture_requested();
    });

    auto* air_row = add_section(air_);
    const QString modes[] = {QStringLiteral("standby"), QStringLiteral("cooling"),
                             QStringLiteral("heating"), QStringLiteral("dehumidify")};
    for (int mode_code = 0; mode_code < 4; ++mode_code) {
        auto* action = button(air_row, air_mode_name(mode_code));
        action->setToolTip(QStringLiteral("连续 100 秒未收到试运行指令后，空调恢复自动运行。"));
        air_trials_.append(action);
        connect(action, &QPushButton::clicked, this, [this, mode = modes[mode_code]] {
            set_air_busy(true);
            air_.manual_feedback->setText(QStringLiteral("正在下发试运行指令并读取空调模式…"));
            emit air_control_requested({{"action", "set_air_mode"}, {"mode", mode}});
        });
    }

    range_sample_ = button(add_section(range_), QStringLiteral("单次测距"));
    connect(range_sample_, &QPushButton::clicked, this, [this] {
        range_test_pending_ = true;
        range_.manual_feedback->setText(QStringLiteral("正在读取测距仪…"));
        update_manual_controls();
        emit range_read_requested();
    });

    auto* motor_row = add_section(motor_);
    motor_forward_ = button(motor_row, QStringLiteral("正向点动"));
    motor_reverse_ = button(motor_row, QStringLiteral("反向点动"));
    motor_stop_ = button(motor_row, QStringLiteral("停止点动"), true);
    const auto connect_motor = [this](QPushButton* action_button, const QString& action) {
        connect(action_button, &QPushButton::clicked, this, [this, action] {
            motor_pending_ = true;
            motor_.manual_feedback->setText(QStringLiteral("正在发送指令…"));
            auto* seconds = motor_.fields.front().box;
            seconds->interpretText();
            const int duration = action == QLatin1String("stop") ? 0 : qRound(seconds->value() * 1000.0);
            update_manual_controls();
            emit motor_requested(action, duration);
        });
    };
    connect_motor(motor_forward_, QStringLiteral("forward"));
    connect_motor(motor_reverse_, QStringLiteral("reverse"));
    connect_motor(motor_stop_, QStringLiteral("stop"));
}

void ParameterPage::update_manual_controls() {
    const bool connected = snapshot_.valid && snapshot_.error.isEmpty();
    const auto opened = [this, connected](const QString& name) {
        const auto* device = snapshot_.device(name);
        return connected && device && device->opened;
    };
    set_control_enabled(hardware_start_, connected && snapshot_.gate.allowed && !measuring_ && !measurement_command_pending_ &&
                                !laser_.pending && !spectrometer_.pending && !spectrum_pending_ && !lamp_pending_);
    hardware_start_->setToolTip(snapshot_.gate.allowed ? QStringLiteral("使用正式检测流程触发实际出光和光谱采集。")
                                                       : snapshot_.gate.detail);
    set_control_enabled(hardware_stop_, connected && measuring_ && !measurement_command_pending_);
    set_control_enabled(spectrum_trial_, opened(spectrometer_.name) && !measuring_ && !spectrometer_.pending &&
                                !spectrum_pending_ && !measurement_command_pending_);
    const bool air_available = opened(air_.name) && !measuring_ && !air_.pending && !measurement_command_pending_;
    for (auto* action : air_trials_) set_control_enabled(action, air_available);
    set_control_enabled(range_sample_, opened(range_.name) && !range_test_pending_ && !measuring_);
    const auto& data = snapshot_.motor;
    const bool controls = snapshot_.valid && snapshot_.error.isEmpty() &&
                          data.value("opened").toBool() && data.value("control_enabled").toBool();
    const bool settled = !motor_pending_ && !measuring_ && (!controls || data.value("stopped").toBool());
    const bool can_jog = controls && settled && data.value("communication_control").toBool() &&
                         data.value("fault_code").toInt(-1) == 0;
    const bool frequency_saved = !motor_.fields.back().edited;
    set_control_enabled(motor_forward_, can_jog && frequency_saved && !data.value("forward_limit").toBool());
    set_control_enabled(motor_reverse_, can_jog && frequency_saved && !data.value("reverse_limit").toBool());
    set_control_enabled(motor_stop_, controls && !motor_pending_);
    if (connected && data.value("opened").toBool() && !data.value("control_enabled").toBool()) {
        motor_.manual_feedback->setText(QStringLiteral("当前站点只读，点动未启用。"));
    }
    set_control_enabled(motor_.read, settled && opened(motor_.name) && data.value("stopped").toBool());
    set_control_enabled(motor_.save, settled);
    set_control_enabled(motor_.fields.front().box->parentWidget(), settled);
    set_control_enabled(motor_.fields.back().box->parentWidget(), can_jog && motor_.fields.back().loaded);
}

void ParameterPage::set_measurement_command_pending(bool pending) {
    measurement_command_pending_ = pending;
    update_manual_controls();
    render_lamp();
}

void ParameterPage::finish_capture(const QJsonObject& result, bool ok, const QString& error) {
    spectrum_pending_ = false;
    if (!ok) {
        spectrometer_.manual_feedback->setText(error);
    } else {
        const auto scans = result.value("scan_groups").toArray();
        const bool complete = !scans.isEmpty() && scans.first().toObject().value("complete").toBool();
        spectrometer_.manual_feedback->setText(complete
            ? QStringLiteral("已采集并保存真机单帧 · %1 路通道").arg(result.value("channel_count").toInt())
            : QStringLiteral("采集未获得完整一帧，请检查通道读回。"));
        spectrometer_.manual_feedback->setToolTip(result.value("delivery").toObject().value("local_path").toString());
    }
    update_manual_controls();
}

void ParameterPage::finish_air_control(const QJsonObject& reading, bool ok, const QString& error) {
    if (ok) {
        auto actual = readings_.value(air_.name).toObject();
        for (auto field = reading.constBegin(); field != reading.constEnd(); ++field) actual.insert(field.key(), field.value());
        readings_.insert(air_.name, actual);
        air_.manual_feedback->setText(QStringLiteral("模式回读为%1。连续 100 秒未收到模式指令后，空调会恢复自动运行。").arg(
            air_mode_name(reading.value("mode_code").toInt(-1))));
        render_status();
    } else air_.manual_feedback->setText(error);
    set_air_busy(false);
}

void ParameterPage::finish_motor(const QString& action, const QJsonObject& data, bool ok, const QString& error) {
    motor_pending_ = false;
    if (action == QLatin1String("profile") || action == QLatin1String("set_frequency")) {
        QString message = error;
        if (ok) {
            auto& frequency = motor_.fields.back();
            const double forward_hz = data.value("forward_hz").toDouble();
            const double reverse_hz = data.value("reverse_hz").toDouble();
            const QSignalBlocker blocker(frequency.box);
            frequency.box->setValue(forward_hz);
            frequency.loaded = true;
            frequency.edited = false;
            if (action == QLatin1String("set_frequency")) {
                message = QStringLiteral("正反向 %1 Hz 已写入并回读。%2").arg(forward_hz).arg(
                    persist_motor_duration() ? QStringLiteral("点动时长已保存到本机。")
                                             : QStringLiteral("点动时长本机保存失败。"));
            } else {
                const int timeout = data.value("communication_timeout_ms").toInt();
                message = QStringLiteral("正向 %1 Hz · 反向 %2 Hz · 点动减速 %3 ms · 当前通讯超时 %4")
                    .arg(forward_hz).arg(reverse_hz).arg(data.value("deceleration_ms").toInt())
                    .arg(timeout ? QStringLiteral("%1 ms").arg(timeout) : QStringLiteral("未启用"));
            }
        }
        motor_.feedback->setText(message);
        update_manual_controls();
        return;
    }
    if (ok && !data.isEmpty()) snapshot_.motor = data;
    motor_.manual_feedback->setText(!ok ? error : action != QLatin1String("stop")
        ? QStringLiteral("已发送点动指令，请查看状态回读。")
        : data.value("stopped").toBool() ? QStringLiteral("变频器停机已回读。")
        : QStringLiteral("已发送停止指令，等待停机回读。"));
    render_status();
    update_manual_controls();
}

ParameterPage::Group* ParameterPage::measurement_group(const QString& name) {
    if (name == laser_.name) return &laser_;
    if (name == spectrometer_.name) return &spectrometer_;
    return nullptr;
}

void ParameterPage::apply_snapshot(const MachineSnapshot& snapshot) {
    const bool was_measuring = measuring_;
    measuring_ = snapshot.laser_running || snapshot.measurement_running
        || snapshot.stop_confirmation == QLatin1String("unconfirmed");
    if (measuring_ && !was_measuring) active_range_rules_ = range_rules_;
    snapshot_ = snapshot;
    if (!motor_site_loaded_ && snapshot_.motor.value("opened").toBool() &&
            snapshot_.motor.value("stopped").toBool() && !motor_pending_) {
        motor_site_loaded_ = true;
        const auto& field = motor_.fields.front();
        if (!field.edited && !field.box->hasFocus()) load_motor_settings();
        request_motor_profile();
    }
    render_status();
    update_manual_controls();
    for (auto* group : {&laser_, &spectrometer_}) {
        update_measurement_controls(*group);
        if (group->pending || !snapshot.valid || snapshot.param_values.isEmpty()
            || (snapshot.params_requested_at_ms > 0 && snapshot.params_requested_at_ms < group->refresh_after_ms)) continue;
        const auto edited = [](const auto& field) { return field.edited || field.box->hasFocus(); };
        if (std::any_of(group->fields.cbegin(), group->fields.cend(), edited)) continue;
        apply_values(*group, snapshot.param_values);
    }
}

void ParameterPage::apply_values(Group& group, const QJsonObject& groups) {
    const auto values = groups.value(group.name).toObject().value("values").toObject();
    for (auto& field : group.fields) {
        const auto value = values.value(field.key);
        field.known = value.isDouble() && value.toDouble() >= field.box->minimum()
            && value.toDouble() <= field.box->maximum();
        field.loaded = true;
        field.edited = false;
        const QSignalBlocker blocked(field.box);
        field.box->setValue(field.known ? value.toDouble() : field.box->minimum());
        field.original = field.box->value();
        field.error->setText(field.known ? QString{} : QStringLiteral("请填写本机标定值。"));
        field.error->setVisible(!field.known);
    }
    update_measurement_controls(group);
}

void ParameterPage::update_measurement_controls(Group& group) {
    const bool enabled = snapshot_.valid && !measuring_ && !group.pending;
    for (const auto& field : group.fields) {
        set_control_enabled(field.box->parentWidget(), enabled && field.loaded);
    }
    const bool can_request = enabled && !laser_.pending && !spectrometer_.pending;
    set_control_enabled(group.read, can_request);
    set_control_enabled(group.save, can_request && !group.fields.isEmpty() && group.fields.front().loaded);
    if (measuring_ && !group.pending) group.feedback->setText(QStringLiteral("检测中，停止后可修改。"));
    else if (group.feedback->text() == QStringLiteral("检测中，停止后可修改。")) group.feedback->clear();
    update_manual_controls();
}

void ParameterPage::save_measurement(Group& group) {
    if (laser_.pending || spectrometer_.pending || measuring_) return;
    QJsonObject values;
    for (auto& field : group.fields) {
        field.box->interpretText();
        if (!field.known && !field.edited) {
            group.feedback->setText(QStringLiteral("请填写标出的本机标定值。"));
            return;
        }
        if (!field.known || field.box->value() != field.original) values.insert(field.key, field.box->value());
    }
    if (values.isEmpty()) {
        group.feedback->setText(QStringLiteral("参数无需保存。"));
        return;
    }
    group.pending = true;
    update_measurement_controls(laser_);
    update_measurement_controls(spectrometer_);
    group.feedback->setText(QStringLiteral("正在保存…"));
    emit save_requested({{"group", group.name}, {"values", values}});
}

void ParameterPage::finish_save(const QString& name, bool ok, const QString& message, const QStringList& failed_fields) {
    auto* group = measurement_group(name);
    if (!group) return;
    if (ok) group->refresh_after_ms = QDateTime::currentMSecsSinceEpoch();
    for (auto& field : group->fields) {
        if (ok) {
            field.original = field.box->value();
            field.known = true;
            field.edited = false;
            field.error->hide();
        } else if (failed_fields.contains(field.key)) {
            field.error->setText(QStringLiteral("该参数未通过校验。"));
            field.error->show();
        }
    }
    group->pending = false;
    update_measurement_controls(laser_);
    update_measurement_controls(spectrometer_);
    group->feedback->setText(ok ? QStringLiteral("已保存，下次正式检测使用。") : message);
    group->feedback->setToolTip(message);
}

void ParameterPage::finish_reload(const QString& name, const QJsonObject& groups, bool ok, const QString& message) {
    auto* group = measurement_group(name);
    if (!group) return;
    if (ok) {
        group->refresh_after_ms = QDateTime::currentMSecsSinceEpoch();
        apply_values(*group, groups);
    }
    group->pending = false;
    update_measurement_controls(laser_);
    update_measurement_controls(spectrometer_);
    group->feedback->setText(ok ? QStringLiteral("已重新读取。") : message);
}

void ParameterPage::set_air_busy(bool busy) {
    air_.pending = busy;
    set_control_enabled(air_.read, !busy);
    set_control_enabled(air_.save, !busy);
    for (const auto& field : air_.fields) set_control_enabled(field.box->parentWidget(), !busy);
    update_manual_controls();
}

void ParameterPage::show_air_result(const QJsonObject& values, bool ok, const QString& message, bool applied) {
    if (!ok) {
        air_.feedback->setText(message);
        set_air_busy(false);
        return;
    }
    const bool valid = std::all_of(air_.fields.cbegin(), air_.fields.cend(), [&values](const auto& field) {
        const auto value = values.value(field.key);
        return value.isDouble() && value.toDouble() >= field.box->minimum() && value.toDouble() <= field.box->maximum();
    });
    if (valid) {
        for (const auto& field : air_.fields) field.box->setValue(values.value(field.key).toDouble());
        if (!save_air_settings(values)) {
            air_.feedback->setText(QStringLiteral("设备设定已确认，本机缓存失败。"));
            set_air_busy(false);
            return;
        }
    }
    air_.feedback->setText(!valid ? QStringLiteral("设备返回的设定不完整或超出范围。")
        : applied ? QStringLiteral("空调设定已保存，回读一致。") : QStringLiteral("已读取设备设定。"));
    set_air_busy(false);
}

void ParameterPage::load_range_rules() {
    static const QHash<QString, int> defaults{
        {QStringLiteral("empty_belt_mm"), 520}, {QStringLiteral("material_delta_mm"), 20},
        {QStringLiteral("valid_min_mm"), 420}, {QStringLiteral("valid_max_mm"), 500},
    };
    QSettings settings;
    settings.beginGroup(QStringLiteral("rangefinderRules"));
    const bool needs_defaults = settings.childKeys().isEmpty();
    range_rules_ = {};
    for (const auto& field : range_.fields) {
        const int value = needs_defaults ? defaults.value(field.key) : settings.value(field.key).toInt();
        if (value > 0 && value <= field.box->maximum()) {
            field.box->setValue(value);
            range_rules_.insert(field.key, value);
        } else field.box->setValue(0);
    }
    range_.feedback->setText(needs_defaults ? QStringLiteral("已填入参考默认值，请按现场空带和可测距离校准。")
                                            : QStringLiteral("已读取本机保存的判定参数。"));
}

void ParameterPage::save_range_rules() {
    QJsonObject values;
    for (const auto& field : range_.fields) {
        field.box->interpretText();
        if (field.box->value() <= 0) {
            range_.feedback->setText(QStringLiteral("请填写四项判定参数。"));
            return;
        }
        values.insert(field.key, field.box->value());
    }
    if (values.value("valid_min_mm").toDouble() >= values.value("valid_max_mm").toDouble()) {
        range_.feedback->setText(QStringLiteral("可测距离下限须小于上限。"));
        return;
    }
    if (values.value("material_delta_mm").toDouble() >= values.value("empty_belt_mm").toDouble()) {
        range_.feedback->setText(QStringLiteral("来料阈值须小于空带基准。"));
        return;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("rangefinderRules"));
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) settings.setValue(it.key(), it.value().toInt());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        range_.feedback->setText(QStringLiteral("本机保存失败。"));
        return;
    }
    range_rules_ = values;
    range_.feedback->setText(measuring_ ? QStringLiteral("已保存，下次检测使用。") : QStringLiteral("判定参数已保存到本机。"));
    render_range();
}

void ParameterPage::finish_range_sample(const QJsonObject& reading, const QString& error) {
    range_test_pending_ = false;
    if (!error.isEmpty()) {
        range_.manual_feedback->setText(error);
        update_manual_controls();
        return;
    }
    range_reading_ = reading;
    readings_.insert("rangefinder", reading);
    const auto current = reading.value("error").toString();
    if (!current.isEmpty()) range_.manual_feedback->setText(current);
    else if (!reading.value("opened").toBool()) range_.manual_feedback->setText(QStringLiteral("测距仪未打开。"));
    else if (!reading.value("distance_mm").isDouble()) range_.manual_feedback->setText(QStringLiteral("本次距离读数无效。"));
    else range_.manual_feedback->setText(range_summary(draft_values(range_)));
    render_status();
    update_manual_controls();
}

void ParameterPage::render_range() {
    auto* label = status_views_.value(range_.name).readings;
    const auto status = present_device(range_.name, snapshot_, readings_, reading_error_);
    const bool show = status.level == Level::Ok;
    label->setVisible(show);
    if (show) label->setText(range_summary(measuring_ ? active_range_rules_ : range_rules_));
}

QString ParameterPage::range_summary(const QJsonObject& rules) const {
    const auto distance = range_reading_.value("distance_mm");
    if (!distance.isDouble()) return QStringLiteral("当前距离  —");
    QString text = QStringLiteral("当前距离 %1 mm").arg(distance.toDouble());
    if (rules.value("material_delta_mm").toDouble() > 0 &&
        rules.value("empty_belt_mm").toDouble() > rules.value("material_delta_mm").toDouble() &&
        rules.value("valid_min_mm").toDouble() > 0 &&
        rules.value("valid_max_mm").toDouble() > rules.value("valid_min_mm").toDouble()) {
        const double mm = distance.toDouble();
        const bool material = rules.value("empty_belt_mm").toDouble() - mm >= rules.value("material_delta_mm").toDouble();
        const bool in_window = mm >= rules.value("valid_min_mm").toDouble() && mm <= rules.value("valid_max_mm").toDouble();
        text += QStringLiteral("  ·  %1  ·  %2")
            .arg(material ? QStringLiteral("有料") : QStringLiteral("无料"),
                 in_window ? QStringLiteral("距离可测") : QStringLiteral("距离超窗"));
    }
    return text;
}

void ParameterPage::build_lamp_card() {
    using namespace libs::measurement_limits;
    lamp_card_ = new Card(QStringLiteral("三色状态灯"), this);
    lamp_card_->setProperty("deviceGroup", QStringLiteral("indicator"));
    build_status(lamp_card_, QStringLiteral("indicator"));
    lamp_dot_ = new StatusDot(lamp_card_);
    lamp_card_->body()->addWidget(lamp_dot_);
    build_section(lamp_card_, QStringLiteral("手动测试"), QStringLiteral("indicator"));
    auto* fields = new QGridLayout;
    auto* color_label = new QLabel(QStringLiteral("试灯颜色"), lamp_card_);
    auto* mode_label = new QLabel(QStringLiteral("显示方式"), lamp_card_);
    auto* duration_label = new QLabel(QStringLiteral("试灯持续时间"), lamp_card_);
    lamp_color_ = new QComboBox(lamp_card_);
    lamp_color_->setAccessibleName(color_label->text());
    lamp_color_->addItems({QStringLiteral("红灯"), QStringLiteral("黄灯"), QStringLiteral("绿灯"), QStringLiteral("全灭")});
    lamp_mode_ = new QComboBox(lamp_card_);
    lamp_mode_->setAccessibleName(mode_label->text());
    lamp_mode_->addItems({QStringLiteral("常亮"), QStringLiteral("快闪（2 Hz）")});
    lamp_duration_ = new ParameterSpinBox(lamp_card_);
    lamp_duration_->setAccessibleName(duration_label->text());
    lamp_duration_->setDecimals(1);
    lamp_duration_->setSingleStep(0.1);
    lamp_duration_->setRange(kLampTestMinMs / 1000.0, kLampTestMaxMs / 1000.0);
    lamp_duration_->setSuffix(QStringLiteral(" s"));
    lamp_duration_->setKeyboardTracking(false);
    lamp_duration_->setToolTip(QStringLiteral("灯态回读成功后开始计时，到时自动恢复状态灯。"));
    const auto key = QStringLiteral("indicatorTrial/durationMs");
    const auto saved = QSettings().value(key, kLampTestDefaultMs).toUInt();
    lamp_duration_->setValue((saved >= kLampTestMinMs && saved <= kLampTestMaxMs ? saved : kLampTestDefaultMs) / 1000.0);
    connect(lamp_duration_, &QDoubleSpinBox::valueChanged, this, [key](double seconds) {
        QSettings().setValue(key, qRound(seconds * 1000.0));
    });
    fields->addWidget(color_label, 0, 0);
    fields->addWidget(mode_label, 0, 1);
    fields->addWidget(duration_label, 0, 2);
    fields->addWidget(lamp_color_, 1, 0);
    fields->addWidget(lamp_mode_, 1, 1);
    fields->addWidget(lamp_duration_, 1, 2);
    fields->setColumnStretch(0, 1);
    fields->setColumnStretch(1, 1);
    fields->setColumnStretch(2, 1);
    lamp_card_->body()->addLayout(fields);

    auto* footer = new QHBoxLayout;
    lamp_feedback_ = make_hint(QStringLiteral("设备就绪且待机时可试灯，到时自动恢复状态灯。"));
    lamp_feedback_->setTextFormat(Qt::PlainText);
    footer->addWidget(lamp_feedback_, 1);
    lamp_test_ = new QPushButton(QStringLiteral("真机试灯"), lamp_card_);
    lamp_test_->setObjectName(QStringLiteral("ghost"));
    lamp_test_->setFocusPolicy(Qt::TabFocus);
    footer->addWidget(lamp_test_);
    lamp_card_->body()->addLayout(footer);

    connect(lamp_color_, &QComboBox::currentIndexChanged, this, [this] { render_lamp(); });
    connect(lamp_test_, &QPushButton::clicked, this, [this] {
        static const QStringList colors{QStringLiteral("red"), QStringLiteral("yellow"), QStringLiteral("green"), QStringLiteral("off")};
        lamp_duration_->interpretText();
        const int duration_ms = qRound(lamp_duration_->value() * 1000.0);
        lamp_pending_ = true;
        lamp_feedback_->setText(QStringLiteral("正在试灯及确认自动灯态恢复…"));
        render_lamp();
        update_manual_controls();
        emit indicator_test_requested({{"action", "test_indicator"}, {"color", colors.value(lamp_color_->currentIndex())},
            {"mode", lamp_mode_->currentIndex() == 1 ? QStringLiteral("blink-2hz") : QStringLiteral("on")},
            {"duration_ms", duration_ms}});
    });
    render_lamp();
}

void ParameterPage::finish_indicator_test(bool ok, const QString& error) {
    lamp_pending_ = false;
    lamp_feedback_->setText(ok ? QStringLiteral("真机灯态已回读，已恢复自动。") : error);
    render_lamp();
    update_manual_controls();
}

void ParameterPage::render_lamp() {
    const auto* indicator = snapshot_.device(QStringLiteral("indicator"));
    set_control_enabled(lamp_test_, snapshot_.valid && snapshot_.error.isEmpty() && indicator && indicator->opened &&
                           snapshot_.gate.allowed && !measuring_ && !lamp_pending_ && !measurement_command_pending_);
    set_control_enabled(lamp_color_, !lamp_pending_);
    set_control_enabled(lamp_mode_, !lamp_pending_ && lamp_color_->currentIndex() != 3);
    set_control_enabled(lamp_duration_, !lamp_pending_);
    const QString intent = snapshot_.valid ? snapshot_.lamp_intent : QString{};
    if (intent == QLatin1String("ready_green")) lamp_dot_->set_state(Level::Ok, QStringLiteral("自动 · 绿灯"));
    else if (intent == QLatin1String("standby_yellow")) lamp_dot_->set_state(Level::Warn, QStringLiteral("自动 · 黄灯"));
    else if (intent == QLatin1String("fault_red")) lamp_dot_->set_state(Level::Danger, QStringLiteral("自动 · 红灯"));
    else if (intent == QLatin1String("offline_all_off")) lamp_dot_->set_state(Level::Idle, QStringLiteral("自动 · 全灭"));
    else lamp_dot_->set_state(Level::Idle, QStringLiteral("自动 · 灯态未知"));
    lamp_dot_->setToolTip(snapshot_.lamp_error);
}

}  // namespace libs::ui

