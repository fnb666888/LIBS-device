#pragma once

// 工业暗色主题（`ui/src/theme.hpp`）。
//
// 为什么是暗色：现场工控机多在暗光/半暗料流车间，亮底界面在强光下更刺眼、
// 长时间盯着数据区也更累；暗底 + 高饱和状态色让"绿=正常 / 黄=待机 /
// 红=故障"这类**语义色**成为画面上最显眼的东西。
//
// 语义色的用法是纪律，不是审美：
//   ok(绿)    = 设备在线、参数已生效、门禁允许
//   warn(黄)  = 数据未知/未标定、暂存未生效、降级可用
//   danger(红)= 采集失败、设备离线、门禁拒绝
//   idle(灰)  = 未配置 / 未启用 / 没有数据
// 任何"不知道"都必须落到 warn 或 idle，**绝不能落到 ok**——把未知画成
// 正常是现场排障时最贵的一种错误。

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

// 统一入口：设置暗色调色板 + 字体 + QSS。
void apply(QApplication& app);

}  // namespace libs::ui::theme
