#pragma once

// 复用小控件（`ui/src/widgets.hpp`）。
//
// 为什么单独一个头：主窗口已经够长了，面板布局代码如果和业务逻辑混在
// 一起，改一个卡片样式要翻几百行。这些控件都**不含业务判定**——它们只
// 负责把"某个状态"画成"某个样子"，状态怎么来的由调用方决定。

#include <QColor>
#include <QFrame>
#include <QString>

class QHBoxLayout;
class QLabel;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace libs::ui {

class DotWidget;   // 状态圆点；定义在 widgets.cpp

// 状态级别。刻意只有这四个：**"不确定"没有单独的成功色**。
enum class Level { Ok, Warn, Danger, Idle };

// 一个圆点 + 一行文字（+ 可选说明）的状态行。
// 用于"激光器 已打开 COM10"这类带附加说明的状态。
// 点只表达级别，文字由调用方给足上下文——颜色单独承载不了语义。
class StatusDot : public QWidget {
    Q_OBJECT
public:
    explicit StatusDot(QWidget* parent = nullptr);

    void set_state(Level level, const QString& text, const QString& detail = QString());
    void set_level(Level level);
    void set_text(const QString& text);
    void set_detail(const QString& detail);

    // 让圆点做一次呼吸动画，用于"预热中 / 连接中"这类过渡态。
    void set_pulsing(bool pulsing);
    Level level() const { return level_; }

    // 顶栏空间有限（现场屏 1536px 宽），长错误详情会把右侧按钮挤出可视区。
    // 设上限后详情按宽度省略号截断，完整原文放进 tooltip —— 截断只是让人
    // 看得见，不让人丢掉信息。px <= 0 表示不限宽。
    void set_max_width(int px);

private:
    void apply_pulse_phase();
    void refresh_detail_text();

    Level level_ = Level::Idle;
    class DotWidget* dot_ = nullptr;
    QLabel* text_ = nullptr;
    QLabel* detail_ = nullptr;
    QString detail_full_;   // 未截断的详情原文，截断只发生在显示层
    QTimer* pulse_timer_ = nullptr;
    int pulse_phase_ = 0;
};

// 卡片容器：标题 + 可选副标题 + 内容区。统一圆角、边框、内边距。
class Card : public QFrame {
    Q_OBJECT
public:
    explicit Card(const QString& title, QWidget* parent = nullptr);

    // 追加到标题行右侧的小控件（状态点、"恢复默认"按钮之类）。
    void add_header_widget(QWidget* w);
    void set_hint(const QString& hint);

    QVBoxLayout* body() const { return body_; }

private:
    QVBoxLayout* outer_ = nullptr;
    QHBoxLayout* header_ = nullptr;   // add_header_widget 靠它把控件插到最右
    QVBoxLayout* body_ = nullptr;
};

// 大字数值 + 单位 + 说明。用于"测距 4.5 cm""计数 1240"这类关键读数。
// 单位单独一格是为了让它跟着数值走基线，而不是浮在正中间。
class Metric : public QWidget {
    Q_OBJECT
public:
    explicit Metric(const QString& caption, QWidget* parent = nullptr);

    void set_value(const QString& text, Level level = Level::Idle);
    void set_unit(const QString& unit);
    // 值暂时拿不到时的诚实写法：显示 "—" 而不是上一个数或 0。
    void set_unavailable(const QString& why = QString());
    QString value() const;

private:
    QLabel* value_ = nullptr;
    QLabel* unit_ = nullptr;
    QLabel* caption_ = nullptr;
};

// 按级别取语义色的小工具，集中在这里避免各处硬编码 hex。
QColor color_for(Level level);
QString level_key(Level level);

QLabel* make_section_title(const QString& text);
QLabel* make_hint(const QString& text);
QLabel* make_divider();

}  // namespace libs::ui


