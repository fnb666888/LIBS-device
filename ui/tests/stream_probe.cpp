// 无头验证：用 UI **自己的** StreamPoller 走一遍"订阅 → 取帧"。
//
// 为什么需要它：前端曾经订阅成功却从不取帧（subscriber 从响应顶层取，
// 拿成 -1，定时器根本没启动）。那纯粹是 UI 侧的问题 —— 后端的
// spectrum_stream status 看起来完全正常（subscribers=1、frames 在涨），
// 只有把 UI 的取帧循环单独跑一遍才能证明它真的通了。
#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

#include "frame_model.hpp"
#include "pipe_client.hpp"
#include "stream_poller.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // 先把流起起来（界面点按钮时做的事），再订阅。
    libs::ui::PipeClient client;
    try {
        client.command(QStringLiteral("spectrum_stream"),
                       QJsonObject{{QStringLiteral("action"), QStringLiteral("start")},
                                   {QStringLiteral("min_period_ms"), 1000}});
    } catch (const QString& e) {
        std::printf("start failed: %s\n", qPrintable(e));
        return 1;
    }

    libs::ui::StreamPoller poller{libs::ui::PipeClient()};
    int frames = 0;
    int present = 0;
    int absent = 0;

    QObject::connect(&poller, &libs::ui::StreamPoller::frame_ready,
                     [&](const libs::ui::FrameData& f) {
                         ++frames;
                         int p = 0, a = 0;
                         double peak = 0.0;
                         for (const libs::ui::ChannelData& c : f.channels) {
                             if (c.present) {
                                 ++p;
                                 for (double v : c.counts) if (v > peak) peak = v;
                             } else {
                                 ++a;
                             }
                         }
                         present += p;
                         absent += a;
                         std::printf("frame %lld: present=%d absent=%d ok=%d peak=%.1f\n",
                                     static_cast<long long>(f.frame_index), p, a,
                                     f.ok ? 1 : 0, peak);
                     });
    QObject::connect(&poller, &libs::ui::StreamPoller::stream_error,
                     [](const QString& m) { std::printf("stream_error: %s\n", qPrintable(m)); });
    QObject::connect(&poller, &libs::ui::StreamPoller::capture_failed,
                     [](const QString& c, const QString& m) {
                         std::printf("capture_failed: %s / %s\n", qPrintable(c), qPrintable(m));
                     });

    if (!poller.open()) {
        std::printf("poller.open() FAILED — 前端会显示“订阅预览流失败”\n");
        return 1;
    }

    QTimer::singleShot(12000, &app, [&]() {
        poller.close();
        try {
            client.command(QStringLiteral("spectrum_stream"),
                           QJsonObject{{QStringLiteral("action"), QStringLiteral("stop")}});
        } catch (const QString&) {
        }
        app.quit();
    });
    app.exec();

    std::printf("== frames=%d  present_channels=%d  absent_channels=%d ==\n",
                frames, present, absent);
    return (frames > 0 && present > 0) ? 0 : 1;
}
