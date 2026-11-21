#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

#include "machine_state.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTimer;

namespace libs::ui {

class Card;
class StatusDot;

class ParameterPage : public QWidget {
    Q_OBJECT
public:
    explicit ParameterPage(QWidget* parent = nullptr);
    void apply_snapshot(const MachineSnapshot& snapshot);
    void apply_range_reading(const QJsonObject& reading);
    void show_air_result(const QJsonObject& values, bool ok, const QString& message, bool applied);
    void finish_reload(const QString& group, const QJsonObject& groups, bool ok, const QString& message);
    void finish_save(const QString& group, bool ok, const QString& message, const QStringList& failed_fields);

signals:
    void save_requested(const QJsonObject& request);
    void reload_requested(const QString& group);
    void air_read_requested();
    void air_apply_requested(const QJsonObject& values);
    void range_read_requested();

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
    void build_group(Group& group, const QString& title, const FieldSpec* specs, std::size_t count);
    Group* measurement_group(const QString& name);
    void build_lamp_card();
    void save_measurement(Group& group);
    void apply_values(Group& group, const QJsonObject& groups);
    void update_measurement_controls(Group& group);
    void set_air_busy(bool busy);
    void load_range_rules();
    void save_range_rules();
    void render_range();
    void render_lamp();

    Group laser_;
    Group spectrometer_;
    Group air_;
    Group range_;
    Card* lamp_card_ = nullptr;
    StatusDot* lamp_dot_ = nullptr;
    QComboBox* lamp_color_ = nullptr;
    QComboBox* lamp_mode_ = nullptr;
    QLabel* lamp_feedback_ = nullptr;
    QPushButton* lamp_preview_ = nullptr;
    QPushButton* lamp_restore_ = nullptr;
    QTimer* lamp_timer_ = nullptr;
    bool lamp_test_ = false;
    bool lamp_phase_ = true;
    int lamp_test_color_ = 0;
    MachineSnapshot snapshot_;
    bool measuring_ = false;
    bool air_ready_ = false;
    QJsonObject range_rules_;
    QJsonObject active_range_rules_;
    QJsonObject range_reading_;
    QLabel* range_result_ = nullptr;
};

}  // namespace libs::ui
