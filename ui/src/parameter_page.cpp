#include "parameter_page.hpp"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QVBoxLayout>

#include "theme.hpp"
#include "widgets.hpp"

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
    QString text = QStringLiteral("%1 ~ %2 %3")
                       .arg(QString::number(lo, 'f', decimals))
                       .arg(QString::number(hi, 'f', decimals))
                       .arg(QString::fromUtf8(unit));
    return text;
}

}  // namespace

// 表定义在**成员函数内部**是有意的：FieldSpec 是 ParameterPage 的 private
// 嵌套类型，类外的匿名命名空间拿不到它的限定名。放进成员函数里既不用把
// private 放开成 public，表也仍然只有这一份 —— 不存在"两份字段名表"这个
// 老问题。

const ParameterPage::FieldSpec* ParameterPage::laser_specs(std::size_t& count) {
    // 量程**逐个抄自后端** `facade.cpp` 的 kRanges 表与
    // `site_config.cpp` 的 validate_laser_fire()，不是抄自设计文档。
    // 以前这里下限全是 0.0，于是界面允许输入 0，后端当场拒绝 ——
    // "界面能填但存不进去"比直接禁掉更难查。
    //
    // 上下限收紧还有一层用意：未标定框会被推到本字段下限（见 apply_values），
    // 下限非 0 的字段因此一眼就能看出"这里要填数"，而不会是 0。
    static const FieldSpec kLaserFields[] = {
        {"discharge_current_a", "放电电流", 15.0, 100.0, 1, "A",
         "协议 0x11：量程 15~100 A。真机默认 80 A。超过 30 A 属高功率区，提交后二次确认。"},
        {"pulse_width_us", "放电脉宽", 50.0, 300.0, 0, "μs",
         "协议 0x12：量程 50~300 μs。真机默认 200 μs。"},
        {"frequency_hz", "放电频率", 1.0, 10.0, 0, "Hz",
         "协议 0x13：量程 1~10 Hz。真机默认 1 Hz。"},
        {"discharge_delay_us", "放电延时", 0.0, 500.0, 0, "μs",
         "协议 0x14：量程 0~500 μs。真机默认 0 μs。"},
        {"q_delay_us", "Q 延时", 2.0, 500.0, 0, "μs",
         "协议 0x16：量程 2~500 μs。真机默认 210 μs —— Q 开关靠这个值决定巨脉冲，"
         "取量程下限(2 μs)等于不调 Q、不出光。"},
        {"q_warmup_s", "Q 预热", 0.0, 3600.0, 0, "s",
         "协议 0x19（未标量程）。真机默认 0 s。Q 开关接通后的等待时间。"},
        {"settle_timeout_ms", "稳定等待超时", 1.0, 600000.0, 0, "ms",
         "设备从 02「自适应中」走到 01「正在工作」的等待上限。真机默认 20000 ms。"},
        {"emission_hold_ms", "出光保持", 0.0, 600000.0, 0, "ms",
         "光闸保持打开的时长 = 连续出光窗口。真机默认 3000 ms。"},
        {"min_discharges", "最小放电次数", 1.0, 100000.0, 0, "次",
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
        {"integration_ms", "积分时间", 0.002, 60000.0, 3, "ms",
         "驱动硬校验 0.002~60000 ms。超范围后端直接拒绝。真机默认 1.05 ms。"},
        {"capture_timeout_ms", "采集超时", 1.0, 600000.0, 0, "ms",
         "等 pin6 触发的超时，必须覆盖整个出光窗口。真机默认 10000 ms。"},
        {"integration_delay_ns", "积分延时", 0.0, 1000000.0, 0, "ns",
         "收到激光 TTL 后延迟多久开始积分。真机默认 1280 ns；每个样品要重新标定。"},
    };
    count = sizeof(kSpectrometerFields) / sizeof(FieldSpec);
    return kSpectrometerFields;
}



ParameterPage::ParameterPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("parameterPage"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);

    // 参数齐备状态条。这一条直接决定门禁里的 parameters_incomplete，
    // 所以必须**如实显示**，包括"还不知道"（param_get 这一轮失败）。
    readiness_dot_ = new StatusDot(this);
    readiness_dot_->set_state(Level::Idle, QStringLiteral("参数齐备状态未知"));
    readiness_text_ = make_hint(QStringLiteral("尚未从守护进程读到参数齐备状态。"));
    outer->addWidget(readiness_dot_);
    outer->addWidget(readiness_text_);

    // 表单放进滚动区：12 个字段 + 两个按钮在 1180x720 的屏上放不下，
    // 而"放不下就得拖窗口"在工控触摸屏上不可接受。
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget(scroll);
    auto* form = new QVBoxLayout(host);
    form->setContentsMargins(0, 0, 8, 0);
    form->setSpacing(10);

    Group laser;
    laser.name = QStringLiteral("laser");
    laser.title = QStringLiteral("激光器参数");
    build_group(laser);
    form->addWidget(laser.card);

    Group spec;
    spec.name = QStringLiteral("spectrometer");
    spec.title = QStringLiteral("光谱仪参数");
    build_group(spec);
    form->addWidget(spec.card);

    form->addStretch(1);
    groups_ = {laser, spec};   // Group 持的是指针，值拷贝安全

    scroll->setWidget(host);
    outer->addWidget(scroll, 1);

    // 保存结果行。常驻，不是弹窗：一次改多个字段时，结果要能一直看着。
    save_result_ = make_hint(QStringLiteral(
        "参数改动立即生效。测量进行中保存，下一帧按新参数采集；"
        "正在出光的那一次仍用它启动时读到的值。"));
    save_result_->setWordWrap(true);
    outer->addWidget(save_result_);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(10);

    // 放弃修改 = 回读。理由：参数可能在别的终端被改过，
    // 界面不能拿自己开窗时的值当真值。
    discard_btn_ = new QPushButton(QStringLiteral("放弃修改（重新读取）"), this);
    discard_btn_->setObjectName(QStringLiteral("ghost"));
    discard_btn_->setToolTip(QStringLiteral(
        "丢弃本页未保存的输入，回到守护进程里参数库当前的取值。"));
    connect(discard_btn_, &QPushButton::clicked, this, &ParameterPage::on_discard);
    actions->addWidget(discard_btn_);

    actions->addStretch(1);

    save_btn_ = new QPushButton(QStringLiteral("保存"), this);
    save_btn_->setObjectName(QStringLiteral("primary"));
    save_btn_->setMinimumWidth(140);
    save_btn_->setToolTip(QStringLiteral(
        "把本页改动过的字段发给守护进程。改动立即生效。"));
    connect(save_btn_, &QPushButton::clicked, this, &ParameterPage::on_save);
    actions->addWidget(save_btn_);

    outer->addLayout(actions);
}

void ParameterPage::build_group(Group& g) {
    g.card = new Card(g.title, this);
    g.dot = new StatusDot(g.card);
    g.dot->set_state(Level::Idle, QStringLiteral("未修改"));
    g.card->add_header_widget(g.dot);

    std::size_t count = 0;
    const FieldSpec* specs = (g.name == QLatin1String("laser"))
                                 ? laser_specs(count)
                                 : spectrometer_specs(count);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 1);

    for (std::size_t i = 0; i < count; ++i) {
        const FieldSpec& spec = specs[i];

        // 中文裸字面量必须按 UTF-8 解码。用 QLatin1String 会按 latin1 逐字节
        // 取值，一个汉字的 3 个字节变成 3 个孤立字符 —— 界面上就是"锟斤拷"式乱码。
        // key/unit 是纯 ASCII，UTF-8 解码结果与原串相同，所以一并换掉。
        auto* label = new QLabel(QString::fromUtf8(spec.label), g.card);
        label->setObjectName(QStringLiteral("dim"));

        Field f;
        f.key = QString::fromUtf8(spec.key);
        f.range_text = fmt_range(spec.lo, spec.hi, spec.decimals, spec.unit);

        f.box = new QDoubleSpinBox(g.card);
        f.box->setDecimals(spec.decimals);
        f.box->setRange(spec.lo, spec.hi);
        // 不用 setSingleStep 的缺省 1：量程小到 0.002 ms 的字段
        // （积分时间）点一下就跳 1 ms，等于一步跳出量程。
        const double span = spec.hi - spec.lo;
        f.box->setSingleStep(span <= 10.0 ? 0.001 : 1.0);
        f.box->setKeyboardTracking(false);   // 输入中途不去后端查，回车/失焦才生效
        f.box->setToolTip(QString::fromUtf8(spec.tooltip));
        f.box->setMinimumWidth(150);

        f.error = new QLabel(g.card);
        f.error->setObjectName(QStringLiteral("dim"));
        f.error->setStyleSheet(QStringLiteral("color:%1;").arg(theme::color::danger().name()));
        f.error->hide();   // 有错才出现

        // 量程提示挂在标签下方（两行标签）。**不是**塞进第三列 ——
        // 那一列归 f.error（后端拒绝原因）所有，两个提示抢一列会互相顶掉。
        // range_text 以前只写不读，是死数据，于是界面上从来没显示过量程，
        // 而卡片说明还在承诺"右侧显示该字段的可输入范围"。
        auto* label_box = new QWidget(g.card);
        auto* label_layout = new QVBoxLayout(label_box);
        label_layout->setContentsMargins(0, 0, 0, 0);
        label_layout->setSpacing(1);
        label->setParent(label_box);
        label_layout->addWidget(label);
        auto* range_label = new QLabel(f.range_text, label_box);
        range_label->setObjectName(QStringLiteral("caption"));
        range_label->setToolTip(QStringLiteral("可输入范围，取自协议量程"));
        label_layout->addWidget(range_label);
        grid->addWidget(label_box, static_cast<int>(i), 0);
        grid->addWidget(f.box, static_cast<int>(i), 1);
        grid->addWidget(f.error, static_cast<int>(i), 2);

        connect(f.box, &QDoubleSpinBox::valueChanged, this, &ParameterPage::on_field_changed);
        g.fields.push_back(f);
    }

    // 字段右侧写死量程提示：现场最常见的事是把值改到协议量程外，
    // 然后困惑于"为什么后端说 parameter_field_bad"。提示写在旁边，
    // 就不必去翻协议手册。
    g.card->body()->addLayout(grid);
    g.card->set_hint(QStringLiteral("改动立即生效。字段名下方的范围即该字段的可输入范围。"));
}

void ParameterPage::apply_snapshot(const MachineSnapshot& s) {
    if (s.readiness_known) render_summary(s);
    if (s.param_values.isEmpty()) return;

    // **正在输入时不覆盖。** 轮询 800ms 一次，用户还没输完就被抹掉，
    // 是这种"参数页配一个状态轮询"最典型的翻车方式。
    if (has_pending_changes() || busy()) return;
    apply_values(s.param_values);
}

void ParameterPage::apply_values(const QJsonObject& groups, bool force) {
    // 守卫：正在输入时不许覆盖。轮询 800ms 一次，
    // 用户还没输完就被抹掉，是「参数页配一个状态轮询」最典型的翻车方式。
    // force 只给「放弃修改」用 —— 那正是这个按钮要的行为。
    if (!force && (has_pending_changes() || busy())) return;
    for (Group& g : groups_) {
        // 注意：**不能**因为该组在响应里缺失就 `continue` 跳过整组。
        //
        // 方案 B 之后 laser 组不会被播种，所以"整组不存在"就是它的
        // 常态 —— 而那恰恰是最需要提示的字段。曾经写 `if (entry.isEmpty())
        // continue;`，结果 laser 组既不铺值、也不标红、状态点还停在
        // "未修改"，操作员看到的是"一排上下限 + 一个没动过的状态点"，
        // 完全不知道这 9 个字段根本没被标定。
        // 缺组与缺字段在这里走同一条路：都当成"没有"处理。
        const QJsonObject entry = groups.value(g.name).toObject();
        const QJsonObject values = entry.value("values").toObject();

        for (Field& f : g.fields) {
            const QJsonValue v = values.value(f.key);
            if (v.isDouble()) {
                f.original = v.toDouble();
                f.known = true;
                // loaded_once 要**在** setValue 之前落定：setValue 会触发
                // valueChanged -> on_field_changed -> has_pending_changes()，
                // 那时如果 loaded_once 还是 false，这个自己造成的变化会被
                // 当成"用户正在输入"，第一帧就被挡回去。
                f.loaded_once = true;
                set_suppress_events_(true);
                f.box->setValue(f.original);
                set_suppress_events_(false);
                f.error->clear();
                f.error->hide();
            } else {
                // 参数库里**没有**这个字段。这是"缺"，不是 0。
                //
                // 以前这里只把右侧提示标签点亮，输入框**仍停在 0** ——
                // 于是界面上六个框全是 0，看着像"把出光电流设成了 0"。
                // QDoubleSpinBox 没有"空"状态，它的值永远是个数，所以
                // 未标定的框一律**推到下限**并标黄提示：
                //   * discharge_delay_us / q_warmup_s 这类本就允许 0 的字段，
                //     下限就是 0，看起来仍是 0，但旁边明写"未标定"；
                //   * discharge_current_a 这类下限非 0 的字段，推到下限会被
                //     后端 param_out_of_range 当场拒，操作员立刻知道要填。
                // 关键：不能让它停在 0 上还看起来像个有效值。
                f.known = false;
                f.loaded_once = true;
                set_suppress_events_(true);
                f.box->setValue(f.box->minimum());
                set_suppress_events_(false);
                f.error->setText(QString::fromUtf8(
                    "参数库里还没有这个字段（未标定），请填写后保存。\n"
                    "缺任意一个出光参数都会让门禁判 parameters_incomplete。"));
                f.error->show();
            }
        }
        if (g.dot) {
            g.dot->set_state(Level::Idle, QStringLiteral("已读取"));
        }
    }
}

bool ParameterPage::has_pending_changes() const {
    for (const Group& g : groups_) {
        for (const Field& f : g.fields) {
            // 还没铺过守护进程数据：**不算**改动。见 loaded_once 的说明 ——
            // 这条判据是"第一次 param_get 能铺上去"的前提，去掉它整页会
            // 停在量程下限（曾经真的这样）。
            if (!f.loaded_once) continue;
            // 未标定字段（!known）一律算有改动：QDoubleSpinBox 停在量程下限，
            // 而 original 是 0.0。若只比数值，操作员在"下限恰好是 0"的字段
            // （放电延时 / Q 预热）里填了 0，页面会以为没改动而拒绝保存 ——
            // 而 0 恰恰是这些字段的合法真实值。
            if (!f.known) return true;
            if (f.box->value() != f.original) return true;
        }
    }
    return false;
}

// 「本页是否还有该由操作员去填的空框」。
//
// 这个判定**刻意与 has_pending_changes() 分开**，因为两者回答的是不同问题：
//   * has_pending_changes() = "用户正在输入吗" —— 用来挡住轮询覆盖；
//   * has_uncalibrated_fields() = "有该填还没填的框吗" —— 用来提示。
//
// 曾经用同一个函数回答两个问题，后果是：未标定的 laser 组恒让
// has_pending_changes() 为 true，于是 apply_snapshot 每轮都被挡，
// **光谱组那三个有真实值的框也铺不上去**，整页停在量程下限（0.002 / 1 / 0）。
// 现场看过去就是"参数全是下限"，实际是"一个真实值都没显示"。
bool ParameterPage::has_uncalibrated_fields() const {
    for (const Group& g : groups_) {
        for (const Field& f : g.fields) {
            // 还没铺过守护进程数据时 known 一律是 false，那是"还没读"，
            // 不是"没标定"。少了这道门，第一帧会凭空多出一句"有字段未标定"。
            if (!f.loaded_once) continue;
            if (!f.known) return true;
        }
    }
    return false;
}

QJsonObject ParameterPage::collect_changed(const Group& g) const {
    QJsonObject values;
    for (const Field& f : g.fields) {
        // 有改动才放进去。**不整组覆盖** —— param_set 是立即生效的，
        // 整组覆盖会在测量进行中把没碰过的字段也一起改掉。
        if (!f.known || f.box->value() != f.original) {
            values.insert(f.key, f.box->value());
        }
    }
    return values;
}

void ParameterPage::clear_errors(Group& g) {
    for (Field& f : g.fields) {
        if (f.known) {
            f.error->clear();
            f.error->hide();
        }
    }
}

void ParameterPage::commit_group(Group& g) {
    for (Field& f : g.fields) {
        if (!f.box->isEnabled()) continue;
        f.original = f.box->value();
        f.known = true;
        f.loaded_once = true;   // 保存成功即视为已铺过，别再让"待保存"残留
        f.error->clear();
        f.error->hide();
    }
}

void ParameterPage::set_editing_enabled(bool on) {
    for (Group& g : groups_) {
        for (Field& f : g.fields) f.box->setEnabled(on);
    }
    save_btn_->setEnabled(on);
    discard_btn_->setEnabled(on);
}

void ParameterPage::on_field_changed() {
    if (suppress_events_) return;
    // 有任何改动就点亮"待保存"。不立刻发请求：输 80 要经过
    // "8" -> "80" 两个中间态，每敲一下就往参数库里写一次是灾难。
    const bool dirty = has_pending_changes();
    for (Group& g : groups_) {
        if (g.dot) {
            g.dot->set_state(dirty ? Level::Warn : Level::Idle,
                             dirty ? QStringLiteral("有未保存的改动") : QStringLiteral("已读取"));
        }
    }
}

void ParameterPage::on_save() {
    if (save_in_flight_) return;
    if (!has_pending_changes()) {
        save_result_->setText(QStringLiteral("本页没有改动，不需要保存。"));
        return;
    }

    // 一次只发一个分组，**发一条等一条**：两个分组并发送出去，
    // 后端那边要过 device_mutex，落地顺序不保证，而"哪一组成功了"
    // 与"哪一组失败了"会混在一起分不清。
    Group* target = nullptr;
    for (Group& g : groups_) {
        if (!collect_changed(g).isEmpty()) {
            target = &g;
            break;
        }
    }
    if (target == nullptr) {
        save_result_->setText(QStringLiteral("本页没有改动，不需要保存。"));
        return;
    }

    QJsonObject request;
    request.insert(QStringLiteral("group"), target->name);
    request.insert(QStringLiteral("values"), collect_changed(*target));
    clear_errors(*target);
    save_in_flight_ = true;
    set_editing_enabled(false);
    emit save_requested(request);
}

void ParameterPage::on_discard() {
    if (save_in_flight_ || reloading_) return;
    reloading_ = true;
    set_editing_enabled(false);
    save_result_->setText(QStringLiteral("正在从守护进程重新读取参数……"));
    emit reload_requested();
}

void ParameterPage::on_save_result(bool ok, const QString& summary) {
    save_in_flight_ = false;
    reloading_ = false;
    set_editing_enabled(true);
    save_result_->setText(summary);
    // 保存/回读之后立刻刷一次状态：顶栏的参数齐备指示和门禁快照
    // 都靠它，不刷新的话界面会停在旧状态最长一个轮询周期。
    emit refresh_requested();
    Q_UNUSED(ok)
}

void ParameterPage::render_summary(const MachineSnapshot& s) {
    if (!s.readiness_known) {
        readiness_dot_->set_state(Level::Idle, QStringLiteral("参数齐备状态未知"));
        readiness_text_->setText(QStringLiteral("尚未从守护进程读到参数齐备状态。"));
        return;
    }
    if (s.readiness_ready) {
        readiness_dot_->set_state(Level::Ok, QStringLiteral("参数齐备"));
        readiness_text_->setText(QStringLiteral(
            "必填测量参数已全部就位，可以发起检测。（是否真的出光由业务层判定，界面不预判。）"));
        return;
    }
    readiness_dot_->set_state(Level::Warn, QStringLiteral("参数未齐备"));
    QString detail = QStringLiteral("%1\n缺哪个字段就填哪个字段，保存后立即生效 —— "
                                    "门禁的拦截由业务层负责，界面不替你决定能不能开始。")
                         .arg(s.readiness_summary);
    if (has_uncalibrated_fields()) {
        // 说清"为什么是空的"：这些框不是 0，是参数库里根本没有这个字段。
        // 出光参数是本机标定值，没有通用默认值，后端按方案 B 不再从配置文件
        // 播种 —— 所以开箱状态下 laser 组必然是空的，必须靠操作员在这里标定。
        //
        // 这句是 QStringLiteral 里要显示给人看的正文，所以**不能用** markdown
        // 强调符（界面不解析 markdown，写进去就原样显示成两个星号）。
        detail += QStringLiteral(
            "\n标黄的字段在参数库里还不存在（未标定），输入框里的数是量程下限，"
            "并不是测量值 —— 请逐项填写真实标定值后保存。");
    }
    readiness_text_->setText(detail);
}

}  // namespace libs::ui




