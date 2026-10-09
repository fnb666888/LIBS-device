#pragma once

// 浅色主题：统一调色板、字体与控件样式；状态颜色仍表达后端事实。

#include <QColor>
#include <QString>

class QApplication;

namespace libs::ui::theme {

namespace color {

QColor base();         // 窗口底色
QColor surface();      // 卡片
QColor surfaceAlt();   // 卡片内的次级分区（输入框、表头）
QColor border();
QColor borderStrong();
QColor text();
QColor textDim();
QColor textFaint();
QColor accent();
QColor ok();
QColor warn();
QColor danger();
QColor idle();

// 光谱通道配色。索引 0..3 对应 CH1..CH4（与设计方案 §4.1 一致），
// 超出四路时按顺序循环取用，绝不"随便给个灰"。
QColor series(int index);

}  // namespace color

// 全局 QSS。控件一律靠 objectName / 类名选择器，不靠逐个 setStyleSheet，
// 这样换主题只改这一个文件。
QString styleSheet();

// 统一入口：设置浅色调色板 + 字体 + QSS。
void apply(QApplication& app);

}  // namespace libs::ui::theme
