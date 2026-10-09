#include "prediction_poller.hpp"

#include <algorithm>
#include <exception>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

namespace libs::ui {

class PredictionPoller::Worker : public QThread {
public:
    Worker(Fetch fetch, PredictionPoller* owner) : fetch_(std::move(fetch)), owner_(owner) {}
    void poke() {
        QMutexLocker lock(&mutex_);
        poked_ = true;
        wake_.wakeAll();
    }
    void request_stop() {
        QMutexLocker lock(&mutex_);
        stopping_ = true;
        wake_.wakeAll();
    }
protected:
    void run() override {
        QJsonObject previous;
        bool have_previous = false, failed = false;
        for (;;) {
            {
                QMutexLocker lock(&mutex_);
                if (stopping_) return;
                poked_ = false;
            }
            QElapsedTimer elapsed;
            elapsed.start();
            try {
                const auto status = fetch_();
                // A completed result stays visible; unchanged polls need no repaint.
                if (!have_previous || failed || status != previous) {
                    QMetaObject::invokeMethod(owner_, [owner = owner_, status] {
                        emit owner->status_ready(status);
                    }, Qt::QueuedConnection);
                }
                previous = status;
                have_previous = true;
                failed = false;
            } catch (const QString& message) {
                publish_error(message);
                failed = true;
            } catch (const std::exception& error) {
                publish_error(QString::fromUtf8(error.what()));
                failed = true;
            } catch (...) {
                publish_error(QStringLiteral("预测状态读取异常"));
                failed = true;
            }
            QMutexLocker lock(&mutex_);
            if (stopping_) return;
            // Subtract request time, so 150 ms is a cadence, not an extra sleep.
            if (!poked_) wake_.wait(&mutex_, static_cast<unsigned long>(
                std::max<qint64>(25, (failed ? 1000 : 150) - elapsed.elapsed())));
        }
    }
private:
    void publish_error(const QString& message) {
        QMetaObject::invokeMethod(owner_, [owner = owner_, message] {
            emit owner->polling_error(message);
        }, Qt::QueuedConnection);
    }
    Fetch fetch_;
    PredictionPoller* owner_;
    QMutex mutex_;
    QWaitCondition wake_;
    bool stopping_ = false, poked_ = false;
};

PredictionPoller::PredictionPoller(PipeClient client, QObject* parent, Fetch fetch) : QObject(parent) {
    if (!fetch) fetch = [client = std::move(client)]() mutable {
        return client.command_payload(QStringLiteral("prediction"),
            QJsonObject{{QStringLiteral("action"), QStringLiteral("status")}}, 1000);
    };
    worker_ = new Worker(std::move(fetch), this);
}
PredictionPoller::~PredictionPoller() { stop(); delete worker_; }
void PredictionPoller::start() { if (!worker_->isRunning()) worker_->start(); }
void PredictionPoller::stop() { worker_->request_stop(); worker_->wait(); }
void PredictionPoller::request_refresh() { worker_->poke(); }

}  // namespace libs::ui
