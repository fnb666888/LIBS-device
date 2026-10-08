// 无头验证：用 UI **自己的** StreamPoller 走一遍"订阅 → 取帧 → 退订"。
//
// 为什么需要它：前端曾经订阅成功却从不取帧（subscriber 从响应顶层取，
// 拿成 -1，定时器根本没启动）。那纯粹是 UI 侧的问题 —— 后端的
// spectrum_stream status 看起来完全正常（subscribers=1、frames 在涨），
// 只有把 UI 的取帧循环单独跑一遍才能证明它真的通了。
//
// 注意（重写后）：spectrum_stream 现在**只有订阅/退订/latest/status**，
// 没有 start/stop —— 出不出光由 measurement_start / laser_fire 决定。
// 所以这个探针**自己不会产生任何帧**：它只验证订阅与取帧链路是通的。
// 想看到帧，请在另一个终端跑 measurement_start（见 build_probe_run 提示）。
#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

#include "frame_model.hpp"
#include "pipe_client.hpp"
#include "stream_poller.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    libs::ui::PipeClient client;
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

    // poller.open() 内部已经做了 subscribe + 订阅号解析，不需要外部再发命令。
    if (!poller.open()) {
        std::printf("poller.open() FAILED — 前端会显示“订阅光谱展示失败”\n");
        return 1;
    }
    std::printf("subscribed; waiting %lld ms for frames...\n",
                static_cast<long long>(argc > 1 ? 12000 : 8000));

    QTimer::singleShot(argc > 1 ? 12000 : 8000, &app, [&]() {
        poller.close();   // 退订
        app.quit();
    });
    app.exec();

    // 退订之后，后端应该不再保留这个 subscriber。
    try {
        const QJsonObject st = client.command(QStringLiteral("spectrum_stream"),
                                               QJsonObject{{QStringLiteral("action"),
                                                            QStringLiteral("status")}});
        std::printf("after close: %s\n", qPrintable(QString::fromUtf8(QJsonDocument(st).toJson(
                                                 QJsonDocument::Compact))));
    } catch (const QString& e) {
        std::printf("status failed: %s\n", qPrintable(e));
    }

    std::printf("== frames=%d  present_channels=%d  absent_channels=%d ==\n",
                frames, present, absent);
    if (frames == 0) {
        std::printf("(frames=0 是预期的：本探针不出光。"
                   "请另开终端跑 measurement_start 再跑本探针。)\n");
    }
    return frames > 0 ? 0 : 2;
}
