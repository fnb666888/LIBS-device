// 入口（`ui/src/main.cpp`）。
//
// 极薄：装主题、建窗口、跑事件循环。业务判定一条都没有——
// 这个进程是**纯管道客户端**，不链接 libs_devices，也不链接 libs_business。

#include <QApplication>
#include <QDateTime>

#include "main_window.hpp"
#include "pipe_client.hpp"
#include "theme.hpp"
#include "ui_log.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("libs-spectrum-ui"));
    QApplication::setOrganizationName(QStringLiteral("LIBS"));
    // 主题在窗口之前装好：控件一创建就带着暗色调色板和 QSS，
    // 免得出现"先亮后暗"的闪一下——现场触摸屏上那一下很扎眼。
    libs::ui::theme::apply(app);
    // 起手就落一笔：**一个会自己消失的进程必须留下证据**。
    // 以前这个进程完全没有日志，界面消失后无从查起。
    libs::ui::ui_log("START", QStringLiteral("界面启动 pid=%1 日志=%2 管道=%3")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(libs::ui::ui_log_path())
                                   .arg(libs::ui::PipeClient::default_pipe_name()));

    libs::ui::MainWindow window;
    window.show();

    // exec() 正常返回 = 走的是正常退出路径。日志里**没有**这一行，
    // 就说明进程是被杀 / 崩掉的（而不是"窗口被关掉"）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [] {
        libs::ui::ui_log("APP_EXIT",
                         QStringLiteral("事件循环正常结束（app.exec() 返回），进程即将退出"));
    });

    return app.exec();
}
