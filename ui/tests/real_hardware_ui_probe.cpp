// Explicitly authorized hardware test: use production window/control objects.
// Normal application startup has no auto-start behavior; only this test binary
// clicks measurement controls, and it requires an explicit environment opt-in.
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include "main_window.hpp"
#include "theme.hpp"

int main(int argc, char** argv) {
    if (qEnvironmentVariable("LIBS_UI_HARDWARE_TEST") != QLatin1String("authorized-short-test"))
        return 2;
    const QString output = qEnvironmentVariable("LIBS_UI_HARDWARE_TEST_DIR");
    if (output.isEmpty()) return 2;
    bool duration_ok = false;
    const int requested_duration = qEnvironmentVariable("LIBS_UI_HARDWARE_TEST_MS").toInt(&duration_ok);
    const int duration_ms = duration_ok ? requested_duration : 5000;
    if (duration_ms < 1000 || duration_ms > 120000) return 2;
    const bool preserve = qEnvironmentVariable("LIBS_UI_HARDWARE_PRESERVE") == QLatin1String("1");
    QApplication app(argc, argv);
    libs::ui::theme::apply(app);
    libs::ui::MainWindow window;
    window.show();
    auto* start = window.findChild<QPushButton*>(QStringLiteral("startBig"));
    auto* stop = window.findChild<QPushButton*>(QStringLiteral("stopBig"));
    auto* branch = window.findChild<QComboBox*>(QStringLiteral("predictionBranch"));
    QPushButton* precheck = nullptr;
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("设备预检")) precheck = button;
    if (!start || !stop || !branch || !precheck) return 3;
    QDir().mkpath(output);
    QJsonArray events;
    QElapsedTimer total, emission;
    total.start();
    auto text = [&]() {
        QStringList labels;
        for (auto* label : window.findChildren<QLabel*>()) labels.append(label->text());
        return labels.join('\n');
    };
    auto record = [&](const QString& action) {
        events.append(QJsonObject{{"action", action}, {"elapsed_ms", double(total.elapsed())},
            {"start_enabled", start->isEnabled()}, {"stop_enabled", stop->isEnabled()},
            {"labels", text()}});
        QFile file(output + "/ui-control-events.json");
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(events).toJson());
    };
    bool failed = false;
    int phase = 0;
    QTimer timer;
    timer.setInterval(100);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        const QString labels = text();
        if (phase == 6) return; // Keep the visible UI and SDK session for diagnostics.
        if (total.elapsed() > duration_ms + 100000) {
            record("ui_test_timeout");
            window.grab().save(output + "/ui-timeout.png");
            app.exit(1); return; // Controller/watchdog independently requests stop.
        }
        if (phase == 0 && total.elapsed() > 1000 && precheck->isEnabled() && start->isEnabled()) {
            record("click_precheck"); precheck->click(); phase = 1;
        } else if (phase == 1 && labels.contains(QStringLiteral("设备预检未通过"))) {
            record("precheck_failed"); window.grab().save(output + "/ui-precheck-failed.png");
            app.exit(1);
        } else if (phase == 1 && start->isEnabled() && precheck->isEnabled() && branch->isEnabled() &&
                   (labels.contains(QStringLiteral("设备预检通过")) || QFile::exists(output + "/precheck-passed.flag"))) {
            record("select_oxide_model"); branch->setCurrentIndex(branch->findData("oxide")); phase = 2;
        } else if (phase == 2 && labels.contains(QStringLiteral("模型加载失败"))) {
            record("ui_model_load_failed"); app.exit(1);
        } else if (phase == 2 && labels.contains(QStringLiteral("模型已加载，等待测量光谱。")) && start->isEnabled()) {
            record("ui_ready_to_start");
            QFile ready(output + "/arm-ready.flag");
            if (ready.open(QIODevice::WriteOnly)) ready.write("ready");
            phase = 20;
        } else if (phase == 20 && QFile::exists(output + "/armed.flag") && start->isEnabled()) {
            window.grab().save(output + "/ui-before-start.png");
            record("click_start_detection"); start->click(); phase = 3;
        } else if (phase == 3 && (labels.contains(QStringLiteral("开始检测被业务层拒绝")) ||
                                 QFile::exists(output + "/start-rejected.flag"))) {
            record("ui_start_rejected"); window.grab().save(output + "/ui-start-rejected.png");
            QFile finished(output + "/finished.flag");
            if (finished.open(QIODevice::WriteOnly)) finished.write("start_rejected");
            if (preserve) phase = 6;
            else app.exit(1);
        } else if (phase == 3 && !start->isEnabled() && stop->isEnabled()) {
            emission.start(); record("ui_start_succeeded"); phase = 4;
        } else if (phase == 4) {
            failed = failed || labels.contains(QStringLiteral("采集失败"));
            if ((failed || emission.elapsed() >= duration_ms || QFile::exists(output + "/stop-request.flag")) && stop->isEnabled()) {
                window.grab().save(output + "/ui-during-detection.png");
                record(failed ? "click_stop_after_capture_error" : "click_stop_detection");
                stop->click(); phase = 5;
            }
        } else if (phase == 5 && start->isEnabled() && !stop->isEnabled()) {
            record("ui_stop_completed");
            window.grab().save(output + "/ui-after-stop.png");
            QFile finished(output + "/finished.flag");
            if (finished.open(QIODevice::WriteOnly)) finished.write(failed ? "capture_error" : "stopped");
            if (preserve) phase = 6;
            else app.exit(failed ? 1 : 0);
        }
    });
    timer.start();
    return app.exec();
}
