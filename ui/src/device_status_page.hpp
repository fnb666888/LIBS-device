#pragma once

#include "machine_state.hpp"
#include "widgets.hpp"

namespace libs::ui {

QString air_mode_name(int code);

struct DevicePresentation {
    Level level = Level::Idle;
    QString status;
    QStringList readings;
    QString detail;
};

DevicePresentation present_device(const QString& key, const MachineSnapshot& snapshot,
                                  const QJsonObject& readings = {}, const QString& error = {});

}  // namespace libs::ui
