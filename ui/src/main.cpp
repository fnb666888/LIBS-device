// 入口（`ui/src/main.cpp`）。
//
// 极薄：装主题、建窗口、跑事件循环。业务判定一条都没有——
// 这个进程是**纯管道客户端**，不链接 libs_devices，也不链接 libs_business。

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QTimer>

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

    // ── 离屏自检入口（仅在设了 LIBS_UI_SHOT 时存在）────────────────────
    //
    // 用途：把当前窗口抓成 PNG 后退出。参数页的"字段缺失时显示什么"
    // 这类问题**只有看图才能确认**，靠读代码会漏掉字号截断、提示换行
    // 之类的排版问题。
    //
    // 为什么用环境变量而不是命令行参数：这个 exe 链接成 GUI 子系统
    // （-mwindows，build.ps1 里写明是为了现场双击不弹黑框），
    // 从 PowerShell 传 argv 很别扭。环境变量在两条路径上都简单。
    //
    // QT_QPA_PLATFORM **必须留在 windows**：offscreen 平台插件下
    // QFontDatabase::families() 返回空，界面上一个中文字形都取不到，
    // 截出来的图全是方块 —— 那是插件的假象，不是真乱码。
    const QString shot = qEnvironmentVariable("LIBS_UI_SHOT");
    if (!shot.isEmpty()) {
        const QString page = qEnvironmentVariable("LIBS_UI_SHOT_PAGE");
        if (page == QLatin1String("parameter")) {
            // show_parameter_page() 是 private slot，走 invokeMethod 而不是
            // 直接调用：加一个 public 的"给测试跳页"入口，等于把页面导航
            // 的可变状态暴露给任何调用方。
            QMetaObject::invokeMethod(&window, "show_parameter_page",
                                      Qt::DirectConnection);
        }
        int delay = qEnvironmentVariableIntValue("LIBS_UI_SHOT_DELAY_MS");
        if (delay <= 0) delay = 2500;   // 没设或设坏了都用 2.5s
        QTimer::singleShot(delay, [&window, &shot] {
            window.grab().save(shot);
            QCoreApplication::exit(0);
        });
    }

    return app.exec();
}
