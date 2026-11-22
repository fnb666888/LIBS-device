#pragma once

#include <QVector>
#include <QWidget>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QTextBrowser;

namespace libs::ui {

class HelpPage : public QWidget {
    Q_OBJECT
public:
    explicit HelpPage(QWidget* parent = nullptr);
    void show_topic(const QString& id);

signals:
    void parameters_requested(const QString& id);

private:
    struct Topic {
        QString id;
        QString html;
        QString search_text;
        QListWidgetItem* item = nullptr;
    };

    QVector<Topic> topics_;
    QLineEdit* search_ = nullptr;
    QListWidget* navigation_ = nullptr;
    QTextBrowser* article_ = nullptr;
};

}  // namespace libs::ui
