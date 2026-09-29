// 入口（`ui/src/main.cpp`）。
//
// 极薄：装主题、建窗口、跑事件循环。业务判定一条都没有——
// 这个进程是**纯管道客户端**，不链接 libs_devices，也不链接 libs_business。

#include <QApplication>

#include "main_window.hpp"
#include "theme.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("libs-spectrum-ui"));
    QApplication::setOrganizationName(QStringLiteral("LIBS"));
    // 主题在窗口之前装好：控件一创建就带着暗色调色板和 QSS，
    // 免得出现"先亮后暗"的闪一下——现场触摸屏上那一下很扎眼。
    libs::ui::theme::apply(app);
    libs::ui::MainWindow window;
    window.show();
    return app.exec();
}
