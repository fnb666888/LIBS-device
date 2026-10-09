#include "prediction_poller.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <mutex>
#include <vector>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QElapsedTimer clock;
    clock.start();
    std::mutex mutex;
    std::vector<qint64> starts;
    int fetches = 0, deliveries = 0, errors = 0, ticks = 0;
    bool fetch_on_ui = false, delivery_off_ui = false;
    auto* ui_thread = QThread::currentThread();
    libs::ui::PredictionPoller poller(libs::ui::PipeClient(), nullptr, [&] {
        int index;
        {
            std::lock_guard<std::mutex> lock(mutex);
            index = ++fetches;
            starts.push_back(clock.elapsed());
            fetch_on_ui = fetch_on_ui || QThread::currentThread() == ui_thread;
        }
        QThread::msleep(80); // Simulate IPC work; the GUI must keep processing events.
        if (index == 5) throw QStringLiteral("temporary pipe failure");
        return QJsonObject{{QStringLiteral("state"), QStringLiteral("completed")},
                           {QStringLiteral("completed"), 1}};
    });
    QObject::connect(&poller, &libs::ui::PredictionPoller::status_ready, &app, [&](const QJsonObject&) {
        ++deliveries;
        delivery_off_ui = delivery_off_ui || QThread::currentThread() != ui_thread;
    });
    QObject::connect(&poller, &libs::ui::PredictionPoller::polling_error, &app, [&](const QString&) { ++errors; });
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++ticks; });
    heartbeat.start(20);
    poller.start();
    QTimer::singleShot(2200, &app, &QCoreApplication::quit);
    app.exec();
    poller.stop();
    // One initial delivery, one after recovery; identical good polls are coalesced.
    bool cadence = starts.size() >= 6;
    for (int i = 1; i < 5 && cadence; ++i)
        cadence = starts[i] - starts[i - 1] >= 120 && starts[i] - starts[i - 1] <= 210;
    const bool passed = cadence && deliveries == 2 && errors == 1 && ticks >= 70 &&
                        !fetch_on_ui && !delivery_off_ui;
    std::printf("fetches=%d deliveries=%d errors=%d gui_ticks=%d cadence=%s\n",
                fetches, deliveries, errors, ticks, cadence ? "passed" : "failed");
    return passed ? 0 : 1;
}
