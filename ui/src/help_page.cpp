#include "help_page.hpp"

#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace libs::ui {

HelpPage::HelpPage(QWidget* parent) : QWidget(parent) {
    setAccessibleName(QStringLiteral("帮助"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(24, 18, 24, 22);
    layout->setSpacing(18);
    auto* sidebar = new QWidget(this);
    sidebar->setFixedWidth(224);
    auto* menu = new QVBoxLayout(sidebar);
    menu->setContentsMargins(0, 0, 0, 0);
    search_ = new QLineEdit(sidebar);
    search_->setAccessibleName(QStringLiteral("搜索帮助"));
    search_->setPlaceholderText(QStringLiteral("搜索参数或提示信息"));
    search_->setClearButtonEnabled(true);
    menu->addWidget(search_);
    navigation_ = new QListWidget(sidebar);
    navigation_->setObjectName(QStringLiteral("helpNavigation"));
    navigation_->setAccessibleName(QStringLiteral("帮助目录"));
    menu->addWidget(navigation_);
    layout->addWidget(sidebar);
    article_ = new QTextBrowser(this);
    article_->setObjectName(QStringLiteral("helpArticle"));
    article_->setAccessibleName(QStringLiteral("帮助正文"));
    article_->setOpenLinks(false);
    article_->document()->setDocumentMargin(24);
    article_->document()->setDefaultStyleSheet(QStringLiteral(
        "body,p,li,dd {color:#40536c;} h1 {color:#192a42;font-size:26pt;}"
        "h2 {color:#192a42;font-size:15pt;margin-top:20px;} dt {font-weight:600;color:#192a42;}"
        "dd {margin-bottom:8px;} a {color:#2563eb;} th, td {padding:6px;}"
        ".help-note {background-color:#f4f7fd;} .help-reference {font-size:10pt;}"));
    layout->addWidget(article_, 1);

    QFile source(QStringLiteral(":/ui/help/index.html"));
    if (source.open(QIODevice::ReadOnly)) {
        const auto html = QString::fromUtf8(source.readAll());
        const QRegularExpression articles(QStringLiteral(
            R"rx(<article\b[^>]*\bid="help-([^"]+)"[^>]*>(.*?)</article>)rx"),
            QRegularExpression::DotMatchesEverythingOption);
        auto matches = articles.globalMatch(html);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString id = match.captured(1);
            const QString body = match.captured(2);
            const auto heading = QRegularExpression(QStringLiteral("<h1[^>]*>([^<]+)</h1>")).match(body);
            if (!heading.hasMatch()) continue;
            QTextDocument search_document;
            search_document.setHtml(body);
            auto* item = new QListWidgetItem(heading.captured(1), navigation_);
            item->setData(Qt::UserRole, id);
            topics_.append({id, body, search_document.toPlainText(), item});
        }
    }
    connect(navigation_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item) show_topic(item->data(Qt::UserRole).toString());
    });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& query) {
        const auto words = query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const auto& topic : topics_) {
            const bool match = std::all_of(words.cbegin(), words.cend(), [&](const QString& word) {
                return topic.search_text.contains(word, Qt::CaseInsensitive);
            });
            topic.item->setHidden(!match);
        }
    });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        for (const auto& topic : topics_) {
            if (topic.item->isHidden()) continue;
            show_topic(topic.id);
            break;
        }
    });
    connect(article_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        const auto fragment = url.fragment();
        if (fragment == QLatin1String("help")) show_topic(QStringLiteral("start"));
        else if (fragment.startsWith(QStringLiteral("help/"))) show_topic(fragment.mid(5));
        else if (fragment.startsWith(QStringLiteral("parameters/"))) emit parameters_requested(fragment.mid(11));
        else if (url.scheme() == QLatin1String("https")) QDesktopServices::openUrl(url);
    });
    show_topic(QStringLiteral("start"));
}

void HelpPage::show_topic(const QString& id) {
    const auto found = std::find_if(topics_.cbegin(), topics_.cend(), [&id](const Topic& topic) { return topic.id == id; });
    if (found == topics_.cend()) {
        article_->setPlainText(QStringLiteral("帮助内容暂不可用。"));
        return;
    }
    if (found->item->isHidden()) search_->clear();
    if (navigation_->currentItem() != found->item) {
        navigation_->setCurrentItem(found->item);
        return;
    }
    article_->setHtml(found->html);
    article_->verticalScrollBar()->setValue(0);
}

}  // namespace libs::ui
