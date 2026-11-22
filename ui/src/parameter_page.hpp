#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

#include "machine_state.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QScrollArea;

namespace libs::ui {

class Card;
class StatusDot;

class ParameterPage : public QWidget {
    Q_OBJECT
public:
    explicit ParameterPage(QWidget* parent = nullptr);
    void apply_snapshot(const MachineSnapshot& snapshot);
    void apply_readings(const QJsonObject& readings, const QString& error);
    void finish_range_sample(const QJsonObject& reading, const QString& error);
    void set_reading_pending(bool pending);
    void set_measurement_command_pending(bool pending);
    void finish_capture(const QJsonObject& result, bool ok, const QString& error);
    void finish_air_control(const QJsonObject& reading, bool ok, const QString& error);
    void finish_indicator_test(bool ok, const QString& error);
    void finish_motor(const QString& action, const QJsonObject& data, bool ok, const QString& error);
    void show_device(const QString& topic);
    void show_air_result(const QJsonObject& values, bool ok, const QString& message, bool applied);
    void finish_reload(const QString& group, const QJsonObject& groups, bool ok, const QString& message);
    void finish_save(const QString& group, bool ok, const QString& message, const QStringList& failed_fields);

signals:
    void save_requested(const QJsonObject& request);
    void reload_requested(const QString& group);
    void air_read_requested();
    void air_apply_requested(const QJsonObject& values);
    void range_read_requested();
    void hardware_start_requested();
    void hardware_stop_requested();
    void capture_requested();
    void air_control_requested(const QJsonObject& request);
    void indicator_test_requested(const QJsonObject& request);
    void refresh_requested();
    void help_requested(const QString& topic);
    void motor_requested(const QString& action, int duration);
    void motor_frequency_requested(double frequency_hz);

private:
    struct FieldSpec {
        const char* key;
        const char* label;
        int decimals;
        const char* unit;
        const char* tooltip;
        double minimum = 0;
        double maximum = 0;
    };
    struct Field {
        QString key;
        QDoubleSpinBox* box = nullptr;
        QLabel* error = nullptr;
        double original = 0;
        bool known = false;
        bool loaded = false;
        bool edited = false;
    };
    struct Group {
        QString name;
        Card* card = nullptr;
        QLabel* feedback = nullptr;
        QLabel* manual_feedback = nullptr;
        QPushButton* read = nullptr;
        QPushButton* save = nullptr;
        QVector<Field> fields;
        bool pending = false;
        qint64 refresh_after_ms = 0;
    };

    static const FieldSpec* laser_specs(std::size_t& count);
    static const FieldSpec* spectrometer_specs(std::size_t& count);
    static const FieldSpec* air_specs(std::size_t& count);
    static const FieldSpec* range_specs(std::size_t& count);
    struct StatusView {
        StatusDot* badge = nullptr;
        QLabel* readings = nullptr;
    };
    void build_group(Group& group, const QString& title, const FieldSpec* specs, std::size_t count);
    void build_status(Card* card, const QString& key);
    void build_section(Card* card, const QString& title, const QString& topic = {});
    void build_manual_controls();
    void render_status();
    void update_manual_controls();
    QJsonObject draft_values(const Group& group) const;
    void load_air_settings();
    bool save_air_settings(const QJsonObject& values);
    void load_motor_settings();
    void save_motor_settings();
    void request_motor_profile();
    bool persist_motor_duration() const;
    Group* measurement_group(const QString& name);
    void build_lamp_card();
    void save_measurement(Group& group);
    void apply_values(Group& group, const QJsonObject& groups);
    void update_measurement_controls(Group& group);
    void set_air_busy(bool busy);
    void load_range_rules();
    void save_range_rules();
    void render_range();
    QString range_summary(const QJsonObject& rules) const;
    void render_lamp();

    Group laser_;
    Group spectrometer_;
    Group air_;
    Group range_;
    Group motor_;
    QHash<QString, StatusView> status_views_;
    QScrollArea* scroll_ = nullptr;
    QLabel* refreshed_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QJsonObject readings_;
    QString reading_error_;
    QPushButton* range_sample_ = nullptr;
    bool range_test_pending_ = false;
    QPushButton* hardware_start_ = nullptr;
    QPushButton* hardware_stop_ = nullptr;
    bool measurement_command_pending_ = false;
    QPushButton* spectrum_trial_ = nullptr;
    bool spectrum_pending_ = false;
    QVector<QPushButton*> air_trials_;
    QPushButton* motor_forward_ = nullptr;
    QPushButton* motor_reverse_ = nullptr;
    QPushButton* motor_stop_ = nullptr;
    bool motor_pending_ = false;
    bool motor_site_loaded_ = false;
    Card* lamp_card_ = nullptr;
    StatusDot* lamp_dot_ = nullptr;
    QComboBox* lamp_color_ = nullptr;
    QComboBox* lamp_mode_ = nullptr;
    QDoubleSpinBox* lamp_duration_ = nullptr;
    QLabel* lamp_feedback_ = nullptr;
    QPushButton* lamp_test_ = nullptr;
    bool lamp_pending_ = false;
    MachineSnapshot snapshot_;
    bool measuring_ = false;
    QJsonObject range_rules_;
    QJsonObject active_range_rules_;
    QJsonObject range_reading_;
};

}  // namespace libs::ui
