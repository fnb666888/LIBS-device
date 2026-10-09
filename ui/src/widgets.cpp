#include "widgets.hpp"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include "theme.hpp"

namespace libs::ui {

// 圆点自绘：同色低透明外圈 + 实心内核。
// 用 QLabel + border-radius 也能画圆，但要让它做呼吸动画就得每帧改颜色，
// 走 QPainter + QTimer 更直接，也好控制相位。
class DotWidget : public QWidget {   // 定义在本cpp，头文件只做前置声明
public:
    explicit DotWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(14, 14);
    }
    void set_color(const QColor& c) {
        color_ = c;
        update();
    }
    void set_opacity(double a) {
        alpha_ = a;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor halo = color_;
        halo.setAlphaF(alpha_ * 0.25);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawRoundedRect(QRectF(0.5, 0.5, 13.0, 13.0), 6.5, 6.5);
        QColor core = color_;
        core.setAlphaF(alpha_);
        p.setBrush(core);
        p.drawEllipse(QRectF(4.0, 4.0, 6.0, 6.0));
    }

private:
    QColor color_;
    double alpha_ = 1.0;
};


QColor color_for(Level level) {
    switch (level) {
        case Level::Ok:     return theme::color::ok();
        case Level::Warn:   return theme::color::warn();
        case Level::Danger: return theme::color::danger();
        case Level::Idle:   break;
    }
    return theme::color::idle();
}

QString level_key(Level level) {
    switch (level) {
        case Level::Ok:     return QStringLiteral("ok");
        case Level::Warn:   return QStringLiteral("warn");
        case Level::Danger: return QStringLiteral("danger");
        case Level::Idle:   break;
    }
    return QStringLiteral("idle");
}

QLabel* make_section_title(const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("cardTitle"));
    return l;
}

QLabel* make_hint(const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("cardHint"));
    l->setWordWrap(true);
    return l;
}

QLabel* make_divider() {
    auto* l = new QLabel;
    l->setObjectName(QStringLiteral("sectionDivider"));
    l->setFixedHeight(1);
    return l;
}

StatusDot::StatusDot(QWidget* parent) : QWidget(parent) {
    dot_ = new DotWidget(this);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("dim"));
    detail_ = new QLabel(this);
    detail_->setObjectName(QStringLiteral("cardHint"));
    detail_->hide();
    // 详情默认不换行：顶栏高度固定，换行会把标题行撑高，把下面的页面挤下去。
    // 要看全文就悬停看 tooltip。
    detail_->setWordWrap(false);
    detail_->setToolTip(QString());   // 原文只在真被截断时才填

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(7);
    row->addWidget(dot_, 0, Qt::AlignVCenter);
    row->addWidget(text_, 1);
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(1);
    col->setAlignment(Qt::AlignVCenter);
    col->addLayout(row);
    detail_->setContentsMargins(21, 0, 0, 0);
    col->addWidget(detail_);

    pulse_timer_ = new QTimer(this);
    pulse_timer_->setInterval(45);   // ~22fps：够看出在闪，又不跟帧率抢 CPU
    connect(pulse_timer_, &QTimer::timeout, this, &StatusDot::apply_pulse_phase);
    set_level(Level::Idle);
}

void StatusDot::apply_pulse_phase() {
    pulse_phase_ = (pulse_phase_ + 1) % 44;
    // 三角波 0.35 -> 1.0 -> 0.35：比方波柔和，不像故障指示灯那么刺眼。
    const double t = pulse_phase_ < 22 ? pulse_phase_ / 22.0 : (44 - pulse_phase_) / 22.0;
    dot_->set_opacity(0.35 + 0.65 * t);
}

void StatusDot::set_state(Level level, const QString& text, const QString& detail) {
    set_level(level);
    set_text(text);
    set_detail(detail);
}

void StatusDot::set_level(Level level) {
    level_ = level;
    dot_->set_color(color_for(level));
}

void StatusDot::set_text(const QString& text) { text_->setText(text); }

void StatusDot::set_detail(const QString& detail) {
    detail_full_ = detail;
    refresh_detail_text();
    detail_->setVisible(!detail_full_.isEmpty());
}

void StatusDot::set_max_width(int px) {
    if (px > 0) setMaximumWidth(px);
    refresh_detail_text();
}

void StatusDot::refresh_detail_text() {
    if (detail_full_.isEmpty()) {
        detail_->setText(QString());
        detail_->setToolTip(QString());
        return;
    }
    // 上限宽度扣掉圆点(14) + 间距(7)：StatusDot 自身占的固定宽度不算在里面。
    const int budget = maximumWidth() - 14 - 7;
    const QFontMetrics fm(detail_->font());
    if (budget > 0 && fm.horizontalAdvance(detail_full_) > budget) {
        detail_->setText(fm.elidedText(detail_full_, Qt::ElideRight, budget));
        detail_->setToolTip(detail_full_);   // 截断了才给 tooltip
    } else {
        detail_->setText(detail_full_);
        detail_->setToolTip(QString());
    }
}

void StatusDot::set_pulsing(bool pulsing) {
    if (pulsing) {
        if (!pulse_timer_->isActive()) pulse_timer_->start();
        pulse_phase_ = 0;
    } else {
        pulse_timer_->stop();
        pulse_phase_ = 0;
        dot_->set_opacity(1.0);
    }
}

Card::Card(const QString& title, QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("card"));

    header_ = new QHBoxLayout;
    header_->setContentsMargins(0, 0, 0, 0);
    header_->setSpacing(8);
    auto* title_label = new QLabel(title, this);
    title_label->setObjectName(QStringLiteral("cardTitle"));
    header_->addWidget(title_label);
    header_->addStretch(1);   // add_header_widget 靠这个把控件推到最右

    outer_ = new QVBoxLayout(this);
    outer_->setContentsMargins(20, 18, 20, 18);
    outer_->setSpacing(14);
    outer_->addLayout(header_);

    body_ = new QVBoxLayout;
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(8);
    outer_->addLayout(body_, 1);
}

void Card::add_header_widget(QWidget* w) {
    if (!header_ || header_->count() == 0) return;
    // 插到 stretch 之前，控件就落在标题行最右侧。
    header_->insertWidget(header_->count() - 1, w);
}

void Card::set_hint(const QString& hint) {
    auto* l = new QLabel(hint, this);
    l->setObjectName(QStringLiteral("cardHint"));
    l->setWordWrap(true);
    body_->addWidget(l);
}

Metric::Metric(const QString& caption_text, QWidget* parent) : QWidget(parent) {
    caption_ = new QLabel(caption_text, this);
    caption_->setObjectName(QStringLiteral("caption"));

    value_ = new QLabel(QStringLiteral("—"), this);   // 初始就是"未知"，不是 0
    value_->setObjectName(QStringLiteral("valueStrong"));

    unit_ = new QLabel(this);
    unit_->setObjectName(QStringLiteral("valueUnit"));
    unit_->hide();

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(5);
    row->addWidget(value_, 0, Qt::AlignBottom);
    row->addWidget(unit_, 0, Qt::AlignBottom);
    row->addStretch(1);

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(2);
    col->addWidget(caption_);
    col->addLayout(row);
}

void Metric::set_value(const QString& text, Level level) {
    value_->setText(text);
    value_->setStyleSheet(QStringLiteral("color:%1;").arg(color_for(level).name()));
}

void Metric::set_unit(const QString& unit) {
    unit_->setText(unit);
    unit_->setVisible(!unit.isEmpty());
}

void Metric::set_unavailable(const QString& why) {
    // 灰掉 + 破折号。why 有话时挂到 title 上，鼠标悬停能读到原因。
    value_->setText(QStringLiteral("—"));
    value_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::color::idle().name()));
    if (!why.isEmpty()) {
        value_->setToolTip(why);
        setToolTip(why);
    }
}

QString Metric::value() const { return value_->text(); }

}  // namespace libs::ui



