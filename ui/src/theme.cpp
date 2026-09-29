#include "theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QString>

namespace libs::ui::theme {
namespace color {

QColor base()         { return QColor(0x0E, 0x13, 0x19); }
QColor surface()      { return QColor(0x16, 0x1C, 0x23); }
QColor surfaceAlt()   { return QColor(0x1C, 0x24, 0x2D); }
QColor border()       { return QColor(0x26, 0x30, 0x3A); }
QColor borderStrong() { return QColor(0x35, 0x42, 0x50); }
QColor text()         { return QColor(0xE6, 0xED, 0xF3); }
QColor textDim()      { return QColor(0x93, 0xA3, 0xB2); }
QColor textFaint()    { return QColor(0x64, 0x72, 0x80); }
QColor accent()       { return QColor(0x2F, 0x81, 0xF7); }
QColor ok()           { return QColor(0x3F, 0xB9, 0x50); }
QColor warn()         { return QColor(0xD2, 0x99, 0x22); }
QColor danger()       { return QColor(0xF8, 0x51, 0x49); }
QColor idle()         { return QColor(0x6E, 0x7B, 0x8A); }

QColor series(int index) {
    // 与设计方案 §4.1 的四通道配色一致；这里只是把它固化成代码，
    // 免得图例、曲线、状态灯各写一份颜色然后慢慢漂移。
    static const QColor kSeries[] = {
        QColor(0x21, 0x96, 0xF3),  // CH1 蓝
        QColor(0x4C, 0xAF, 0x50),  // CH2 绿
        QColor(0xFF, 0x98, 0x00),  // CH3 橙
        QColor(0x9C, 0x27, 0xB0),  // CH4 紫
        QColor(0x00, 0xBC, 0xD4),  // CH5 青（多于四路时）
        QColor(0xFF, 0x57, 0x20),  // CH6 深橙
    };
    const int n = static_cast<int>(sizeof(kSeries) / sizeof(kSeries[0]));
    return kSeries[((index % n) + n) % n];
}

}  // namespace color

QString styleSheet() {
    // 颜色先用局部变量固定住，再逐段拼进 QSS——比 .arg() 链安全得多：
    // 样式表里出现 % 字符（转义、百分比宽度）时 .arg() 的位置参数极易错位。
    const QString base      = QStringLiteral("#0E1319");
    const QString surface   = QStringLiteral("#161C23");
    const QString alt       = QStringLiteral("#1C242D");
    const QString border    = QStringLiteral("#26303A");
    const QString strong    = QStringLiteral("#354250");
    const QString text      = QStringLiteral("#E6EDF3");
    const QString dim       = QStringLiteral("#93A3B2");
    const QString faint     = QStringLiteral("#647280");
    const QString accent    = QStringLiteral("#2F81F7");
    const QString ok        = QStringLiteral("#3FB950");
    const QString warn      = QStringLiteral("#D29922");
    const QString danger    = QStringLiteral("#F85149");
    const QString idle      = QStringLiteral("#6E7B8A");

    QString qss;
    qss += QStringLiteral("QWidget{background-color:%1;color:%7;font-size:13px;}\n").arg(base, text);
    qss += QStringLiteral(
        "QToolTip{background-color:%1;color:%2;border:1px solid %3;padding:6px 8px;border-radius:4px;}\n")
        .arg(alt, text, strong);

    // ── 卡片：所有分区共用，标题在上、内容在下 ─────────────────────────
    qss += QStringLiteral("QFrame#card{background-color:%1;border:1px solid %2;border-radius:10px;}\n")
               .arg(surface, border);
    qss += QStringLiteral(
        "QLabel#cardTitle{color:%1;font-size:12px;font-weight:600;background:transparent;}\n").arg(text);
    qss += QStringLiteral("QLabel#cardHint{color:%1;font-size:11px;background:transparent;}\n").arg(faint);
    qss += QStringLiteral("QLabel#sectionDivider{background-color:%1;max-height:1px;min-height:1px;}\n")
               .arg(border);

    // ── 文本角色 ────────────────────────────────────────────────────────
    qss += QStringLiteral(
        "QLabel#valueStrong{color:%1;font-size:20px;font-weight:600;background:transparent;}\n").arg(text);
    qss += QStringLiteral("QLabel#valueUnit{color:%1;font-size:12px;background:transparent;}\n").arg(dim);
    qss += QStringLiteral("QLabel#caption{color:%1;font-size:11px;background:transparent;}\n").arg(dim);
    qss += QStringLiteral("QLabel#dim{color:%1;background:transparent;}\n").arg(dim);

    // ── 按钮 ────────────────────────────────────────────────────────────
    qss += QStringLiteral(
        "QPushButton{background-color:%1;color:%2;border:1px solid %3;border-radius:6px;"
        "padding:7px 12px;min-height:18px;}\n").arg(alt, text, strong);
    qss += QStringLiteral("QPushButton:hover{background-color:#232C36;}\n");
    qss += QStringLiteral("QPushButton:pressed{background-color:#131920;}\n");
    qss += QStringLiteral(
        "QPushButton:disabled{background-color:#14191F;color:#4C5966;border-color:#222A33;}\n");
    qss += QStringLiteral(
        "QPushButton#primary{background-color:%1;border-color:%1;color:#FFFFFF;font-weight:600;}\n")
        .arg(accent);
    qss += QStringLiteral("QPushButton#primary:hover{background-color:#4A94FF;border-color:#4A94FF;}\n");
    qss += QStringLiteral("QPushButton#primary:pressed{background-color:#2569C9;}\n");
    qss += QStringLiteral(
        "QPushButton#primary:disabled{background-color:#1E2A38;border-color:#24384F;color:#4C5966;}\n");
    qss += QStringLiteral(
        "QPushButton#danger{background-color:#3A1D1D;border-color:#6E2B2B;color:%1;font-weight:600;}\n")
        .arg(danger);
    qss += QStringLiteral("QPushButton#danger:hover{background-color:#4A2424;border-color:%1;}\n").arg(danger);
    qss += QStringLiteral("QPushButton#danger:disabled{background-color:#241617;border-color:#3A2222;color:#5E4344;}\n");
    qss += QStringLiteral(
        "QPushButton#ghost{background-color:transparent;border-color:%1;color:%2;}\n").arg(strong, dim);
    qss += QStringLiteral("QPushButton#ghost:hover{background-color:%1;color:%2;}\n").arg(alt, text);
    qss += QStringLiteral(
        "QPushButton#ghost:checked{background-color:#16324F;border-color:%1;color:#9CCBFF;}\n").arg(accent);

    // ── 输入控件 ────────────────────────────────────────────────────────
    const QString input = QStringLiteral(
        "QLineEdit,QDoubleSpinBox,QSpinBox,QComboBox{background-color:%1;border:1px solid %2;"
        "border-radius:5px;padding:5px 7px;color:%3;min-height:18px;"
        "selection-background-color:%4;selection-color:#FFFFFF;}\n")
        .arg(alt, strong, text, accent);
    qss += input;
    qss += QStringLiteral(
        "QLineEdit:focus,QDoubleSpinBox:focus,QSpinBox:focus,QComboBox:focus{border-color:%1;}\n")
        .arg(accent);
    qss += QStringLiteral(
        "QLineEdit:disabled,QDoubleSpinBox:disabled,QSpinBox:disabled,QComboBox:disabled"
        "{background-color:#14191F;color:#4C5966;}\n");
    // 上下箭头用 CSS 边框画，不依赖 Qt 自带图标资源（部署时容易漏 qsvg）。
    qss += QStringLiteral(
        "QDoubleSpinBox::up-button,QSpinBox::up-button,"
        "QDoubleSpinBox::down-button,QSpinBox::down-button"
        "{background-color:#232C36;border:none;width:15px;}\n");
    qss += QStringLiteral(
        "QDoubleSpinBox::up-button:hover,QSpinBox::up-button:hover,"
        "QDoubleSpinBox::down-button:hover,QSpinBox::down-button:hover{background-color:#2C3742;}\n");
    qss += QStringLiteral(
        "QDoubleSpinBox::up-arrow,QSpinBox::up-arrow{image:none;width:0;height:0;"
        "border-left:4px solid transparent;border-right:4px solid transparent;"
        "border-bottom:4px solid %1;}\n").arg(dim);
    qss += QStringLiteral(
        "QDoubleSpinBox::down-arrow,QSpinBox::down-arrow{image:none;width:0;height:0;"
        "border-left:4px solid transparent;border-right:4px solid transparent;"
        "border-top:4px solid %1;}\n").arg(dim);
    qss += QStringLiteral("QComboBox::drop-down{border:none;width:18px;}\n");
    qss += QStringLiteral(
        "QComboBox QAbstractItemView{background-color:%1;border:1px solid %2;"
        "selection-background-color:#16324F;selection-color:%3;outline:none;}\n")
        .arg(alt, strong, text);

    qss += QStringLiteral("QCheckBox{color:%1;spacing:7px;background:transparent;}\n").arg(dim);
    qss += QStringLiteral(
        "QCheckBox::indicator{width:14px;height:14px;border:1px solid %1;border-radius:3px;"
        "background-color:%2;}\n").arg(strong, alt);
    qss += QStringLiteral("QCheckBox::indicator:checked{background-color:%1;border-color:%1;}\n")
               .arg(accent);

    // ── 滚动区 ──────────────────────────────────────────────────────────
    qss += QStringLiteral("QScrollArea{border:none;background:transparent;}\n");
    qss += QStringLiteral("QScrollArea>QWidget>QWidget{background:transparent;}\n");
    qss += QStringLiteral("QScrollBar:vertical{background:transparent;width:9px;margin:0;}\n");
    qss += QStringLiteral(
        "QScrollBar::handle:vertical{background:#2E3945;border-radius:4px;min-height:28px;}\n");
    qss += QStringLiteral("QScrollBar::handle:vertical:hover{background:#3C4A58;}\n");
    qss += QStringLiteral("QScrollBar:horizontal{background:transparent;height:9px;}\n");
    qss += QStringLiteral(
        "QScrollBar::handle:horizontal{background:#2E3945;border-radius:4px;min-width:28px;}\n");
    qss += QStringLiteral(
        "QScrollBar::add-line,QScrollBar::sub-line{height:0;width:0;}\n");
    qss += QStringLiteral("QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}\n");

    // ── 顶栏 / 底栏 / 门禁药丸 ────────────────────────────────────────
    qss += QStringLiteral(
        "QWidget#headerBar{background-color:%1;border-bottom:1px solid %2;}\n").arg(surface, border);
    qss += QStringLiteral(
        "QWidget#bottomBar{background-color:%1;border-top:1px solid %2;}\n").arg(surface, border);
    qss += QStringLiteral("QLabel#appTitle{color:%1;font-size:18px;font-weight:700;background:transparent;}\n").arg(text);
    qss += QStringLiteral("QLabel#appSubtitle{color:%1;font-size:12px;background:transparent;}\n").arg(faint);
    qss += QStringLiteral("QLabel#appClock{color:%1;font-size:14px;background:transparent;}\n").arg(dim);
    qss += QStringLiteral("QLabel#gatePill{background-color:%1;}\n").arg(idle);
    qss += QStringLiteral(
        "QLabel#deviceStrip{background-color:%1;border:1px solid %2;border-radius:6px;"
        "padding:6px 10px;font-size:12px;}\n").arg(alt, border);
    qss += QStringLiteral(
        "QLabel#placeholderText{color:%1;font-size:14px;background-color:%2;"
        "border:1px dashed %3;border-radius:6px;}\n").arg(faint, QStringLiteral("#0F151B"), strong);

    // ── 离线遮罩页 ────────────────────────────────────────────────────
    // 做成整页而不是半透明盖在按钮上：半透明控件收不到鼠标事件，
    // 设备离线时按钮照样点得动——那不叫保护，那叫摆设。
    qss += QStringLiteral("QWidget#offlineOverlay{background-color:#0A0E13;}\n");
    qss += QStringLiteral(
        "QLabel#offlineOverlayText{background:transparent;font-size:14px;}\n");

    // ── 大按钮（启停）────────────────────────────────────────────────
    // 设计方案 §3.1 要求 ≥120x60、圆角 8px、"物理按压"感。这里用
    // primary/danger 两个 objectName 表达，不逐个 setStyleSheet。
    qss += QStringLiteral(
        "QPushButton#startBig,QPushButton#stopBig{font-size:17px;font-weight:700;"
        "border-radius:8px;padding:10px 14px;}\n");
    qss += QStringLiteral(
        "QPushButton#startBig{background-color:#1F6F2E;border-color:#2E8B3E;color:#EAFBEE;}\n");
    qss += QStringLiteral("QPushButton#startBig:hover{background-color:#268138;border-color:#35A349;}\n");
    qss += QStringLiteral("QPushButton#startBig:pressed{background-color:#185A25;border-color:#2E8B3E;}\n");
    qss += QStringLiteral(
        "QPushButton#startBig:disabled{background-color:#16201A;border-color:#243029;color:#48554D;}\n");
    qss += QStringLiteral(
        "QPushButton#stopBig{background-color:#2A1416;border-color:#5E2B2B;color:#FFD7D4;}\n");
    qss += QStringLiteral("QPushButton#stopBig:hover{background-color:#3B1C1F;border-color:#7A3A3A;}\n");
    qss += QStringLiteral("QPushButton#stopBig:pressed{background-color:#221012;border-color:#5E2B2B;}\n");
    qss += QStringLiteral(
        "QPushButton#stopBig:disabled{background-color:#1A1416;border-color:#2A1E1F;color:#544748;}\n");
    // ── 日志列表 ────────────────────────────────────────────────────────
    qss += QStringLiteral(
        "QListWidget#logView{background-color:#10161C;border:1px solid %1;border-radius:6px;"
        "outline:none;font-family:'Cascadia Mono','Consolas',monospace;font-size:12px;}\n").arg(border);
    qss += QStringLiteral("QListWidget#logView::item{padding:2px 6px;border:none;}\n");
    qss += QStringLiteral("QListWidget#logView::item:selected{background-color:#1B2733;}\n");

    // ── 工具条 / 菜单 / 分隔条 ──────────────────────────────────────────
    qss += QStringLiteral("QToolBar{background:transparent;border:none;spacing:4px;padding:0;}\n");
    qss += QStringLiteral("QToolBar::separator{background-color:%1;width:1px;margin:4px 6px;}\n").arg(border);
    qss += QStringLiteral(
        "QToolButton{background-color:%1;border:1px solid %2;border-radius:5px;padding:4px 9px;color:%3;}\n")
        .arg(alt, strong, dim);
    qss += QStringLiteral("QToolButton:hover{background-color:#232C36;color:%1;}\n").arg(text);
    qss += QStringLiteral("QToolButton:checked{background-color:#16324F;border-color:%1;color:#9CCBFF;}\n")
               .arg(accent);
    qss += QStringLiteral("QMenuBar{background-color:%1;border-bottom:1px solid %2;}\n").arg(surface, border);
    qss += QStringLiteral("QMenuBar::item{padding:5px 10px;background:transparent;}\n");
    qss += QStringLiteral("QMenuBar::item:selected{background-color:%1;}\n").arg(alt);
    qss += QStringLiteral("QMenu{background-color:%1;border:1px solid %2;padding:4px;}\n").arg(alt, strong);
    qss += QStringLiteral("QMenu::item{padding:5px 22px 5px 10px;border-radius:4px;}\n");
    qss += QStringLiteral("QMenu::item:selected{background-color:#16324F;}\n");
    qss += QStringLiteral("QMenu::separator{height:1px;background:%1;margin:4px 6px;}\n").arg(border);
    qss += QStringLiteral(
        "QSplitter::handle{background-color:transparent;}\n"
        "QSplitter::handle:horizontal{width:8px;}\n"
        "QSplitter::handle:vertical{height:8px;}\n");
    // ok/warn 目前由各控件自行取色，这里保留一个类名出口，
    // 便于以后加"整行按状态染色"的日志条目。
    qss += QStringLiteral("QWidget[statusLevel='%1']{}\n").arg(ok);
    return qss;
}

void apply(QApplication& app) {
    // 暗色调色板要先设好：QChart 等自绘控件会读 QPalette，
    // 只给 QSS 的话它们内部仍按系统浅色调画。
    QPalette p;
    p.setColor(QPalette::Window,          color::base());
    p.setColor(QPalette::WindowText,      color::text());
    p.setColor(QPalette::Base,            color::surfaceAlt());
    p.setColor(QPalette::AlternateBase,   color::surface());
    p.setColor(QPalette::Text,            color::text());
    p.setColor(QPalette::Button,          color::surfaceAlt());
    p.setColor(QPalette::ButtonText,      color::text());
    p.setColor(QPalette::Highlight,       color::accent());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase,     color::surfaceAlt());
    p.setColor(QPalette::ToolTipText,     color::text());
    p.setColor(QPalette::Link,            color::accent());
    p.setColor(QPalette::Disabled,        QPalette::Text,       color::idle());
    p.setColor(QPalette::Disabled,        QPalette::ButtonText, color::idle());
    p.setColor(QPalette::Disabled,        QPalette::WindowText, color::idle());
    app.setPalette(p);

    QFont f(QStringLiteral("Microsoft YaHei UI"));
    f.setPointSize(10);
    f.setHintingPreference(QFont::PreferFullHinting);
    app.setFont(f);

    app.setStyleSheet(styleSheet());
}

}  // namespace libs::ui::theme
