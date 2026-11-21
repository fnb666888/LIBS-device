#pragma once

#include "machine_state.hpp"

#include <QWidget>
#include <array>

class QLabel;
class QPushButton;

namespace libs::ui {

class StatusDot;
class Card;

class DeviceStatusPage : public QWidget {
public:
    explicit DeviceStatusPage(QWidget* parent = nullptr);
    QPushButton* refresh_button() const noexcept { return refresh_; }
    void apply_status(const MachineSnapshot& snapshot);
    void set_reading_pending(bool pending);
    void apply_readings(const QJsonObject& readings, const QString& error);

private:
    void render();

    struct CardView {
        const char* key;
        StatusDot* dot = nullptr;
        QLabel* detail = nullptr;
        Card* frame = nullptr;
    };
    std::array<CardView, 5> cards_{{{"laser"}, {"spectrometer"}, {"indicator"}, {"rangefinder"}, {"air_conditioner"}}};
    MachineSnapshot snapshot_;
    QJsonObject readings_;
    QString reading_error_;
    QLabel* updated_ = nullptr;
    QPushButton* refresh_ = nullptr;
};

}  // namespace libs::ui
