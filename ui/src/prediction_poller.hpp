#pragma once

#include <functional>
#include <QJsonObject>
#include <QObject>
#include "pipe_client.hpp"

namespace libs::ui {

// Poll only the cached prediction status; never issue a device command.
class PredictionPoller : public QObject {
    Q_OBJECT
public:
    using Fetch = std::function<QJsonObject()>;
    explicit PredictionPoller(PipeClient client, QObject* parent = nullptr, Fetch fetch = {});
    ~PredictionPoller() override;
    void start();
    void stop();
    void request_refresh();
signals:
    void status_ready(const QJsonObject& status);
    void polling_error(const QString& message);
private:
    class Worker;
    Worker* worker_ = nullptr;
};

}  // namespace libs::ui
