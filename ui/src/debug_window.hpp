#pragma once

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "pipe_client.hpp"

class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTextEdit;

namespace libs::ui {

// 手动调试窗口：只组装并发送 JSON 命令，不复制业务编排，也不绕过后端门禁。
// 该类刻意不使用 Q_OBJECT，便于作为独立窗口逐步接入现有 UI。
class DebugWindow final : public QWidget {
public:
    explicit DebugWindow(QWidget* parent = nullptr);
    ~DebugWindow() override;

    // Public only so the implementation's small form helpers can name the
    // per-tab state; callers should treat Page as an implementation detail.
    struct Page {
        QString title;
        QString command;
        QComboBox* action = nullptr;
        QWidget* form_host = nullptr;
        QFormLayout* form = nullptr;
        QGroupBox* request_group = nullptr;
        QTextEdit* request = nullptr;
        QTextEdit* response = nullptr;
        QLabel* status = nullptr;
        QPushButton* execute = nullptr;
        QPushButton* copy_response = nullptr;
        QPushButton* save_response = nullptr;
        QListWidget* history = nullptr;
        bool busy = false;
        QHash<QString, QWidget*> fields;
    };

private:

    Page* make_page(const QString& title, const QString& command,
                    const QStringList& actions);
    void rebuild_form(Page* page);
    QJsonObject request_from_page(Page* page, QString* error) const;
    QJsonObject args_from_page(Page* page, QString* error) const;
    void execute(Page* page);
    void execute_json(Page* page, const QJsonObject& request);
    void render_result(Page* page, const QJsonObject& request,
                       const QJsonObject& response, qint64 elapsed_ms);
    void set_field(Page* page, const QString& key, QWidget* widget);
    QLineEdit* line(Page* page, const QString& key, const QString& value = {});
    void add_note(Page* page, const QString& text);
    void add_common_actions(Page* page, const QStringList& actions);
    void choose_algorithm_input(Page* page);
    void save_response(Page* page);
    void copy_response(Page* page);
    void add_history(Page* page, const QJsonObject& request,
                     const QJsonObject& response, qint64 elapsed_ms);
    QString pretty(const QJsonObject& value) const;
    static QJsonObject parse_object(const QString& text, QString* error);
    static bool action_is_write(const QString& command, const QString& action);
    static bool action_is_stop(const QString& command, const QString& action);

    PipeClient client_;
    QTabWidget* tabs_ = nullptr;
    QListWidget* global_history_ = nullptr;
    QLabel* connection_state_ = nullptr;
    QVector<Page*> pages_;
};

}  // namespace libs::ui
