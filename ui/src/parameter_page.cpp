#include "parameter_page.hpp"

#include <algorithm>
#include <QDateTime>
#include <QGridLayout>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "widgets.hpp"
#include "libs/measurement_limits.hpp"

namespace libs::ui {
namespace {

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

// laser 组 9 字段，**全部 required**（required_fields() 里逐个点名）。
//
// 这 9 个字段在启动时由后端按**真机默认值**播种（只补缺失），默认值取自
// docs/real-device-parameters-20260929.md，所以开箱状态下这里显示的就是
// 真机工作点，而不是量程下限。操作员改过并保存的值永远优先，重启不会打回。
// 后端 commissioned_from_store() 仍按"9 个字段齐备且都在协议量程内"
// 判定是否已标定，界面这边如实呈现。

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
         "协议 0x16：量程 2~500 μs。真机默认 210 μs —— Q 开关靠这个值决定巨脉冲，"
         "取量程下限(2 μs)等于不调 Q、不出光。"},
        {"q_warmup_s", "Q 预热", 0, "s",
         "协议 0x19 未标厂家量程，软件允许 0~3600 s。真机默认 0 s。"},
        {"settle_timeout_ms", "稳定等待超时", 0, "ms",
         "设备从 02「自适应中」走到 01「正在工作」的等待上限。真机默认 20000 ms。"},
        {"emission_hold_ms", "出光保持", 0, "ms",
         "光闸保持打开的时长 = 连续出光窗口。真机默认 3000 ms。"},
        {"min_discharges", "最小放电次数", 0, "次",
         "至少放这么多次电才算出光。真机默认 1 次。计数器不涨 = 没放电 = 失败。"},
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
         "等 pin6 触发的超时，必须覆盖整个出光窗口。真机默认 10000 ms。"},
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

const Level lamp_levels[] = {Level::Danger, Level::Warn, Level::Ok, Level::Idle};

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
    auto* scroll = new QScrollArea(this);
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
    build_group(laser_, QStringLiteral("激光器参数"), specs, count);
    spectrometer_.name = QStringLiteral("spectrometer");
    specs = spectrometer_specs(count);
    build_group(spectrometer_, QStringLiteral("光谱仪参数"), specs, count);
    air_.name = QStringLiteral("air_conditioner");
    specs = air_specs(count);
    build_group(air_, QStringLiteral("空调参数"), specs, count);
    range_.name = QStringLiteral("rangefinder");
    specs = range_specs(count);
    build_group(range_, QStringLiteral("测距仪参数"), specs, count);
    build_lamp_card();

    grid->addWidget(laser_.card, 0, 0, Qt::AlignTop);
    grid->addWidget(air_.card, 0, 1, Qt::AlignTop);
    grid->addWidget(spectrometer_.card, 1, 0, Qt::AlignTop);
    grid->addWidget(range_.card, 1, 1, 2, 1, Qt::AlignTop);
    grid->addWidget(lamp_card_, 2, 0, Qt::AlignTop);
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
    air_.read->setText(QStringLiteral("读取设定"));
    air_.save->setText(QStringLiteral("保存并回读"));
    air_.feedback->setText(QStringLiteral("读取设定后可编辑。"));
    connect(air_.read, &QPushButton::clicked, this, [this] {
        set_air_busy(true);
        air_.feedback->setText(QStringLiteral("正在读取空调设定…"));
        emit air_read_requested();
    });
    connect(air_.save, &QPushButton::clicked, this, [this] {
        QJsonObject values;
        for (const auto& field : air_.fields) {
            field.box->interpretText();
            values.insert(field.key, field.box->value());
        }
        set_air_busy(true);
        air_.feedback->setText(QStringLiteral("正在保存并核对…"));
        emit air_apply_requested(values);
    });
    set_air_busy(false);

    range_.read->setText(QStringLiteral("刷新读数"));
    range_result_ = make_hint(QStringLiteral("等待读取距离。"));
    range_.card->body()->insertWidget(1, range_result_);
    connect(range_.read, &QPushButton::clicked, this, [this] {
        range_.read->setEnabled(false);
        emit range_read_requested();
    });
    connect(range_.save, &QPushButton::clicked, this, &ParameterPage::save_range_rules);
    load_range_rules();
    render_range();
}

void ParameterPage::build_group(Group& group, const QString& title, const FieldSpec* specs, std::size_t count) {
    group.card = new Card(title, this);
    group.card->setProperty("deviceGroup", group.name);
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
    group.read->setAccessibleName(QStringLiteral("读取") + title);
    footer->addWidget(group.read);
    group.save = new QPushButton(QStringLiteral("保存参数"), group.card);
    group.save->setObjectName(QStringLiteral("primary"));
    group.save->setAccessibleName(QStringLiteral("保存") + title);
    footer->addWidget(group.save);
    group.card->body()->addLayout(footer);
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
    if (measuring_ && lamp_test_) {
        lamp_test_ = false;
        lamp_timer_->stop();
        lamp_feedback_->setText(QStringLiteral("检测开始，已退出试灯预览。"));
    }
    render_lamp();
    render_range();
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
        field.box->parentWidget()->setEnabled(enabled && field.loaded);
    }
    const bool can_request = enabled && !laser_.pending && !spectrometer_.pending;
    group.read->setEnabled(can_request);
    group.save->setEnabled(can_request && !group.fields.isEmpty() && group.fields.front().loaded);
    if (measuring_ && !group.pending) group.feedback->setText(QStringLiteral("检测中，停止后可修改。"));
    else if (group.feedback->text() == QStringLiteral("检测中，停止后可修改。")) group.feedback->clear();
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
    group->feedback->setText(ok ? QStringLiteral("已保存。") : message);
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
    air_.read->setEnabled(!busy);
    air_.save->setEnabled(!busy && air_ready_);
    for (const auto& field : air_.fields) {
        field.box->setSpecialValueText(air_ready_ ? QString{} : QStringLiteral("—"));
        if (!air_ready_) field.box->setValue(field.box->minimum());
        field.box->parentWidget()->setEnabled(!busy && air_ready_);
    }
}

void ParameterPage::show_air_result(const QJsonObject& values, bool ok, const QString& message, bool applied) {
    if (!ok) {
        if (!applied) air_ready_ = false;
        air_.feedback->setText(message);
        set_air_busy(false);
        return;
    }
    air_ready_ = std::all_of(air_.fields.cbegin(), air_.fields.cend(), [&values](const auto& field) {
        const auto value = values.value(field.key);
        return value.isDouble() && value.toDouble() >= field.box->minimum() && value.toDouble() <= field.box->maximum();
    });
    if (air_ready_) {
        for (const auto& field : air_.fields) field.box->setValue(values.value(field.key).toDouble());
    }
    air_.feedback->setText(!air_ready_ ? QStringLiteral("设备返回的设定不完整或超出范围。")
        : applied ? QStringLiteral("空调设定已保存，回读一致。") : QStringLiteral("已读取设备设定。"));
    set_air_busy(false);
}

void ParameterPage::load_range_rules() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("rangefinderRules"));
    for (const auto& field : range_.fields) {
        const int value = settings.value(field.key).toInt();
        if (value > 0 && value <= field.box->maximum()) {
            field.box->setValue(value);
            range_rules_.insert(field.key, value);
        }
    }
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

void ParameterPage::apply_range_reading(const QJsonObject& reading) {
    range_reading_ = reading;
    range_.read->setEnabled(true);
    render_range();
}

void ParameterPage::render_range() {
    const auto distance = range_reading_.value("distance_mm");
    const auto error = range_reading_.value("error").toString();
    if (!distance.isDouble() || !snapshot_.valid) {
        const QString status = !error.isEmpty() ? QStringLiteral("读取失败")
            : range_reading_.value("opened").isBool() && !range_reading_.value("opened").toBool()
                ? QStringLiteral("设备未打开") : QStringLiteral("—");
        range_result_->setText(QStringLiteral("当前距离 · %1").arg(status));
        range_result_->setToolTip(error);
        return;
    }
    QString text = QStringLiteral("当前距离 %1 mm").arg(distance.toDouble());
    const auto& rules = measuring_ ? active_range_rules_ : range_rules_;
    if (rules.size() == 4) {
        const double mm = distance.toDouble();
        const bool material = rules.value("empty_belt_mm").toDouble() - mm >= rules.value("material_delta_mm").toDouble();
        const bool in_window = mm >= rules.value("valid_min_mm").toDouble() && mm <= rules.value("valid_max_mm").toDouble();
        text += QStringLiteral("  ·  %1  ·  %2")
            .arg(material ? QStringLiteral("有料") : QStringLiteral("无料"),
                 in_window ? QStringLiteral("距离可测") : QStringLiteral("距离超窗"));
    }
    range_result_->setText(text);
    range_result_->setToolTip(QStringLiteral("判定预览用于调试。当前测试流程由操作员启停。"));
}

void ParameterPage::build_lamp_card() {
    lamp_card_ = new Card(QStringLiteral("状态灯参数"), this);
    lamp_dot_ = new StatusDot(lamp_card_);
    lamp_card_->add_header_widget(lamp_dot_);
    auto* fields = new QGridLayout;
    auto* color_label = new QLabel(QStringLiteral("试灯颜色"), lamp_card_);
    auto* mode_label = new QLabel(QStringLiteral("显示方式"), lamp_card_);
    lamp_color_ = new QComboBox(lamp_card_);
    lamp_color_->setAccessibleName(color_label->text());
    lamp_color_->addItems({QStringLiteral("红灯"), QStringLiteral("黄灯"), QStringLiteral("绿灯"), QStringLiteral("全灭")});
    lamp_mode_ = new QComboBox(lamp_card_);
    lamp_mode_->setAccessibleName(mode_label->text());
    lamp_mode_->addItems({QStringLiteral("常亮"), QStringLiteral("快闪（2 Hz）")});
    fields->addWidget(color_label, 0, 0);
    fields->addWidget(mode_label, 0, 1);
    fields->addWidget(lamp_color_, 1, 0);
    fields->addWidget(lamp_mode_, 1, 1);
    fields->setColumnStretch(0, 1);
    fields->setColumnStretch(1, 1);
    lamp_card_->body()->addLayout(fields);

    auto* footer = new QHBoxLayout;
    lamp_feedback_ = make_hint(QStringLiteral("在界面中预览灯态。"));
    footer->addWidget(lamp_feedback_, 1);
    lamp_restore_ = new QPushButton(QStringLiteral("恢复自动"), lamp_card_);
    lamp_restore_->setObjectName(QStringLiteral("ghost"));
    footer->addWidget(lamp_restore_);
    lamp_preview_ = new QPushButton(QStringLiteral("预览试灯"), lamp_card_);
    lamp_preview_->setObjectName(QStringLiteral("ghost"));
    footer->addWidget(lamp_preview_);
    auto* save = new QPushButton(QStringLiteral("保存参数"), lamp_card_);
    save->setObjectName(QStringLiteral("primary"));
    save->setAccessibleName(QStringLiteral("保存状态灯试灯参数"));
    footer->addWidget(save);
    lamp_card_->body()->addLayout(footer);

    QSettings settings;
    lamp_color_->setCurrentIndex(qBound(0, settings.value(QStringLiteral("lampTrial/color"), 0).toInt(), 3));
    lamp_mode_->setCurrentIndex(qBound(0, settings.value(QStringLiteral("lampTrial/mode"), 0).toInt(), 1));
    lamp_timer_ = new QTimer(this);
    lamp_timer_->setInterval(250);
    connect(lamp_timer_, &QTimer::timeout, this, [this] { lamp_phase_ = !lamp_phase_; render_lamp(); });
    connect(lamp_color_, &QComboBox::currentIndexChanged, this, [this] { render_lamp(); });
    connect(lamp_restore_, &QPushButton::clicked, this, [this] {
        lamp_test_ = false;
        lamp_timer_->stop();
        lamp_feedback_->setText(QStringLiteral("已退出试灯预览。"));
        render_lamp();
    });
    connect(lamp_preview_, &QPushButton::clicked, this, [this] {
        lamp_test_ = true;
        lamp_phase_ = true;
        lamp_test_color_ = lamp_color_->currentIndex();
        if (lamp_mode_->currentIndex() == 1 && lamp_test_color_ != 3) lamp_timer_->start();
        else lamp_timer_->stop();
        lamp_feedback_->setText(QStringLiteral("正在预览试灯。"));
        render_lamp();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        QSettings state;
        state.setValue(QStringLiteral("lampTrial/color"), lamp_color_->currentIndex());
        state.setValue(QStringLiteral("lampTrial/mode"), lamp_mode_->currentIndex());
        state.sync();
        lamp_feedback_->setText(state.status() == QSettings::NoError
            ? QStringLiteral("试灯预设已保存到本机。") : QStringLiteral("本机保存失败。"));
    });
    render_lamp();
}

void ParameterPage::render_lamp() {
    lamp_restore_->setVisible(lamp_test_);
    lamp_preview_->setEnabled(!measuring_);
    lamp_mode_->setEnabled(lamp_color_->currentIndex() != 3);
    if (lamp_test_) {
        const auto name = lamp_color_->itemText(lamp_test_color_);
        lamp_dot_->set_state(lamp_phase_ ? lamp_levels[lamp_test_color_] : Level::Idle,
            QStringLiteral("试灯 · %1").arg(name));
        return;
    }
    const QString intent = snapshot_.valid ? snapshot_.lamp_intent : QString{};
    if (intent == QLatin1String("ready_green")) lamp_dot_->set_state(Level::Ok, QStringLiteral("自动 · 绿灯"));
    else if (intent == QLatin1String("standby_yellow")) lamp_dot_->set_state(Level::Warn, QStringLiteral("自动 · 黄灯"));
    else if (intent == QLatin1String("fault_red")) lamp_dot_->set_state(Level::Danger, QStringLiteral("自动 · 红灯"));
    else if (intent == QLatin1String("offline_all_off")) lamp_dot_->set_state(Level::Idle, QStringLiteral("自动 · 全灭"));
    else lamp_dot_->set_state(Level::Idle, QStringLiteral("自动 · 灯态未知"));
    lamp_dot_->setToolTip(snapshot_.lamp_error);
}

}  // namespace libs::ui

