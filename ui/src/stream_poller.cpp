#include "stream_poller.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "ui_log.hpp"

#include <QJsonArray>
#include <QMutexLocker>
#include <QJsonObject>
#include <QWaitCondition>

namespace libs::ui {

// 光谱帧可能包含四路、每路数千个点，单次 latest 的 JSON 往返在工控机上
// 可达数百毫秒。它必须和 QWidget 事件循环分开，否则 QTimer 的槽会把整
// 个界面冻住。Worker 只做管道和 JSON 工作，结果通过 queued invoke 回到
// StreamPoller 所在线程。
class StreamPoller::Worker : public QThread {
public:
    Worker(PipeClient client, StreamPoller* owner, qint64 session, qint64 subscriber)
        : client_(std::move(client)), owner_(owner), session_(session), subscriber_(subscriber) {}

    void request_stop() {
        stopping_.storeRelease(1);
        wake_.wakeAll();
    }

protected:
    void run() override {
        while (!stopping_.loadAcquire()) {
            const auto started = std::chrono::steady_clock::now();
            try {
                const QJsonObject response = client_.command(
                    QStringLiteral("spectrum_stream"),
                    QJsonObject{{QStringLiteral("action"), QStringLiteral("latest")},
                                {QStringLiteral("subscriber"), static_cast<double>(subscriber_)}});
                const QJsonObject payload = response.value(QStringLiteral("payload")).toObject();
                const int pending = payload.value(QStringLiteral("pending")).toInt(0);
                const qint64 dropped =
                    static_cast<qint64>(payload.value(QStringLiteral("dropped_frames")).toDouble(0));
                QMetaObject::invokeMethod(
                    owner_, [owner = owner_, session = session_, pending, dropped]() {
                        owner->deliver_stats(session, pending, dropped);
                    }, Qt::QueuedConnection);

                if (payload.value(QStringLiteral("has_frame")).toBool(false)) {
                    const FrameData frame = parse_frame(payload);
                    QMetaObject::invokeMethod(
                        owner_, [owner = owner_, session = session_, frame]() {
                            owner->deliver_frame(session, frame);
                        }, Qt::QueuedConnection);
                }
            } catch (const QString& e) {
                publish_error(e);
            } catch (const std::exception& e) {
                // 与 StatusPoller 同理：非 QString 异常以前会穿出 run()，
                // 那是 std::terminate —— 进程当场消失。
                publish_error(QStringLiteral("取帧线程异常：%1").arg(QString::fromUtf8(e.what())));
            } catch (...) {
                publish_error(QStringLiteral("取帧线程未知异常（非 QString / 非 std::exception）"));
            }

            // UI 请求的 min_period_ms 是 1000；每 250 ms 检查一次足够把
            // 新帧及时取走，同时给 status/param_get 留出命名管道实例。
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
            const int wait_ms = static_cast<int>(std::max<qint64>(50, 250 - elapsed));
            QMutexLocker lock(&wake_mutex_);
            if (!stopping_.loadAcquire()) wake_.wait(&wake_mutex_, wait_ms);
        }
    }

private:
    // 三种 catch 共用的收尾：把错误送回 UI 线程，并降速重试
    // （管道故障时高频重试会把单实例命名管道占满，
    // 进而把状态轮询也误报成 Win32 231）。
    void publish_error(const QString& message) {
        QMetaObject::invokeMethod(
            owner_, [owner = owner_, session = session_, message]() {
                owner->deliver_error(session, message);
            }, Qt::QueuedConnection);
        QMutexLocker lock(&wake_mutex_);
        wake_.wait(&wake_mutex_, 750);
    }

    PipeClient client_;
    StreamPoller* owner_;
    qint64 session_;
    qint64 subscriber_;
    QAtomicInt stopping_{0};
    QMutex wake_mutex_;
    QWaitCondition wake_;
};

StreamPoller::StreamPoller(PipeClient client, QObject* parent)
    : QObject(parent), client_(std::move(client)) {}

StreamPoller::~StreamPoller() { close(); }

bool StreamPoller::open() {
    // **重复 open() 必须先退订上一次。**
    //
    // 界面上「启动检测」与「连续出光」都会调 open()；不退订的话每点一次就多留
    // 一个订阅号 —— 它的队列没人取（客户端只取当前那个 id），于是每帧都被它
    // 丢掉一次。实测泄漏到 6 个订阅、dropped 涨到 1756，看着像"显示跟不上"，
    // 其实是我们自己订了一堆没人读的队列。
    close();
    // 订阅号可能在守护进程重启后从 0 重新分配；不要拿上一轮的帧号
    // 去重当前会话，否则新会话的第一帧可能被错误丢掉。
    last_frame_index_ = -1;
    dropped_ = 0;
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
        last_stream_error_.clear();
        worker_ = new Worker(client_, this, session_, subscriber_);
        worker_->start();
        return true;
    } catch (const QString& e) {
        emit stream_error(e);
        return false;
    }
}

void StreamPoller::close() {
    const qint64 old_subscriber = subscriber_;
    const bool was_open = open_;
    open_ = false;  // 先让已经排队的帧失效，再等 worker 退出
    ++session_;
    subscriber_ = -1;
    if (worker_) {
        worker_->request_stop();
        // **绝不 terminate()**，理由与 StatusPoller::stop() 相同：
        // TerminateThread 会在线程执行 Win32 调用的中途把它杀掉，
        // 堆和互斥量都可能就此损坏。这里的线程只调 spectrum_stream
        // （不取设备锁，通常很快就回），等不到就留着它自己退。
        constexpr int kJoinWaitMs = 12000;
        if (worker_->wait(kJoinWaitMs)) {
            delete worker_;
        } else {
            // **不能 delete 一个还在跑的 QThread**：析构会直接崩。
            // 交给它自己的 finished 信号自删；close() 立刻放手。
            ui_log("ERROR", QStringLiteral(
                "取帧线程 %1 ms 内没有退出，交给 finished 自删（没有强杀）。")
                .arg(kJoinWaitMs));
            QThread* dying = worker_;
            connect(dying, &QThread::finished, dying, &QObject::deleteLater);
        }
        worker_ = nullptr;
    }
    if (was_open && old_subscriber >= 0) {
        try {
            client_.command(QStringLiteral("spectrum_stream"),
                           QJsonObject{{QStringLiteral("action"), QStringLiteral("unsubscribe")},
                                       {QStringLiteral("subscriber"), static_cast<double>(old_subscriber)}});
        } catch (const QString&) {
            // 退订失败无所谓：守护进程重启后订阅号自然作废。
            // 这里**不**把它升级成错误弹窗，那会在正常关机时碍眼。
        }
    }
    last_frame_index_ = -1;
    dropped_ = 0;
    last_stream_error_.clear();
}

void StreamPoller::deliver_frame(qint64 session, const FrameData& frame) {
    if (!open_ || session != session_) return;
    last_stream_error_.clear();
    if (frame.frame_index == last_frame_index_) return;
    last_frame_index_ = frame.frame_index;
    emit frame_ready(frame);
    if (!frame.ok) emit capture_failed(frame.capture_error_code, frame.capture_error_message);
}

void StreamPoller::deliver_error(qint64 session, const QString& message) {
    if (!open_ || session != session_) return;
    if (message == last_stream_error_) return;
    last_stream_error_ = message;
    emit stream_error(message);
}

void StreamPoller::deliver_stats(qint64 session, int pending, qint64 dropped) {
    if (!open_ || session != session_) return;
    dropped_ = dropped;
    emit stats_changed(pending, dropped);
}

}  // namespace libs::ui
