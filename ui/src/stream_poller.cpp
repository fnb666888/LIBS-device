#include "stream_poller.hpp"

#include <QJsonArray>
#include <QJsonObject>

namespace libs::ui {

StreamPoller::StreamPoller(PipeClient client, QObject* parent)
    : QObject(parent), client_(std::move(client)) {
    // 轮询节奏 100ms：流本身默认 min_period_ms=200（≈5fps），
    // UI 比流快一点，保证队列基本是空的、不人为制造背压丢弃。
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &StreamPoller::poll);
}

bool StreamPoller::open() {
    // **重复 open() 必须先退订上一次。**
    //
    // 界面上「启动检测」与「连续出光」都会调 open()；不退订的话每点一次就多留
    // 一个订阅号 —— 它的队列没人取（客户端只取当前那个 id），于是每帧都被它
    // 丢掉一次。实测泄漏到 6 个订阅、dropped 涨到 1756，看着像"显示跟不上"，
    // 其实是我们自己订了一堆没人读的队列。
    close();
    try {
        // command_payload：订阅号在 payload 里，不是响应顶层。
        // 取错一层的话 subscriber_ 会是 -1、open() 返回 false、定时器不启动 ——
        // 于是"订阅成功了却永远不取帧"，守护进程那边一路丢帧，界面上一条谱都没有。
        const QJsonObject sub = client_.command_payload(
            QStringLiteral("spectrum_stream"),
            QJsonObject{{QStringLiteral("action"), QStringLiteral("subscribe")}});
        subscriber_ = static_cast<qint64>(sub.value("subscriber").toDouble(-1));
        if (subscriber_ < 0) {
            emit stream_error(QStringLiteral("守护进程没有返回订阅号，流无法订阅"));
            return false;
        }
        open_ = true;
        timer_.start();
        poll();  // 立刻取一帧，别让界面空一秒
        return true;
    } catch (const QString& e) {
        emit stream_error(e);
        return false;
    }
}

void StreamPoller::close() {
    timer_.stop();
    if (open_ && subscriber_ >= 0) {
        try {
            client_.command(QStringLiteral("spectrum_stream"),
                           QJsonObject{{QStringLiteral("action"), QStringLiteral("unsubscribe")},
                                       {QStringLiteral("subscriber"), static_cast<double>(subscriber_)}});
        } catch (const QString&) {
            // 退订失败无所谓：守护进程重启后订阅号自然作废。
            // 这里**不**把它升级成错误弹窗，那会在正常关机时碍眼。
        }
    }
    open_ = false;
    subscriber_ = -1;
}

void StreamPoller::poll() {
    if (!open_ || subscriber_ < 0) return;
    try {
        const QJsonObject r = client_.command(
            QStringLiteral("spectrum_stream"),
            QJsonObject{{QStringLiteral("action"), QStringLiteral("latest")},
                        {QStringLiteral("subscriber"), static_cast<double>(subscriber_)}});
        const QJsonObject payload = r.value("payload").toObject();

        // 空队列不是错误：ok=true + has_frame=false。UI 继续等下一拍，
        // 别弹"失败"。这正是 stream 协议把"没数据"和"出错"分开的地方。
        const int pending = payload.value("pending").toInt(0);
        const qint64 dropped = static_cast<qint64>(payload.value("dropped_frames").toDouble(0));
        // dropped 变化本身就是"显示跟不上采集"的信号，UI 要能看见。
        dropped_ = dropped;
        emit stats_changed(pending, dropped);

        if (!payload.value("has_frame").toBool(false)) return;
        const FrameData frame = parse_frame(payload);
        if (frame.frame_index == last_frame_index_) return;  // 去重（理论上不会）
        last_frame_index_ = frame.frame_index;

        emit frame_ready(frame);
        if (!frame.ok) {
            emit capture_failed(frame.capture_error_code, frame.capture_error_message);
        }
    } catch (const QString& e) {
        emit stream_error(e);
    }
}

}  // namespace libs::ui
