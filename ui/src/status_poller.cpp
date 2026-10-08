#include "status_poller.hpp"

#include <exception>

#include <QMutexLocker>
#include <QWaitCondition>

#include "ui_log.hpp"

namespace libs::ui {

namespace {

// status 失败后用来分辨"忙"与"断"的 ping 探测超时。
// ping 不碰设备、不取设备锁，正常几毫秒就回；给 2 秒是为了在守护进程
// 真的不在时也不至于把这个轮询周期拖得太长。
constexpr int kPingProbeTimeoutMs = 2000;

}  // namespace

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

    // QThread 可以在一次 run() 完成后再次 start()，但本对象的停止标志
    // 必须显式清零。出光测量期间 StatusPoller 会 stop()，测量结束再
    // start()；不重置的话新线程第一轮采样后就直接退出，界面状态永远停在
    // 测量前的快照。
    void prepare_start(int interval_ms) {
        stopping_.storeRelease(0);
        if (interval_ms > 0) interval_ms_ = interval_ms;
        QMutexLocker lock(&wake_mutex_);
        poked_ = false;
    }

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
    // 一次 status 往返失败后的统一处理：保留上一帧、标 valid=false，
    // 再用 ping 分辨"忙"还是"断"。三种 catch（QString / std::exception /
    // 其它）都走这里，避免同一段逻辑抄三遍。
    void handle_failure(MachineSnapshot& next, const QString& reason) {
        MachineSnapshot previous;
        {
            QMutexLocker lock(&owner_->mutex_);
            previous = owner_->snapshot_;
        }
        next = previous;
        next.valid = false;
        next.error = reason;
        // "忙"还是"断"？status 要先取设备锁，长设备操作期间它只能排队等，
        // 排到超时就失败 —— 那不是掉线。ping 不碰设备、不取设备锁，
        // 正常只要几毫秒，用它把两种情形分开，别让"忙"冒充"断"。
        next.daemon_busy = false;
        try {
            client_.command(QStringLiteral("ping"), QJsonObject(), kPingProbeTimeoutMs);
            next.daemon_busy = true;
        } catch (...) {
            // ping 也不通：守护进程确实不在服务。这里刻意 catch(...) ——
            // 探测失败的原因是什么都不重要，重要的是别再抛出去。
        }
    }

    void fetch_and_publish() {
        MachineSnapshot next;
        try {
            // status：整机快照（health / gate / 空调 / 运动 / 有矿 / 阶段）
            const QJsonObject status = client_.command(QStringLiteral("status"));
            const QJsonObject payload = status.value(QStringLiteral("payload")).toObject();
            if (payload.value(QStringLiteral("busy")).toBool(false)) {
                // 后端有界等待设备锁，拿不到就明确回答"我忙，这轮没读状态"。
                // 这不是失败、更不是掉线：保留上一帧的数据，只把"忙"标出来。
                // 好处是界面 250ms 就变黄，而不是等 5 秒超时之后再报红色"未连接"。
                MachineSnapshot previous;
                {
                    QMutexLocker lock(&owner_->mutex_);
                    previous = owner_->snapshot_;
                }
                next = previous;
                next.valid = false;
                next.daemon_busy = true;
                next.error = payload.value(QStringLiteral("busy_note")).toString();
            } else {
                next = parse_status(payload);
                next.error.clear();
                next.daemon_busy = false;

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
            }
        } catch (const QString& e) {
            // 守护进程没起 / 管道断开：valid=false + error，
            // 同时**保留上一帧的好数据**。这样 UI 能显示"数据可能已过期"，
            // 而不是把整屏清空让人以为设备全没了。
            handle_failure(next, e);
        } catch (const std::exception& e) {
            // 非 QString 的异常以前会**直接穿出 QThread::run()**，
            // 那是 std::terminate —— 进程当场消失、连一行日志都不会留。
            // 轮询线程里的任何异常都不该有资格结束整个界面。
            handle_failure(next, QStringLiteral("轮询线程异常：%1")
                                      .arg(QString::fromUtf8(e.what())));
        } catch (...) {
            handle_failure(next, QStringLiteral("轮询线程未知异常（非 QString / 非 std::exception）"));
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
    if (worker_ && !worker_->isRunning()) {
        worker_->prepare_start(interval_ms);
        worker_->start();
    }
}

void StatusPoller::stop() {
    if (!running_) return;
    running_ = false;
    if (!worker_) return;

    worker_->request_stop();

    // **绝不 terminate()。**
    //
    // 这里原来是 `if (!worker_->wait(2000)) { worker_->terminate(); }`，
    // 注释还写着"一次 status 往返最坏 650ms"——那个假设是错的：
    // status 要先取 device_mutex，而出光 / 采集 / 运动全程握着它，
    // 所以客户端侧的真实上限是 5000ms。出光前那次 stop() 因此经常超时，
    // 走 TerminateThread 把一个正卡在 Win32 命名管道调用里的线程当场杀掉。
    // 后果是堆损坏、句柄泄漏、wake_mutex_ 可能被永久锁死 —— 进程随后死掉。
    // 现场表现完全吻合："按了出光测量之后界面完全消失，过一会儿又被计划任务
    // 拉回来"（退出码非 0 才会触发 RestartOnFailure）。
    //
    // 现在只等、不杀：stopping_ 已置位，线程手里那次调用一返回就会自己退出
    // （那次调用本身有超时上限，不会无限挂着）。等不到就留着它 ——
    // 一个还没退出的线程是可以容忍的，一个被强杀的线程不是。
    // start() 有 !isRunning() 守卫，不会在它还活着时重复启动。
    constexpr int kJoinWaitMs = 12000;   // 大于任何单次 status 往返的上限
    if (!worker_->wait(kJoinWaitMs)) {
        ui_log("ERROR", QStringLiteral(
            "状态轮询线程 %1 ms 内没有退出，**没有强杀**（强杀会破坏堆并让进程消失）。"
            "出光/采集结束后它会自行退出；这行日志用于下一次定性。").arg(kJoinWaitMs));
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



