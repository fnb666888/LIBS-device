#include "theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace libs::ui::theme {
namespace color {
QColor base()         { return QColor("#F4F7FB"); }
QColor surface()      { return QColor("#FFFFFF"); }
QColor surfaceAlt()   { return QColor("#F7F9FC"); }
QColor border()       { return QColor("#E2E8F0"); }
QColor borderStrong() { return QColor("#CAD5E2"); }
QColor text()         { return QColor("#192A42"); }
QColor textDim()      { return QColor("#53657D"); }
QColor textFaint()    { return QColor("#78889D"); }
QColor accent()       { return QColor("#2563EB"); }
QColor ok()           { return QColor("#168563"); }
QColor warn()         { return QColor("#B77915"); }
QColor danger()       { return QColor("#CD4052"); }
QColor idle()         { return QColor("#7C8DA3"); }
QColor series(int index) {
    static const QColor colors[] = {QColor("#2563EB"), QColor("#159778"),
        QColor("#D98A20"), QColor("#8758D4"), QColor("#0891B2"), QColor("#D25B42")};
    const int n = static_cast<int>(sizeof(colors) / sizeof(colors[0]));
    return colors[((index % n) + n) % n];
}
}  // namespace color

QString styleSheet() {
    return QStringLiteral(R"QSS(
QWidget { color:#192A42; font-size:14px; }
QMainWindow, QWidget#parameterPage { background:#F4F7FB; }
QLabel { background:transparent; }
QToolTip { color:#192A42; background:#FFFFFF; border:1px solid #CAD5E2; padding:9px; }
QFrame#card { background:#FFFFFF; border:1px solid #E2E8F0; border-radius:14px; }
QLabel#cardTitle { font-size:16px; font-weight:600; }
QLabel#cardHint, QLabel#caption { color:#78889D; font-size:12px; }
QLabel#dim { color:#53657D; }
QLabel#pageTitle { font-size:25px; font-weight:700; }
QLabel#valueStrong { font-size:23px; font-weight:600; }
QLabel#valueUnit { color:#53657D; font-size:12px; }
QLabel#sectionDivider { background:#E2E8F0; min-height:1px; max-height:1px; }
QWidget#appHeader, QWidget#headerBar { background:#FFFFFF; border-bottom:1px solid #E2E8F0; }
QLabel#brandMark { background:#2563EB; color:#FFFFFF; border-radius:12px; font-size:15px; font-weight:700; }
QLabel#appTitle { font-size:19px; font-weight:700; }
QLabel#appSubtitle, QLabel#appClock { color:#78889D; font-size:12px; }
QPushButton { background:#FFFFFF; color:#40536C; border:1px solid #D7E0EB;
    border-radius:9px; padding:9px 16px; min-height:22px; }
QPushButton:hover { background:#F3F7FF; border-color:#AAC4F8; color:#2563EB; }
QPushButton:pressed { background:#E9F0FF; }
QPushButton:disabled { background:#F1F4F8; color:#9CAABE; border-color:#E3E9F1; }
QPushButton#primary, QPushButton#startBig { background:#2563EB; color:#FFFFFF; border-color:#2563EB; font-weight:600; }
QPushButton#primary:hover, QPushButton#startBig:hover { background:#1D54D2; border-color:#1D54D2; }
QPushButton#primary:pressed, QPushButton#startBig:pressed { background:#1D46AD; }
QPushButton#primary:disabled, QPushButton#startBig:disabled { background:#DCE8FC; border-color:#DCE8FC; color:#8CA8D8; }
QPushButton#stopBig, QPushButton#danger { background:#FFF3F4; color:#C63D50; border-color:#F2CCD2; font-weight:600; }
QPushButton#stopBig:hover, QPushButton#danger:hover { background:#FFE7EB; border-color:#E6A5B0; }
QPushButton#stopBig:disabled, QPushButton#danger:disabled { background:#FAF5F6; color:#C9ABB2; border-color:#EDE2E5; }
QPushButton#startBig, QPushButton#stopBig, QPushButton#actionBtn { font-size:15px; padding:10px 20px; }
QPushButton#ghost { background:transparent; }
QPushButton#navTab { background:transparent; border:none; color:#53657D; font-weight:600; padding:10px 17px; }
QPushButton#navTab:hover { background:#F1F5FC; }
QPushButton#navTab:checked { background:#EAF1FF; color:#2563EB; }
QPushButton#advancedToggle { background:#F7F9FD; color:#667A95; border:none; padding:7px 10px; font-size:12px; }
QPushButton#advancedToggle:checked { background:#EDF3FF; color:#2563EB; }
QPushButton#predictionImport { background:#F7F9FD; color:#2563EB; padding:6px 10px; min-height:16px; font-size:12px; }
QLabel#predictionStatus, QLabel#predictionInfo { color:#53657D; font-size:12px; }
QLineEdit, QDoubleSpinBox, QSpinBox, QComboBox { background:#FFFFFF; color:#192A42;
    border:1px solid #D7E1EF; border-radius:8px; padding:8px 10px; min-height:24px;
    selection-background-color:#2563EB; selection-color:white; }
QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color:#2563EB; background:#FAFCFF; }
QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QComboBox:disabled { background:#F4F7FB; color:#97A6B9; }
QDoubleSpinBox::up-button, QSpinBox::up-button { width:24px; border-left:1px solid #E2E8F0; border-bottom:1px solid #E2E8F0; }
QDoubleSpinBox::down-button, QSpinBox::down-button { width:24px; border-left:1px solid #E2E8F0; }
QDoubleSpinBox::up-arrow, QSpinBox::up-arrow { image:url(:/ui/icons/chevron-up.svg); width:12px; height:12px; }
QDoubleSpinBox::down-arrow, QSpinBox::down-arrow { image:url(:/ui/icons/chevron-down.svg); width:12px; height:12px; }
QComboBox::drop-down { border:none; width:24px; }
QComboBox::down-arrow { image:url(:/ui/icons/chevron-down.svg); width:12px; height:12px; }
QComboBox QAbstractItemView { background:white; border:1px solid #E2E8F0; selection-background-color:#EAF1FF; selection-color:#2563EB; }
QFrame#parameterTile { background:#F8FAFD; border:1px solid #EAF0F7; border-radius:10px; }
QLabel#parameterLabel { color:#53657D; font-size:13px; font-weight:600; }
QFrame#parameterTile QDoubleSpinBox { font-size:19px; font-weight:600; }
QLabel#parameterError { color:#CD4052; font-size:12px; }
QLabel#bannerIdle, QLabel#bannerOk, QLabel#bannerWarn, QLabel#bannerDanger {
    padding:10px 14px; border-radius:9px; font-size:12px; }
QLabel#bannerIdle { background:#EAF0F8; color:#60728C; }
QLabel#bannerOk { background:#E9F6F0; color:#168563; }
QLabel#bannerWarn { background:#FFF6E6; color:#A36C16; }
QLabel#bannerDanger { background:#FFF0F2; color:#BD3E50; }
QCheckBox { color:#53657D; background:transparent; spacing:8px; font-size:13px; }
QCheckBox::indicator { width:16px; height:16px; border:1px solid #CAD5E2; border-radius:4px; background:white; }
QCheckBox::indicator:hover { border-color:#2563EB; }
QCheckBox::indicator:checked { background:#2563EB; border-color:#2563EB; image:url(:/ui/icons/check.svg); }
QScrollArea { border:none; background:transparent; }
QScrollArea>QWidget>QWidget { background:transparent; }
QScrollBar:vertical { width:8px; background:transparent; margin:2px; }
QScrollBar::handle:vertical { background:#CCD6E4; border-radius:3px; min-height:32px; }
QScrollBar::handle:vertical:hover { background:#A6B7CE; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
QMenu, QMenuBar { background:white; border:1px solid #E2E8F0; }
QMenu::item { padding:8px 24px; }
QMenu::item:selected { background:#EAF1FF; }
QSplitter::handle { background:#E2E8F0; }
QListWidget#logView { background:white; border:1px solid #E2E8F0; border-radius:8px; }
)QSS");
}

void apply(QApplication& app) {
    // Fusion 控件绘制 + 固定浅色调色板，独立于 Windows 的系统深色偏好。
    app.setStyle(QStringLiteral("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, color::base());
    p.setColor(QPalette::WindowText, color::text());
    p.setColor(QPalette::Base, color::surface());
    p.setColor(QPalette::AlternateBase, color::surfaceAlt());
    p.setColor(QPalette::Text, color::text());
    p.setColor(QPalette::Button, color::surface());
    p.setColor(QPalette::ButtonText, color::text());
    p.setColor(QPalette::Highlight, color::accent());
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, color::surface());
    p.setColor(QPalette::ToolTipText, color::text());
    p.setColor(QPalette::Link, color::accent());
    p.setColor(QPalette::Disabled, QPalette::Text, color::idle());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, color::idle());
    p.setColor(QPalette::Disabled, QPalette::WindowText, color::idle());
    app.setPalette(p);
    QFont font(QStringLiteral("Microsoft YaHei UI"), 10);
    font.setHintingPreference(QFont::PreferFullHinting);
    app.setFont(font);
    app.setStyleSheet(styleSheet());
}
}  // namespace libs::ui::theme
