#include "debug_window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>

#include <cmath>

namespace libs::ui {
namespace {

QString command_for(const DebugWindow::Page* page) {
    const QString a = page->action ? page->action->currentData().toString() : QString();
    if (page->command == QStringLiteral("overview")) return a;
    if (page->command == QStringLiteral("laser"))
        return a == QStringLiteral("capture") ? QStringLiteral("capture") : QStringLiteral("laser_fire");
    if (page->command == QStringLiteral("params")) return a;
    if (page->command == QStringLiteral("algorithm")) return QStringLiteral("algorithm");
    if (page->command == QStringLiteral("heike")) return QStringLiteral("heike_report_preview");
    if (page->command == QStringLiteral("recovery")) return a;
    if (page->command == QStringLiteral("context")) return QStringLiteral("debug_context");
    if (page->command == QStringLiteral("contract")) return QStringLiteral("debug_contract");
    if (page->command == QStringLiteral("motor")) return QStringLiteral("motor");
    return QStringLiteral("device_debug");
}

QString label_for(const QString& key) {
    static const QHash<QString, QString> labels = {
        {"device", "设备"}, {"action", "动作"}, {"start", "起始寄存器"},
        {"count", "寄存器数量"}, {"on", "开机"}, {"mode", "模式"},
        {"parameter", "参数"}, {"value", "数值"}, {"channel", "通道"},
        {"state", "状态"}, {"group", "参数组"}, {"values", "参数 JSON"},
        {"kind", "报文类型"}, {"input", "算法输入 JSON"}, {"reason", "原因"},
        {"target_belt", "目标带"}, {"belt_sequence", "带序号"},
        {"frequency_hz", "工作频率 Hz"}, {"unit_confirmed", "频率单位已确认"},
        {"calibrated", "频率已标定"},
    };
    return labels.value(key, key);
}

QString default_json(const QJsonObject& o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

QString field_text(QWidget* widget) {
    if (auto* line = qobject_cast<QLineEdit*>(widget)) return line->text();
    if (auto* edit = qobject_cast<QTextEdit*>(widget)) return edit->toPlainText();
    return widget ? widget->property("text").toString() : QString();
}

QString heike_template(const QString& kind) {
    if (kind == QStringLiteral("heartbeat"))
        return QStringLiteral("{\"equipment_id\":\"DEV-001\",\"equipment_type\":\"LIBS\",\"customs\":\"\",\"timeseries\":\"2026-01-01 00:00:00\"}");
    if (kind == QStringLiteral("event"))
        return QStringLiteral("{\"equipment_id\":\"DEV-001\",\"equipment_type\":\"LIBS\",\"customs\":\"\",\"timeseries\":\"2026-01-01 00:00:00\",\"code\":\"0\",\"value\":\"0\"}");
    if (kind == QStringLiteral("openinit"))
        return QStringLiteral("{\"equipment_id\":\"DEV-001\",\"equipment_type\":\"LIBS\",\"customs\":\"\",\"timeseries\":\"2026-01-01 00:00:00\",\"stats\":[]}");
    if (kind == QStringLiteral("alarm"))
        return QStringLiteral("{\"equipment_id\":\"DEV-001\",\"equipment_type\":\"LIBS\",\"customs\":\"\",\"timeseries\":\"2026-01-01 00:00:00\",\"alarms\":[]}");
    if (kind == QStringLiteral("composition"))
        return QStringLiteral("{\"biz_id\":\"\",\"customs_code\":\"\",\"message_id\":\"00000000-0000-0000-0000-000000000000\",\"event_source\":\"LIBS-device\",\"event_content\":{\"ID\":0,\"SEQ_NO\":null,\"DATA_SOURCE\":null,\"COMPO_AL2O3\":null,\"COMPO_CAO\":null,\"COMPO_FEO\":null,\"COMPO_MGO\":null,\"COMPO_MNO\":null,\"COMPO_P2O5\":null,\"COMPO_S\":null,\"COMPO_SIO2\":null,\"COMPO_TFE\":null,\"COMPO_TIO2\":null}}");
    if (kind == QStringLiteral("libs_raw"))
        return QStringLiteral("{\"event_name\":\"LIBS_RAW\",\"biz_id\":\"\",\"customs_code\":\"\",\"belt_code\":\"\",\"edge_code\":\"\",\"collect_time\":\"\",\"message_id\":\"00000000-0000-0000-0000-000000000000\",\"event_code\":\"\",\"raw_capture\":{\"schema\":\"libs.algorithm.v1\"}}");
    return QStringLiteral("{\"biz_id\":\"\",\"customs_code\":\"\",\"message_id\":\"00000000-0000-0000-0000-000000000000\",\"event_source\":\"LIBS-device\",\"event_content\":{}}");
}

bool number_key(const QString& key) {
    return key == QStringLiteral("start") || key == QStringLiteral("count") ||
           key == QStringLiteral("value") || key == QStringLiteral("belt_sequence") ||
           key == QStringLiteral("frequency_hz");
}

bool bool_key(const QString& key) {
    return key == QStringLiteral("on") || key == QStringLiteral("unit_confirmed") ||
           key == QStringLiteral("calibrated");
}

}  // namespace

DebugWindow::DebugWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(QStringLiteral("LIBS 手动调试台"));
    resize(1280, 820);
    auto* root = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    connection_state_ = new QLabel(QStringLiteral("仅用户点击时访问：%1").arg(client_.pipe_name()), this);
    connection_state_->setObjectName(QStringLiteral("debugConnection"));
    top->addWidget(connection_state_, 1);
    auto* retry = new QPushButton(QStringLiteral("连接测试"), this);
    top->addWidget(retry);
    connect(retry, &QPushButton::clicked, this, [this] {
        for (Page* p : pages_) if (p->command == QStringLiteral("overview")) {
            p->action->setCurrentIndex(p->action->findData(QStringLiteral("ping")));
            execute(p);
            break;
        }
    });
    root->addLayout(top);

    auto* split = new QSplitter(Qt::Horizontal, this);
    tabs_ = new QTabWidget(split);
    global_history_ = new QListWidget(split);
    global_history_->setMinimumWidth(280);
    global_history_->setToolTip(QStringLiteral("本窗口会话内的请求历史；历史记录不代表命令成功。"));
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    root->addWidget(split, 1);

    Page* overview = make_page(QStringLiteral("连接总览"), QStringLiteral("overview"),
                               {QStringLiteral("ping"), QStringLiteral("status"),
                                QStringLiteral("health"), QStringLiteral("config_show")});
    Page* laser = make_page(QStringLiteral("激光光谱"), QStringLiteral("laser"),
                           {QStringLiteral("precheck_only"), QStringLiteral("capture")});
    add_note(laser, QStringLiteral("调试页不会自动出光；预检仍经过后台门禁。真正出光请使用主页面既有控制。capture 使用 from_config=true。"));
    Page* laser_status = make_page(QStringLiteral("激光器状态"), QStringLiteral("device_debug"),
                                   {QStringLiteral("status")});
    Page* range = make_page(QStringLiteral("测距仪"), QStringLiteral("device_debug"), {QStringLiteral("read")});
    Page* motor = make_page(QStringLiteral("SU300 电机"), QStringLiteral("motor"),
                            {QStringLiteral("status"), QStringLiteral("forward"), QStringLiteral("reverse"),
                             QStringLiteral("jog_forward"), QStringLiteral("jog_reverse"),
                             QStringLiteral("stop"), QStringLiteral("reset_fault")});
    Page* ac = make_page(QStringLiteral("空调"), QStringLiteral("device_debug"),
                         {QStringLiteral("status"), QStringLiteral("set_power"), QStringLiteral("set_mode"),
                          QStringLiteral("set_setpoint")});
    Page* lamp = make_page(QStringLiteral("指示灯"), QStringLiteral("device_debug"),
                           {QStringLiteral("query"), QStringLiteral("write")});
    Page* params = make_page(QStringLiteral("参数"), QStringLiteral("params"),
                             {QStringLiteral("param_get"), QStringLiteral("param_set")});
    add_note(params, QStringLiteral("保存参数表示后台参数存储已处理，不等于设备硬件已经下发；本页写入使用 immediately。"));
    Page* algorithm = make_page(QStringLiteral("算法"), QStringLiteral("algorithm"),
                                {QStringLiteral("contract"), QStringLiteral("validate"), QStringLiteral("analyze")});
    add_note(algorithm, QStringLiteral("算法输入统一使用 schema_version=libs.algorithm.v1；导入 JSON 只更新请求，不会联网或自动分析。"));
    Page* heike = make_page(QStringLiteral("海科报文"), QStringLiteral("heike"),
                            {QStringLiteral("heartbeat"), QStringLiteral("event"), QStringLiteral("openinit"),
                             QStringLiteral("alarm"), QStringLiteral("composition"), QStringLiteral("model"),
                             QStringLiteral("libs_raw")});
    add_note(heike, QStringLiteral("仅构造并校验预览，transport=dry_run；不会向外网发送。"));
    Page* recovery = make_page(QStringLiteral("恢复"), QStringLiteral("recovery"),
                               {QStringLiteral("recovery_list"), QStringLiteral("recovery_write")});
    add_note(recovery, QStringLiteral("恢复页只读/写上下文快照，不会自动重发运动、激光或采集命令。"));
    Page* context = make_page(QStringLiteral("业务上下文"), QStringLiteral("context"),
                              {QStringLiteral("get"), QStringLiteral("set")});
    add_note(context, QStringLiteral("这里只维护手工元数据，不启动检测编排。"));
    Page* contract = make_page(QStringLiteral("接口契约"), QStringLiteral("contract"),
                               {QStringLiteral("protocol_contract")});
    add_note(contract, QStringLiteral("查看后端当前 debug_contract 协议，不执行设备动作。"));
    Q_UNUSED(overview)
    Q_UNUSED(range)
    Q_UNUSED(laser_status)
    Q_UNUSED(motor)
    Q_UNUSED(ac)
    Q_UNUSED(lamp)
    Q_UNUSED(params)
    Q_UNUSED(algorithm)
    Q_UNUSED(heike)
    Q_UNUSED(recovery)
    Q_UNUSED(context)
    Q_UNUSED(contract)
}

DebugWindow::~DebugWindow() = default;

DebugWindow::Page* DebugWindow::make_page(const QString& title, const QString& command,
                                          const QStringList& actions) {
    auto* p = new Page;
    p->title = title;
    p->command = command;
    pages_.push_back(p);
    auto* page_widget = new QWidget;
    auto* outer = new QVBoxLayout(page_widget);
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("动作："), page_widget));
    p->action = new QComboBox(page_widget);
    for (const QString& a : actions) p->action->addItem(a, a);
    bar->addWidget(p->action, 1);
    p->execute = new QPushButton(QStringLiteral("执行"), page_widget);
    p->execute->setToolTip(QStringLiteral("只执行当前动作；普通执行期间本页按钮会暂时禁用。"));
    bar->addWidget(p->execute);
    outer->addLayout(bar);

    p->form_host = new QWidget(page_widget);
    p->form = new QFormLayout(p->form_host);
    p->form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    outer->addWidget(p->form_host);
    p->request_group = new QGroupBox(QStringLiteral("请求 JSON（可展开编辑）"), page_widget);
    auto* request_layout = new QVBoxLayout(p->request_group);
    p->request = new QTextEdit(p->request_group);
    p->request->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    p->request->setMinimumHeight(120);
    request_layout->addWidget(p->request);
    outer->addWidget(p->request_group);

    auto* tools = new QHBoxLayout;
    auto* toggle_request = new QPushButton(QStringLiteral("收起请求 JSON"), page_widget);
    toggle_request->setCheckable(true);
    toggle_request->setChecked(false);
    tools->addWidget(toggle_request);
    auto* import = new QPushButton(QStringLiteral("导入算法输入"), page_widget);
    import->setVisible(command == QStringLiteral("algorithm"));
    tools->addWidget(import);
    p->copy_response = new QPushButton(QStringLiteral("复制响应"), page_widget);
    p->save_response = new QPushButton(QStringLiteral("保存响应"), page_widget);
    tools->addWidget(p->copy_response);
    tools->addWidget(p->save_response);
    tools->addStretch(1);
    p->status = new QLabel(QStringLiteral("未执行"), page_widget);
    tools->addWidget(p->status);
    outer->addLayout(tools);

    p->response = new QTextEdit(page_widget);
    p->response->setReadOnly(true);
    p->response->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    p->response->setPlaceholderText(QStringLiteral("响应会保留 ok=false 的完整业务报文"));
    outer->addWidget(p->response, 1);
    p->history = new QListWidget(page_widget);
    p->history->setMaximumHeight(92);
    p->history->setToolTip(QStringLiteral("本页请求历史"));
    outer->addWidget(p->history);

    connect(p->action, &QComboBox::currentIndexChanged, this, [this, p] {
        rebuild_form(p);
        // 运动过程中停止动作必须保持可发；这是安全动作，不能被普通动作的
        // busy 状态锁死。其它动作仍遵守同页单请求纪律。
        const bool stop = p->command == QStringLiteral("device_debug") &&
                          p->action->currentData().toString() == QStringLiteral("stop");
        p->execute->setEnabled(!p->busy || stop);
    });
    connect(toggle_request, &QPushButton::toggled, this, [p, toggle_request](bool collapsed) {
        p->request_group->setVisible(!collapsed);
        toggle_request->setText(collapsed ? QStringLiteral("展开请求 JSON") : QStringLiteral("收起请求 JSON"));
    });
    connect(p->execute, &QPushButton::clicked, this, [this, p] { execute(p); });
    connect(import, &QPushButton::clicked, this, [this, p] { choose_algorithm_input(p); });
    connect(p->copy_response, &QPushButton::clicked, this, [this, p] { copy_response(p); });
    connect(p->save_response, &QPushButton::clicked, this, [this, p] { save_response(p); });
    tabs_->addTab(page_widget, title);
    rebuild_form(p);
    return p;
}

void DebugWindow::set_field(Page* p, const QString& key, QWidget* widget) {
    p->fields.insert(key, widget);
    p->form->addRow(label_for(key) + QStringLiteral("："), widget);
}

QLineEdit* DebugWindow::line(Page* p, const QString& key, const QString& value) {
    auto* edit = new QLineEdit(value, p->form_host);
    edit->setObjectName(key);
    set_field(p, key, edit);
    return edit;
}

void DebugWindow::add_common_actions(Page* p, const QStringList& actions) {
    Q_UNUSED(p)
    Q_UNUSED(actions)
}

void DebugWindow::rebuild_form(Page* p) {
    while (QLayoutItem* item = p->form->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    p->fields.clear();
    const QString action = p->action->currentData().toString();
    if (p->command == QStringLiteral("rangefinder")) {
        line(p, QStringLiteral("start"), QStringLiteral("512"));
        line(p, QStringLiteral("count"), QStringLiteral("9"));
    } else if (p->command == QStringLiteral("device_debug")) {
        QString device;
        if (p->title == QStringLiteral("测距仪")) device = QStringLiteral("rangefinder");
        else if (p->title == QStringLiteral("激光器状态")) device = QStringLiteral("laser");
        else if (p->title == QStringLiteral("SU300 电机")) device = QStringLiteral("motor");
        else if (p->title == QStringLiteral("空调")) device = QStringLiteral("air_conditioner");
        else if (p->title == QStringLiteral("指示灯")) device = QStringLiteral("indicator");
        line(p, QStringLiteral("device"), device);
        line(p, QStringLiteral("action"), action);
        if (p->title == QStringLiteral("测距仪")) {
            line(p, QStringLiteral("start"), QStringLiteral("512"));
            line(p, QStringLiteral("count"), QStringLiteral("9"));
        } else if (p->title == QStringLiteral("SU300 电机") && action != QStringLiteral("status") && action != QStringLiteral("stop") && action != QStringLiteral("reset_fault")) {
            line(p, QStringLiteral("frequency_percent"), QStringLiteral("10"));
            line(p, QStringLiteral("duration_ms"), QStringLiteral("1000"));
        } else if (p->title == QStringLiteral("空调")) {
            if (action == QStringLiteral("set_power")) line(p, QStringLiteral("on"), QStringLiteral("true"));
            if (action == QStringLiteral("set_mode")) line(p, QStringLiteral("mode"), QStringLiteral("cooling"));
            if (action == QStringLiteral("set_setpoint")) {
                line(p, QStringLiteral("parameter"), QStringLiteral("cooling_on"));
                line(p, QStringLiteral("value"), QStringLiteral("25"));
            }
        } else if (p->title == QStringLiteral("指示灯")) {
            line(p, QStringLiteral("channel"), QStringLiteral("red"));
            if (action == QStringLiteral("write")) line(p, QStringLiteral("state"), QStringLiteral("on"));
        }
    } else if (p->command == QStringLiteral("motor")) {
        if (action != QStringLiteral("status") && action != QStringLiteral("stop") && action != QStringLiteral("reset_fault")) {
            line(p, QStringLiteral("frequency_percent"), QStringLiteral("10"));
            line(p, QStringLiteral("duration_ms"), QStringLiteral("1000"));
        }
    } else if (p->command == QStringLiteral("params")) {
        line(p, QStringLiteral("group"), QStringLiteral("laser"));
        if (action == QStringLiteral("param_set")) {
            auto* values = new QTextEdit(p->form_host);
            values->setPlainText(QStringLiteral("{}"));
            values->setMinimumHeight(70);
            set_field(p, QStringLiteral("values"), values);
        }
    } else if (p->command == QStringLiteral("algorithm")) {
        if (action == QStringLiteral("validate") || action == QStringLiteral("analyze")) {
            auto* input = new QTextEdit(p->form_host);
            input->setPlainText(QStringLiteral("{\"schema_version\":\"libs.algorithm.v1\",\"request_id\":\"debug-001\",\"context\":{},\"spectrum\":{\"spectrum_source\":\"LIBS\",\"spectrum_format\":\"wavelength_nm+counts\",\"scan_groups\":[]},\"parameters\":{},\"model\":{}}"));
            input->setMinimumHeight(100);
            set_field(p, QStringLiteral("input"), input);
        }
    } else if (p->command == QStringLiteral("heike")) {
        auto* profile = new QComboBox(p->form_host);
        profile->addItems({QStringLiteral("LIBS"), QStringLiteral("近红外兼容"), QStringLiteral("通用物模型")});
        set_field(p, QStringLiteral("profile"), profile);
        line(p, QStringLiteral("kind"), action);
        auto* payload = new QTextEdit(p->form_host);
        payload->setPlainText(heike_template(action));
        payload->setMinimumHeight(100);
        set_field(p, QStringLiteral("payload"), payload);
    } else if (p->command == QStringLiteral("recovery")) {
        if (action == QStringLiteral("recovery_write")) line(p, QStringLiteral("reason"), QStringLiteral("手动调试快照"));
    } else if (p->command == QStringLiteral("context")) {
        if (action == QStringLiteral("set")) {
            line(p, QStringLiteral("target_belt"));
            line(p, QStringLiteral("belt_sequence"), QStringLiteral("-1"));
            line(p, QStringLiteral("frequency_hz"));
            line(p, QStringLiteral("unit_confirmed"), QStringLiteral("false"));
            line(p, QStringLiteral("calibrated"), QStringLiteral("false"));
        }
    }
    QString error;
    const QJsonObject args = args_from_page(p, &error);
    QJsonObject req;
    req.insert(QStringLiteral("command"), command_for(p));
    req.insert(QStringLiteral("args"), args);
    p->request->setPlainText(pretty(req));
}

QJsonObject DebugWindow::args_from_page(Page* p, QString* error) const {
    QJsonObject a;
    const QString action = p->action->currentData().toString();
    if (p->command == QStringLiteral("laser")) {
        if (action == QStringLiteral("precheck_only")) a.insert(QStringLiteral("precheck_only"), true);
        else a.insert(QStringLiteral("from_config"), true);
    } else if (p->command == QStringLiteral("rangefinder")) {
        a.insert(QStringLiteral("device"), QStringLiteral("rangefinder"));
        a.insert(QStringLiteral("action"), QStringLiteral("read"));
        a.insert(QStringLiteral("start"), field_text(p->fields.value("start")).toInt());
        a.insert(QStringLiteral("count"), field_text(p->fields.value("count")).toInt());
    } else if (p->command == QStringLiteral("device_debug")) {
        for (auto it = p->fields.cbegin(); it != p->fields.cend(); ++it) {
            const QString key = it.key();
            const QString text = field_text(it.value());
            if (text.isEmpty() && key != QStringLiteral("action")) continue;
            if (bool_key(key)) a.insert(key, text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("1"));
            else if (number_key(key)) a.insert(key, text.toDouble());
            else a.insert(key, text);
        }
    } else if (p->command == QStringLiteral("motor")) {
        a.insert(QStringLiteral("action"), action);
        if (action != QStringLiteral("status") && action != QStringLiteral("stop") && action != QStringLiteral("reset_fault")) {
            a.insert(QStringLiteral("frequency_percent"), p->fields.value("frequency_percent")->property("text").toString().toDouble());
            a.insert(QStringLiteral("duration_ms"), p->fields.value("duration_ms")->property("text").toString().toDouble());
        }
    } else if (p->command == QStringLiteral("params")) {
        a.insert(QStringLiteral("group"), field_text(p->fields.value("group")));
        if (action == QStringLiteral("param_set")) {
            QJsonObject values = parse_object(field_text(p->fields.value("values")), error);
            if (error && !error->isEmpty()) return {};
            a.insert(QStringLiteral("values"), values);
            a.insert(QStringLiteral("timing"), QStringLiteral("immediately"));
        }
    } else if (p->command == QStringLiteral("algorithm")) {
        a.insert(QStringLiteral("action"), action);
        if (action == QStringLiteral("validate") || action == QStringLiteral("analyze")) {
            const QJsonObject input = parse_object(field_text(p->fields.value("input")), error);
            if (error && !error->isEmpty()) return {};
            a.insert(QStringLiteral("input"), input);
        }
    } else if (p->command == QStringLiteral("heike")) {
        a.insert(QStringLiteral("kind"), action);
        const QJsonObject payload = parse_object(field_text(p->fields.value("payload")), error);
        if (error && !error->isEmpty()) return {};
        for (auto it = payload.begin(); it != payload.end(); ++it) a.insert(it.key(), it.value());
    } else if (p->command == QStringLiteral("recovery")) {
        if (action == QStringLiteral("recovery_write")) a.insert(QStringLiteral("reason"), field_text(p->fields.value("reason")));
    } else if (p->command == QStringLiteral("context")) {
        a.insert(QStringLiteral("action"), action);
        if (action == QStringLiteral("set")) {
            a.insert(QStringLiteral("target_belt"), field_text(p->fields.value("target_belt")));
            a.insert(QStringLiteral("belt_sequence"), field_text(p->fields.value("belt_sequence")).toLongLong());
            const QString frequency = field_text(p->fields.value("frequency_hz"));
            if (!frequency.trimmed().isEmpty()) a.insert(QStringLiteral("frequency_hz"), frequency.toDouble());
            for (const QString& key : {QStringLiteral("unit_confirmed"), QStringLiteral("calibrated")}) {
                const QString value = field_text(p->fields.value(key));
                a.insert(key, value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("1"));
            }
        }
    } else if (p->command == QStringLiteral("contract")) {
        a.insert(QStringLiteral("action"), QStringLiteral("protocol_contract"));
    }
    return a;
}

QJsonObject DebugWindow::request_from_page(Page* p, QString* error) const {
    return parse_object(p->request->toPlainText(), error);
}

QJsonObject DebugWindow::parse_object(const QString& text, QString* error) {
    QJsonParseError e{};
    const QJsonDocument d = QJsonDocument::fromJson(text.toUtf8(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject()) {
        if (error) *error = QStringLiteral("请求必须是合法 JSON 对象：%1").arg(e.errorString());
        return {};
    }
    if (error) error->clear();
    return d.object();
}

QString DebugWindow::pretty(const QJsonObject& value) const { return default_json(value); }

void DebugWindow::execute(Page* p) {
    const bool stop = (p->command == QStringLiteral("device_debug") || p->command == QStringLiteral("motor")) &&
                      p->action->currentData().toString() == QStringLiteral("stop");
    if (p->busy && !stop) return;
    QString error;
    QJsonObject request = request_from_page(p, &error);
    if (!error.isEmpty()) {
        // 表单刚切换时更新一次编辑器；之后始终尊重用户手工编辑的 JSON。
        const QJsonObject args = args_from_page(p, &error);
        if (error.isEmpty()) {
            request.insert(QStringLiteral("command"), command_for(p));
            request.insert(QStringLiteral("args"), args);
            p->request->setPlainText(pretty(request));
        } else {
            p->status->setText(error);
            return;
        }
    }
    const QString command = request.value(QStringLiteral("command")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();
    const QString action = args.value(QStringLiteral("action")).toString();
    const bool precheck = command == QStringLiteral("laser_fire") &&
                          args.value(QStringLiteral("precheck_only")).toBool(false);
    if (!precheck && action_is_write(command, action) && !action_is_stop(command, action)) {
        if (QMessageBox::question(this, QStringLiteral("确认调试写操作"),
                                  QStringLiteral("将向后台发送“%1 / %2”写命令。继续吗？").arg(command, action),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    execute_json(p, request);
}

void DebugWindow::execute_json(Page* p, const QJsonObject& request) {
    p->busy = true;
    p->execute->setEnabled(false);
    p->status->setText(QStringLiteral("执行中…"));
    const QString pipe = client_.pipe_name();
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QPointer<DebugWindow> self(this);
    QThread* thread = QThread::create([self, p, request, pipe, started] {
        QJsonObject response;
        QElapsedTimer timer;
        timer.start();
        try {
            PipeClient client(pipe);
            response = client.call(request, 30000);
        } catch (const QString& e) {
            response.insert(QStringLiteral("ok"), false);
            response.insert(QStringLiteral("error"), e);
            response.insert(QStringLiteral("error_kind"), QStringLiteral("transport"));
        } catch (...) {
            response.insert(QStringLiteral("ok"), false);
            response.insert(QStringLiteral("error"), QStringLiteral("未知线程异常"));
            response.insert(QStringLiteral("error_kind"), QStringLiteral("internal"));
        }
        const qint64 elapsed = timer.elapsed();
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, p, request, response, elapsed, started] {
            if (!self) return;
            p->busy = false;
            p->execute->setEnabled(true);
            self->render_result(p, request, response, elapsed);
            self->connection_state_->setText(QStringLiteral("最近请求：%1 ms · %2")
                                             .arg(elapsed)
                                             .arg(response.value(QStringLiteral("ok")).toBool(false)
                                                      ? QStringLiteral("ok") : QStringLiteral("失败/业务拒绝")));
            Q_UNUSED(started)
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void DebugWindow::render_result(Page* p, const QJsonObject& request,
                                const QJsonObject& response, qint64 elapsed_ms) {
    const bool ok = response.value(QStringLiteral("ok")).toBool(false);
    p->response->setPlainText(pretty(response));
    p->status->setText(QStringLiteral("%1 · %2 ms").arg(ok ? QStringLiteral("成功") : QStringLiteral("失败/拒绝")).arg(elapsed_ms));
    add_history(p, request, response, elapsed_ms);
}

void DebugWindow::add_history(Page* p, const QJsonObject& request,
                              const QJsonObject& response, qint64 elapsed_ms) {
    const bool ok = response.value(QStringLiteral("ok")).toBool(false);
    const QString summary = QStringLiteral("%1 · %2 · %3 ms")
                                .arg(request.value(QStringLiteral("command")).toString())
                                .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"))
                                .arg(elapsed_ms);
    p->history->insertItem(0, summary);
    global_history_->insertItem(0, QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss ")) + summary);
    while (p->history->count() > 20) delete p->history->takeItem(p->history->count() - 1);
    while (global_history_->count() > 100) delete global_history_->takeItem(global_history_->count() - 1);
}

void DebugWindow::choose_algorithm_input(Page* p) {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入算法输入 JSON"), QString(), QStringLiteral("JSON (*.json);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { p->status->setText(QStringLiteral("无法读取算法输入文件")); return; }
    const QString text = QString::fromUtf8(f.readAll());
    QString error;
    parse_object(text, &error);
    if (!error.isEmpty()) { p->status->setText(error); return; }
    if (auto* w = qobject_cast<QLineEdit*>(p->fields.value(QStringLiteral("input")))) w->setText(text);
    if (auto* w = qobject_cast<QTextEdit*>(p->fields.value(QStringLiteral("input")))) w->setPlainText(text);
    QString request_error;
    const QJsonObject args = args_from_page(p, &request_error);
    if (request_error.isEmpty()) {
        QJsonObject request;
        request.insert(QStringLiteral("command"), command_for(p));
        request.insert(QStringLiteral("args"), args);
        p->request->setPlainText(pretty(request));
    }
    p->status->setText(QStringLiteral("算法输入已导入请求编辑器"));
}

void DebugWindow::copy_response(Page* p) {
    QApplication::clipboard()->setText(p->response->toPlainText());
    p->status->setText(QStringLiteral("响应已复制"));
}

void DebugWindow::save_response(Page* p) {
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存响应"), QStringLiteral("debug-response.json"), QStringLiteral("JSON (*.json);;文本文件 (*)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(p->response->toPlainText().toUtf8()) < 0 || !file.commit()) {
        p->status->setText(QStringLiteral("保存响应失败"));
        return;
    }
    p->status->setText(QStringLiteral("响应已保存：%1").arg(path));
}

void DebugWindow::add_note(Page* p, const QString& text) {
    auto* note = new QLabel(text, p->form_host->parentWidget());
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#b7c5d6; padding:4px;"));
    auto* layout = qobject_cast<QVBoxLayout*>(p->form_host->parentWidget()->layout());
    if (layout) layout->insertWidget(1, note);
}

bool DebugWindow::action_is_stop(const QString& command, const QString& action) {
    return (command == QStringLiteral("device_debug") || command == QStringLiteral("motor")) && action == QStringLiteral("stop");
}

bool DebugWindow::action_is_write(const QString& command, const QString& action) {
    if (command == QStringLiteral("device_debug"))
        return action != QStringLiteral("status") && action != QStringLiteral("read") &&
               action != QStringLiteral("query");
    if (command == QStringLiteral("motor"))
        return action != QStringLiteral("status") && action != QStringLiteral("stop");
    if (command == QStringLiteral("motor")) return action != QStringLiteral("status") && action != QStringLiteral("stop");
    if (command == QStringLiteral("param_set")) return true;
    if (command == QStringLiteral("debug_context")) return action == QStringLiteral("set");
    if (command == QStringLiteral("recovery_write")) return true;
    return command == QStringLiteral("laser_fire") && action != QStringLiteral("precheck_only");
}

}  // namespace libs::ui
