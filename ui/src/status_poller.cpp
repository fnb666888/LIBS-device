#include "status_poller.hpp"

#include "machine_state.hpp"

#include <QMutexLocker>
#include <QWaitCondition>

namespace libs::ui {

// ── 工作线程 ────────────────────────────────────────────────────────────
//
// 继承 QThread 而不是 moveToThread + worker 对象：这里的工作逻辑就是
// "一个循环 + 一次阻塞往返"，QThread::run() 天然就是这个形状，
// 套一层 moveToThread 只会让 QTimer 的线程归属变得容易搞错。
//
// 循环里用**超时唤醒**而不是纯 sleep：既保证大致节奏，又能让
// request_refresh() 在下一次迭代就被响应。QWaitCondition 的 wake()
// 比"睡 800ms 再看标志位"响应快，且不需要额外的原子变量。
class StatusPoller::Worker : public QThread {
public:
    Worker(PipeClient client, StatusPoller* owner, int interval_ms)
        : client_(std::move(client)),
          owner_(owner),
          interval_ms_(interval_ms > 0 ? interval_ms : 800) {}

    // 线程启动后调用（run 里的第一件事）：立刻采一次，别让界面空一秒。
    void poke() {
        QMutexLocker lock(&wake_mutex_);
        poked_ = true;
        wake_.wakeAll();
    }

protected:
    void run() override {
        // 起手先采一次，这样"启动"那一刻屏幕上就有东西。
        fetch_and_publish();

        for (;;) {
            {
                QMutexLocker lock(&wake_mutex_);
                if (!poked_) {
                    // wait 最多 interval_ms；被 poke() 唤醒就立刻下一轮。
                    wake_.wait(&wake_mutex_, interval_ms_);
                }
                poked_ = false;
            }
            if (stopping_.loadAcquire()) return;
            fetch_and_publish();
        }
    }


public:
    void request_stop() { stopping_.storeRelease(1); wake_.wakeAll(); }

private:

private:
    void fetch_and_publish() {
        MachineSnapshot next;
        try {
            // status：整机快照（health / gate / 空调 / 运动 / 有矿 / 阶段）
            const QJsonObject status = client_.command(QStringLiteral("status"));
            next = parse_status(status.value("payload").toObject());
            next.error.clear();

            // param_get：核心参数齐备与否。门禁的 parameters_incomplete
            // 就来自它，所以要一起取——否则 UI 只能说"参数不齐"，
            // 却列不出缺哪几项，那等于把问题推回给用户去猜。
            try {
                const QJsonObject params = client_.command(QStringLiteral("param_get"));
                merge_params(next, params.value("payload").toObject());
            } catch (const QString&) {
                // 参数拿不到 ≠ 整机拿不到。保留 status 已有事实，
                // 只把 readiness 标成"未知"，不让一个子命令拖垮整屏。
                next.readiness_known = false;
            }

            // measurement_status：采集/落盘的计数。拿不到不是故障 —— 它只是补充信息，
            // 不能因为没有这个信息就把整屏标成"数据已过期"。
            try {
                const QJsonObject ms = client_.command(QStringLiteral("measurement_status"));
                merge_measurement(next, ms.value("payload").toObject());
            } catch (const QString&) {
                next.measurement_known = false;
            }
        } catch (const QString& e) {
            // 守护进程没起 / 管道断开：valid=false + error，
            // 同时**保留上一帧的好数据**。这样 UI 能显示"数据可能已过期"，
            // 而不是把整屏清空让人以为设备全没了。
            MachineSnapshot previous;
            {
                QMutexLocker lock(&owner_->mutex_);
                previous = owner_->snapshot_;
            }
            next = previous;
            next.valid = false;
            next.error = e;
        }
        {
            QMutexLocker lock(&owner_->mutex_);
            owner_->snapshot_ = next;
        }
        // 通知 UI 线程。跨线程信号走队列，槽在 UI 线程执行——安全。
        QMetaObject::invokeMethod(owner_, "publish", Qt::QueuedConnection);
    }

    PipeClient client_;
    StatusPoller* owner_;
    int interval_ms_;

    QMutex wake_mutex_;
    QWaitCondition wake_;
    bool poked_ = false;
    QAtomicInt stopping_{0};
};

// ── StatusPoller ────────────────────────────────────────────────────────

StatusPoller::StatusPoller(PipeClient client, QObject* parent)
    : QObject(parent), client_(std::move(client)) {
    worker_ = new Worker(client_, this, 800);
    thread_ = worker_;   // Worker 自己就是 QThread
}

StatusPoller::~StatusPoller() { stop(); }

void StatusPoller::start(int interval_ms) {
    if (running_) return;
    running_ = true;
    // 间隔只能在构造时定（QThread::run 已进入循环），所以这里只处理
    // 第一次启动；后续 start() 是 no-op，由 request_refresh() 承担刷新。
    Q_UNUSED(interval_ms)
    if (worker_ && !worker_->isRunning()) worker_->start();
}

void StatusPoller::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_) {
        worker_->request_stop();
        // 给它 2s：一次 status 往返最坏情况是串口超时（约 650ms），
        // 两轮足够它退出了。超时就强杀，避免程序退出时卡住。
        if (!worker_->wait(2000)) {
            worker_->terminate();
            worker_->wait(200);
        }
    }
}

MachineSnapshot StatusPoller::snapshot() const {
    QMutexLocker lock(&mutex_);
    return snapshot_;
}

void StatusPoller::request_refresh() {
    if (running_ && worker_) worker_->poke();
}

void StatusPoller::publish() { emit snapshot_ready(); }

}  // namespace libs::ui



