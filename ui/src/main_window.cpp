#include "main_window.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QUuid>
#include <QScrollArea>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QDir>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QStackedWidget>
#include <QTextStream>
#include <QStringConverter>
#include <QTimer>
#include <QVBoxLayout>

#include "parameter_page.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include <cmath>

namespace libs::ui {
namespace {

// 常驻永久免责声明。放在最显眼处而不是"关于"对话框里：
// 看到一条漂亮的光谱曲线却不知道它是否落了盘，是这个界面最大的误读风险。
//
// 注意文案：以前这里写的是"自由触发、未出光、背景谱"，那个模式已经删掉了 ——
// 采集只有一个来源，就是测量作业。现在图上每一帧都是落光射度谱，
// 所以免责声明只说剩下的那件事：**内存里有曲线 ≠ 已落盘**。
const char* kDisclaimer =
    "实时显示激光触发采集的光谱。数据保存进度可在下方采集状态中查看。";

QString humanize_reason_impl(const QString& code) {
    // 码表抄自 readiness/verdict.hpp 的 GateReason，**不认识的码原样透传** ——
    // 宁可显示一个后端新加的英文码，也不要静默吞掉一个自己猜错的原因：
    // 吞掉原因码 = 现场拿不到排障方向。
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("ready"),                 QStringLiteral("已就绪")},
        {QStringLiteral("parameters_incomplete"), QStringLiteral("测量参数未齐备，请到参数页补齐")},
        {QStringLiteral("laser_disabled"),        QStringLiteral("激光器在配置中未启用")},
        {QStringLiteral("laser_unavailable"),     QStringLiteral("读不到激光器状态，按不安全处理")},
        {QStringLiteral("laser_fault"),           QStringLiteral("激光器报故障")},
        {QStringLiteral("over_temperature"),      QStringLiteral("激光器超温")},
        {QStringLiteral("not_armed"),             QStringLiteral("门禁未布防，请先执行设备预检")},
        {QStringLiteral("interlock_open"),        QStringLiteral("联锁断开")},
        {QStringLiteral("emergency_stopped"),     QStringLiteral("急停已触发")},
        {QStringLiteral("q_gate_open"),           QStringLiteral("光闸处于打开位置")},
        {QStringLiteral("indicator_unavailable"), QStringLiteral("状态灯不可用（不影响出光判定）")},
        {QStringLiteral("devices_not_opened"),    QStringLiteral("设备尚未全部打开")},
        {QStringLiteral("spectrometer_not_ready"), QStringLiteral("光谱仪未就绪")},
        {QStringLiteral("laser_not_commissioned"), QStringLiteral("出光参数未在参数管理页面标定齐备，拒绝出光")},
    };
    const auto it = kNames.constFind(code);
    return it == kNames.constEnd() ? code : it.value();
}

QString humanize_state_impl(const QString& s) {
    if (s == QLatin1String("offline"))  return QStringLiteral("设备离线");
    if (s == QLatin1String("standby"))  return QStringLiteral("待机（未布防）");
    if (s == QLatin1String("ready"))     return QStringLiteral("就绪");
    if (s == QLatin1String("armed"))     return QStringLiteral("已布防");
    if (s == QLatin1String("acquiring")) return QStringLiteral("采集中");
    if (s == QLatin1String("fault"))     return QStringLiteral("故障");
    if (s.isEmpty())                     return QStringLiteral("未知");
    return s;
}

}  // namespace

QString MainWindow::humanize_reason(const QString& code) { return humanize_reason_impl(code); }
QString MainWindow::humanize_state(const QString& s) { return humanize_state_impl(s); }

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("LIBS 现场检测系统"));
    resize(1440, 900);
    setMinimumSize(1180, 720);

    poller_ = new StreamPoller(client_, this);
    connect(poller_, &StreamPoller::frame_ready, this, &MainWindow::on_frame);
    connect(poller_, &StreamPoller::capture_failed, this, &MainWindow::on_capture_failed);
    connect(poller_, &StreamPoller::stream_error, this, &MainWindow::on_stream_error);
    connect(poller_, &StreamPoller::stats_changed, this, &MainWindow::on_stats);

    state_poller_ = new StatusPoller(client_, this);
    connect(state_poller_, &StatusPoller::snapshot_ready, this, &MainWindow::on_snapshot);
    state_poller_->start(800);

    // 全程订阅光谱展示通道。**不随测量开始/停止** ——
    // 订阅只是"我准备好看"，不发任何采集指令；采集的唯一来源是
    // measurement_start。以前把它绑在按钮上，结果停止检测后连
    // 「上次那一发到底采到没有」都没法回头看。
    start_stream();

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    build_header(root);

    banner_ = new QLabel(central);
    banner_->setWordWrap(true);
    banner_->setObjectName(QStringLiteral("banner"));
    auto* banner_wrap = new QWidget(central);
    auto* bw = new QVBoxLayout(banner_wrap);
    bw->setContentsMargins(24, 14, 24, 0);
    bw->addWidget(banner_);
    root->addWidget(banner_wrap);
    set_banner_default();

    pages_ = new QStackedWidget(central);
    param_page_ = new ParameterPage(pages_);
    connect(param_page_, &ParameterPage::save_requested, this, &MainWindow::on_param_save_requested);
    connect(param_page_, &ParameterPage::reload_requested, this, &MainWindow::on_param_reload_requested);
    pages_->addWidget(build_main_page());   // 0 = 主页面
    pages_->addWidget(param_page_);         // 1 = 参数页
    root->addWidget(pages_, 1);

    setCentralWidget(central);

    prediction_poller_ = new PredictionPoller(client_, this);
    connect(prediction_poller_, &PredictionPoller::status_ready, this, &MainWindow::render_prediction);
    connect(prediction_poller_, &PredictionPoller::polling_error, this, [this](const QString& message) {
        if (prediction_configuring_) return;
        prediction_status_->setText(QStringLiteral("预测连接暂时中断，保留上次结果。"));
        prediction_status_->setToolTip(message);
    });
    prediction_poller_->start();

    auto* clock_timer = new QTimer(this);
    connect(clock_timer, &QTimer::timeout, this, &MainWindow::tick_clock);
    clock_timer->start(1000);
    tick_clock();
}

MainWindow::~MainWindow() {
    // 退订必须在 QObject 析构前做：否则守护进程那边留一个
    // 订阅号，它的队列没人取，每帧都被丢一次。
    stop_stream();
}

void MainWindow::build_header(QVBoxLayout* root) {
    auto* header = new QWidget();
    header->setObjectName(QStringLiteral("appHeader"));
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(24, 16, 24, 16);
    h->setSpacing(16);
    auto* brand = new QLabel(QStringLiteral("LIBS"), header);
    brand->setObjectName(QStringLiteral("brandMark"));
    brand->setAlignment(Qt::AlignCenter);
    brand->setFixedSize(58, 48);
    h->addWidget(brand);
    auto* brand_text = new QVBoxLayout;
    brand_text->setSpacing(3);

    title_ = new QLabel(QStringLiteral("LIBS 现场检测系统"), header);
    title_->setObjectName(QStringLiteral("appTitle"));
    // 标题优先级最高：设最小宽度，长详情再怎么长也不会把"LIBS 现场检测系统"挤没。
    title_->setMinimumWidth(190);
    brand_text->addWidget(title_);
    auto* subtitle = new QLabel(QStringLiteral("激光诱导击穿光谱 · 测量工作台"), header);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    brand_text->addWidget(subtitle);
    h->addLayout(brand_text);

    h->addSpacing(8);

    daemon_dot_ = new StatusDot(header);
    daemon_dot_->set_state(Level::Idle, QStringLiteral("服务未连接"));
    h->addWidget(daemon_dot_);

    gate_dot_ = new StatusDot(header);

    // 顶栏宽度封顶的原因：现场屏只有 1536px，守护进程报错时那条详情（管道名 +
    // Win32 错误码 + 提示）能把右侧按钮整排顶出可视区，用户就点不到「测量参数」。
    // 两个点分开设：守护进程的错误详情长，门禁的短。
    daemon_dot_->set_max_width(180);
    gate_dot_->set_state(Level::Idle, QStringLiteral("门禁状态未知"));
    h->addWidget(gate_dot_);
    gate_dot_->set_max_width(230);

    h->addStretch(1);

    param_page_btn_ = new QPushButton(QStringLiteral("仪器参数"), header);
    param_page_btn_->setObjectName(QStringLiteral("navTab"));
    param_page_btn_->setCheckable(true);
    // 顶栏挤不下时 Qt 会先压这些按钮。锁住尺寸，宁可压上面的详情文本。
    param_page_btn_->setMinimumWidth(96);
    connect(param_page_btn_, &QPushButton::clicked, this, &MainWindow::show_parameter_page);

    main_page_btn_ = new QPushButton(QStringLiteral("实时光谱"), header);
    main_page_btn_->setObjectName(QStringLiteral("navTab"));
    main_page_btn_->setCheckable(true);
    main_page_btn_->setChecked(true);
    main_page_btn_->setMinimumWidth(110);
    connect(main_page_btn_, &QPushButton::clicked, this, &MainWindow::show_main_page);
    h->addWidget(main_page_btn_);
    h->addWidget(param_page_btn_);

    clock_ = new QLabel(header);
    clock_->setObjectName(QStringLiteral("appClock"));
    clock_->setMinimumWidth(150);   // "2026-10-01 18:57:00" 的宽度，防止被压成竖排
    h->addWidget(clock_);

    root->addWidget(header);
}

// ── 主页面 ──────────────────────────────────────────────────────────────

QWidget* MainWindow::build_main_page() {
    auto* page = new QWidget();
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(24, 18, 24, 22);
    root->setSpacing(16);

    build_action_bar(root);
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* center = new QHBoxLayout(content);
    center->setContentsMargins(0, 0, 4, 0);
    center->setSizeConstraint(QLayout::SetMinimumSize);
    center->setSpacing(16);
    auto* spectrum = new QVBoxLayout;
    spectrum->setContentsMargins(0, 0, 0, 0);
    build_chart(spectrum);
    center->addLayout(spectrum, 1);
    auto* prediction = build_prediction_card();
    prediction->setMinimumWidth(290);
    prediction->setMaximumWidth(340);
    center->addWidget(prediction);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
    build_device_bar(root);
    return page;
}

void MainWindow::build_action_bar(QVBoxLayout* root) {
    auto* bar = new Card(QStringLiteral("检测控制"));
    bar->set_hint(QStringLiteral("准备设备后开始测量，实时查看光谱和采集状态。"));
    auto* row = new QHBoxLayout;
    row->setSpacing(16);
    bar->body()->addLayout(row);

    // ① 设备预检 —— 只读检测。一条激光写指令都不发。
    //    它做的是：布防 -> 读一次激光状态 -> 判定 -> 按结论刷状态灯。
    precheck_btn_ = new QPushButton(QStringLiteral("设备预检"), bar);
    precheck_btn_->setObjectName(QStringLiteral("actionBtn"));
    precheck_btn_->setMinimumSize(140, 48);
    precheck_btn_->setToolTip(QStringLiteral(
        "检查设备连接与激光器状态。\n"
        "预检不会触发激光出光。"));
    connect(precheck_btn_, &QPushButton::clicked, this, &MainWindow::on_precheck);
    row->addWidget(precheck_btn_);

    // ② 开始检测 —— 发 measurement_start。成功即已开光。
    //    可用性**只**由 status.laser_running 决定（见 render_buttons）。
    start_btn_ = new QPushButton(QStringLiteral("开始检测"), bar);
    start_btn_->setObjectName(QStringLiteral("startBig"));
    start_btn_->setMinimumSize(140, 48);
    start_btn_->setToolTip(QStringLiteral(
        "启动激光触发光谱采集，持续显示并保存数据。\n"
        "启动前自动检查设备状态与测量条件。"));
    connect(start_btn_, &QPushButton::clicked, this, &MainWindow::on_start_detection);
    row->addWidget(start_btn_);

    // ③ 停止检测 —— 发 measurement_stop。不做确认弹窗。
    stop_btn_ = new QPushButton(QStringLiteral("停止检测"), bar);
    stop_btn_->setObjectName(QStringLiteral("stopBig"));
    stop_btn_->setMinimumSize(140, 48);
    stop_btn_->setToolTip(QStringLiteral(
        "停止测量、关闭激光，并保存剩余光谱数据。"));
    connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::on_stop_detection);
    row->addWidget(stop_btn_);

    row->addStretch(1);
    root->addWidget(bar);
}

void MainWindow::build_chart(QVBoxLayout* root) {
    auto* card = new Card(QStringLiteral("实时光谱"));
    card->set_hint(QStringLiteral(
        "各通道按独立颜色显示，支持纵轴调整、暂停显示与原始数据导出。"));

    auto* tools = new QHBoxLayout;
    auto_y_ = new QCheckBox(QStringLiteral("纵轴自动"), card);
    auto_y_->setChecked(true);
    connect(auto_y_, &QCheckBox::toggled, this, &MainWindow::on_auto_y_toggled);
    tools->addWidget(auto_y_);

    lock_y_ = new QCheckBox(QStringLiteral("锁定纵轴"), card);
    connect(lock_y_, &QCheckBox::toggled, this, &MainWindow::on_lock_y_toggled);
    tools->addWidget(lock_y_);

    pause_ = new QCheckBox(QStringLiteral("暂停刷新"), card);
    pause_->setToolTip(QStringLiteral(
        "只暂停画面刷新，不影响采集，也不停止落盘。"));
    connect(pause_, &QCheckBox::toggled, this, &MainWindow::on_pause_toggled);
    tools->addWidget(pause_);

    auto* export_btn = new QPushButton(QStringLiteral("导出数据"), card);
    export_btn->setObjectName(QStringLiteral("ghost"));
    export_btn->setToolTip(QStringLiteral(
        "导出当前会话已收到的每一帧的原始计数，不做归一化。\n"
        "用于离线复核；正式数据以守护进程落盘的为准。"));
    connect(export_btn, &QPushButton::clicked, this, &MainWindow::on_export_csv);
    tools->addWidget(export_btn);

    tools->addStretch(1);
    absent_label_ = make_hint(QString());
    tools->addWidget(absent_label_);
    card->body()->addLayout(tools);

    plot_ = new SpectrumPlot(card);
    card->body()->addWidget(plot_, 1);

    frame_label_ = make_hint(QStringLiteral("等待采集 · 尚无光谱数据"));
    card->body()->addWidget(frame_label_);

    // 这张卡片以前建完就没人管了：build_chart 不接收 root，卡片既没被
    // addWidget 进页面，也没设父对象，于是整个光谱区压根不在界面上，
    // 剩下的伸缩空间全被"检测控制"卡片吃掉，看起来就是一大片空白。
    // 伸缩比 1 给绘图区：现场主要看的就是这条谱。
    root->addWidget(card, 1);
    chart_card_ = card;
}

Card* MainWindow::build_prediction_card() {
    auto* card = new Card(QStringLiteral("成分预测"));
    card->set_hint(QStringLiteral("铁矿模型 · 质量分数 %"));
    auto* controls = new QHBoxLayout;
    prediction_branch_ = new QComboBox(card);
    prediction_branch_->setObjectName(QStringLiteral("predictionBranch"));
    prediction_branch_->addItem(QStringLiteral("关闭预测"), QString());
    prediction_branch_->addItem(QStringLiteral("氧化矿"), QStringLiteral("oxide"));
    prediction_branch_->addItem(QStringLiteral("硫化矿"), QStringLiteral("sulfide"));
    prediction_branch_->setToolTip(QStringLiteral("请选择与样品一致的模型分支；预测独立于采集启停。"));
    controls->addWidget(prediction_branch_, 1);
    prediction_retry_ = new QPushButton(QStringLiteral("重试"), card);
    prediction_retry_->setObjectName(QStringLiteral("ghost"));
    prediction_retry_->setToolTip(QStringLiteral("按当前选择重新加载模型，并清空上一条结果。"));
    controls->addWidget(prediction_retry_);
    card->body()->addLayout(controls);
    prediction_import_ = new QPushButton(QStringLiteral("导入 LIBS 光谱预测"), card);
    prediction_import_->setObjectName(QStringLiteral("predictionImport"));
    prediction_import_->setToolTip(QStringLiteral("导入两列数据：波长 nm、原始强度 counts。新模型使用 200–565 nm；真机末端可适配到 564.7 nm，须覆盖全部特征窗口。"));
    card->body()->addWidget(prediction_import_);
    connect(prediction_import_, &QPushButton::clicked, this, &MainWindow::import_prediction_spectrum);
    prediction_status_ = make_hint(QStringLiteral("选择矿石类型后启用实时预测。"));
    prediction_status_->setObjectName(QStringLiteral("predictionStatus"));
    prediction_status_->setWordWrap(true);
    card->body()->addWidget(prediction_status_);
    auto* values = new QGridLayout;
    values->setVerticalSpacing(1);
    const QStringList targets = {QStringLiteral("TFe"), QStringLiteral("SiO₂"), QStringLiteral("Al₂O₃"),
        QStringLiteral("TiO₂"), QStringLiteral("CaO"), QStringLiteral("MgO"), QStringLiteral("MnO"),
        QStringLiteral("P"), QStringLiteral("S"), QStringLiteral("FeO")};
    for (int i = 0; i < targets.size(); ++i) {
        auto* name = new QLabel(targets[i], card);
        name->setObjectName(QStringLiteral("dim"));
        auto* value = new QLabel(QStringLiteral("—"), card);
        value->setObjectName(QStringLiteral("predictionValue"));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet(QStringLiteral("font-size:16px; font-weight:600;"));
        values->addWidget(name, i, 0);
        values->addWidget(value, i, 1);
        prediction_values_.append(value);
    }
    card->body()->addLayout(values);
    prediction_info_ = make_hint(QStringLiteral("研究预测，尚未验证定量精度。P 为元素磷。"));
    prediction_info_->setObjectName(QStringLiteral("predictionInfo"));
    prediction_info_->setWordWrap(true);
    card->body()->addWidget(prediction_info_);
    card->body()->addStretch(1);
    connect(prediction_branch_, &QComboBox::currentIndexChanged, this, [this] { configure_prediction(); });
    connect(prediction_retry_, &QPushButton::clicked, this, &MainWindow::configure_prediction);
    return card;
}

void MainWindow::configure_prediction() {
    if (prediction_configuring_) return;
    prediction_configuring_ = true;
    prediction_branch_->setEnabled(false);
    prediction_retry_->setEnabled(false);
    prediction_import_->setEnabled(false);
    QJsonObject args;
    const QString branch = prediction_branch_->currentData().toString();
    args.insert(QStringLiteral("action"), QStringLiteral("configure"));
    args.insert(QStringLiteral("enabled"), !branch.isEmpty());
    args.insert(QStringLiteral("branch"), branch);
    prediction_status_->setText(QStringLiteral("正在更新预测设置…"));
    for (auto* label : prediction_values_) label->setText(QStringLiteral("—"));
    call_async(QStringLiteral("prediction"), args, 5000,
        [this](const QJsonObject& payload, bool ok, const QString& message) {
            prediction_configuring_ = false;
            prediction_branch_->setEnabled(true);
            prediction_retry_->setEnabled(true);
            prediction_import_->setEnabled(true);
            if (ok) render_prediction(payload);
            else prediction_status_->setText(QStringLiteral("设置失败：%1").arg(message));
            prediction_poller_->request_refresh();
            state_poller_->request_refresh();
        });
}

void MainWindow::import_prediction_spectrum() {
    if (prediction_configuring_) return;
    const QString branch = prediction_branch_->currentData().toString();
    if (branch.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("选择模型"), QStringLiteral("请先选择氧化矿或硫化矿。"));
        return;
    }
    // Screenshot self-check can exercise the same parser and IPC without a modal picker.
    const QString test_path = qEnvironmentVariable("LIBS_UI_SHOT").isEmpty()
        ? QString() : qEnvironmentVariable("LIBS_UI_SHOT_IMPORT");
    const QString path = test_path.isEmpty()
        ? QFileDialog::getOpenFileName(this, QStringLiteral("导入 LIBS 光谱"), QString(),
            QStringLiteral("光谱数据 (*.txt *.csv *.dat);;所有文件 (*)")) : test_path;
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), QStringLiteral("无法读取文件，或文件超过 16 MB。"));
        return;
    }
    const auto lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    QJsonArray wavelengths, counts;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines[i].trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const auto parts = line.split(separators, Qt::SkipEmptyParts);
        bool wave_ok = false, count_ok = false;
        const double wave = parts.value(0).toDouble(&wave_ok), count = parts.value(1).toDouble(&count_ok);
        if (parts.size() != 2 || !wave_ok || !count_ok || !std::isfinite(wave) || !std::isfinite(count) ||
            wave <= 0 || wavelengths.size() >= 100000) {
            QMessageBox::warning(this, QStringLiteral("光谱格式错误"),
                QStringLiteral("第 %1 行无效。请使用两列数字：波长 nm、原始强度 counts；最多 100000 个点。").arg(i + 1));
            return;
        }
        wavelengths.append(wave); counts.append(count);
    }
    if (wavelengths.size() < 3) {
        QMessageBox::warning(this, QStringLiteral("光谱格式错误"), QStringLiteral("至少需要三个光谱数据点。"));
        return;
    }
    QJsonObject channel{{QStringLiteral("serial"), QStringLiteral("imported-spectrum")},
        {QStringLiteral("pixels"), wavelengths.size()}, {QStringLiteral("wavelength_nm"), wavelengths},
        {QStringLiteral("counts"), counts}};
    // Imported files do not assert physical hardware synchronization.
    QJsonObject group{{QStringLiteral("complete"), true}, {QStringLiteral("channels"), QJsonArray{channel}}};
    QJsonObject spectrum{{QStringLiteral("spectrum_source"), QStringLiteral("LIBS")},
        {QStringLiteral("spectrum_format"), QStringLiteral("wavelength_nm+counts")},
        {QStringLiteral("scan_groups"), QJsonArray{group}}};
    QJsonObject input{{QStringLiteral("schema_version"), QStringLiteral("libs.algorithm.v1")},
        {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("branch"), branch}}},
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("source_file"), path}}},
        {QStringLiteral("spectrum"), spectrum}};
    prediction_configuring_ = true;
    prediction_import_->setEnabled(false); prediction_branch_->setEnabled(false); prediction_retry_->setEnabled(false);
    prediction_status_->setText(QStringLiteral("正在提交离线光谱…"));
    for (auto* label : prediction_values_) label->setText(QStringLiteral("—"));
    call_async(QStringLiteral("prediction"), QJsonObject{{QStringLiteral("action"), QStringLiteral("analyze")},
        {QStringLiteral("input"), input}}, 15000,
        [this](const QJsonObject&, bool ok, const QString& message) {
            prediction_configuring_ = false;
            prediction_import_->setEnabled(true); prediction_branch_->setEnabled(true); prediction_retry_->setEnabled(true);
            prediction_status_->setText(ok ? QStringLiteral("已提交，正在预测…") : QStringLiteral("提交失败：%1").arg(message));
            prediction_poller_->request_refresh();
            state_poller_->request_refresh();
        });
}

void MainWindow::render_prediction(const QJsonObject& status) {
    if (prediction_configuring_) return;
    const QSignalBlocker blocker(prediction_branch_);
    const QString branch = status.value("enabled").toBool() ? status.value("branch").toString() : QString();
    const int index = prediction_branch_->findData(branch);
    prediction_branch_->setCurrentIndex(index >= 0 ? index : 0);
    const QString state = status.value("state").toString();
    const auto result = status.value("latest").toObject();
    const auto values = result.value("composition").toObject().value("target_values").toObject();
    const QStringList keys = {QStringLiteral("TFe"), QStringLiteral("SiO2"), QStringLiteral("Al2O3"),
        QStringLiteral("TiO2"), QStringLiteral("CaO"), QStringLiteral("MgO"), QStringLiteral("MnO"),
        QStringLiteral("P"), QStringLiteral("S"), QStringLiteral("FeO")};
    const bool available = result.value("availability").toString() == QLatin1String("available");
    for (int i = 0; i < keys.size(); ++i) {
        const auto value = values.value(keys[i]);
        prediction_values_[i]->setText(available && value.isDouble() ? QString::number(value.toDouble(), 'f', 3) : QStringLiteral("—"));
    }
    QString text;
    if (status.isEmpty()) text = QStringLiteral("尚未读取到预测服务状态。");
    else if (state == QLatin1String("disabled")) text = QStringLiteral("选择矿石类型后启用实时预测。");
    else if (state == QLatin1String("loading")) text = QStringLiteral("正在加载模型…");
    else if (state == QLatin1String("waiting")) text = QStringLiteral("模型已加载，等待测量光谱。");
    else if (state == QLatin1String("queued") || state == QLatin1String("predicting")) {
        text = available ? QStringLiteral("显示帧 %1 结果 · 正在计算下一帧…")
            .arg(result.value("context").toObject().value("frame_index").toDouble(), 0, 'f', 0)
            : QStringLiteral("正在预测…");
    }
    else if (state == QLatin1String("model_unavailable")) text = QStringLiteral("模型加载失败，可检查模型配置后重试。");
    else if (state == QLatin1String("completed")) {
        const auto context = result.value("context").toObject();
        const QString source = result.value("source").toString() == QLatin1String("offline")
            ? QStringLiteral("离线光谱") : QStringLiteral("帧 %1").arg(context.value("frame_index").toDouble(), 0, 'f', 0);
        text = QStringLiteral("%1 · 预测耗时 %2 ms").arg(source).arg(result.value("inference_ms").toDouble(), 0, 'f', 1);
    } else {
        QString reason = result.value("detail").toString();
        const auto rows = result.value("results").toArray();
        if (!rows.isEmpty()) {
            const auto row = rows.first().toObject();
            reason = row.value("detail").toString(row.value("status").toString(reason));
        }
        if (reason.contains(QLatin1String("insufficient_model_wavelength_coverage"))) text = QStringLiteral("波段不足：当前模型至少需要覆盖 200–564.7 nm。");
        else if (reason.contains(QLatin1String("insufficient_200_600_nm_coverage"))) text = QStringLiteral("旧模型波段不足：需要覆盖 200–600 nm。");
        else if (reason.contains(QLatin1String("spectral_gap_exceeds_1_nm"))) text = QStringLiteral("光谱存在大于 1 nm 的缺口，无法预测。");
        else if (reason.contains(QLatin1String("blank_or_low_snr"))) text = QStringLiteral("信号过弱或接近空白，无法预测。");
        else if (reason.contains(QLatin1String("incomplete_scan_group"))) text = QStringLiteral("采集通道不完整，无法预测。");
        else text = QStringLiteral("本次无法预测：%1").arg(reason.isEmpty() ? status.value("detail").toString() : reason);
    }
    prediction_status_->setText(text);
    prediction_status_->setToolTip(status.value("detail").toString() + QStringLiteral("\n") + result.value("request_id").toString());
    QString info = QStringLiteral("研究预测，尚未验证定量精度。P 为元素磷。");
    const auto issues = result.value("quality").toObject().value("issues").toArray();
    QStringList notes;
    for (const auto& value : issues) {
        const auto issue = value.toString();
        if (issue == QLatin1String("outside_observed_domain")) notes << QStringLiteral("超出模型训练域");
        else if (issue == QLatin1String("model_boundary_adapted")) notes << QStringLiteral("末端波段适配至 564.7 nm");
        else if (issue == QLatin1String("unphysical_output")) notes << QStringLiteral("预测值超出物理范围");
        else if (issue == QLatin1String("iron_pool_violation")) notes << QStringLiteral("铁含量约束未满足");
        else if (issue == QLatin1String("hardware_synchronization_not_confirmed")) notes << QStringLiteral("硬件同步未确认");
    }
    if (status.value("skipped_frames").toDouble() > 0) notes << QStringLiteral("预测队列曾跳过部分帧");
    if (result.contains("saved") && !result.value("saved").toBool()) notes << QStringLiteral("预测记录保存失败");
    if (!notes.isEmpty()) info += QStringLiteral("\n") + notes.join(QStringLiteral("；"));
    prediction_info_->setText(info);
    prediction_info_->setToolTip(result.value("result_path").toString());
}

void MainWindow::build_device_bar(QVBoxLayout* root) {
    auto* card = new Card(QStringLiteral("设备状态与采集计数"));
    auto* row = new QHBoxLayout;
    row->setSpacing(28);
    card->body()->addLayout(row);

    // 设备就是三台。**不再给测距仪和空调留位置** ——
    // DeviceSession::health() 已经不账户它们，界面上那两格只会
    // 常年显示"未配置"，让人以为那台机器坏了。
    laser_dot_ = new StatusDot(card);
    laser_dot_->set_state(Level::Idle, QStringLiteral("激光器 · 未知"));
    row->addWidget(laser_dot_);

    indicator_dot_ = new StatusDot(card);
    indicator_dot_->set_state(Level::Idle, QStringLiteral("状态灯 · 未知"));
    row->addWidget(indicator_dot_);

    spectrometer_dot_ = new StatusDot(card);
    spectrometer_dot_->set_state(Level::Idle, QStringLiteral("光谱仪 · 未知"));
    row->addWidget(spectrometer_dot_);

    row->addStretch(1);

    measurement_label_ = make_hint(QStringLiteral("采集计数 · 未知"));
    measurement_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(measurement_label_);

    root->addWidget(card);
}

// ── 渲染 ────────────────────────────────────────────────────────────────

Level MainWindow::level_for_state(const MachineSnapshot& s) {
    // "不知道"绝不能落到 Ok。把未知放在最前面判，是为了防止将来有人
    // 在函数末尾加分支时把默认值排到 Ok 那一侧。
    if (!s.error.isEmpty()) return Level::Danger;
    if (!s.valid) return Level::Idle;
    if (s.gate.known && !s.gate.allowed) return Level::Danger;
    return Level::Ok;
}

void MainWindow::set_banner(const QString& text, Level level) {
    if (!banner_) return;
    // 用 objectName 选样式，而不是逐次 setStyleSheet：后者会把内联样式
    // 钉死在控件上，改主题时它不跟着变（theme.cpp 的纪律）。
    const char* name = "bannerIdle";
    switch (level) {
        case Level::Ok:     name = "bannerOk";     break;
        case Level::Warn:   name = "bannerWarn";   break;
        case Level::Danger: name = "bannerDanger"; break;
        case Level::Idle:   name = "bannerIdle";   break;
    }
    banner_->setObjectName(QString::fromLatin1(name));
    banner_->setText(text);
    // objectName 变了要重新 polish，否则新 QSS 不生效。
    banner_->style()->unpolish(banner_);
    banner_->style()->polish(banner_);
}

void MainWindow::set_banner_default() {
    set_banner(QString::fromUtf8(kDisclaimer), Level::Idle);
}

void MainWindow::render_snapshot(const MachineSnapshot& s) {
    // 顶栏：守护进程连通性。
    if (!s.error.isEmpty()) {
        daemon_dot_->set_state(Level::Danger, QStringLiteral("服务未连接"), s.error);
        set_banner(QStringLiteral("服务未连接，当前显示上一次读取的数据。\n%1")
                       .arg(s.error),
                   Level::Danger);
    } else if (s.valid) {
        daemon_dot_->set_state(Level::Ok, QStringLiteral("服务已连接"));
        set_banner_default();
    } else {
        daemon_dot_->set_state(Level::Idle, QStringLiteral("尚未取到状态"));
    }

    render_gate(s);
    render_devices(s);
    render_measurement(s);
    render_buttons(s);
    // Prediction has its own fast cached-status poller. An older machine
    // snapshot must not overwrite a newer prediction result.

    if (param_page_) param_page_->apply_snapshot(s);
}

void MainWindow::render_gate(const MachineSnapshot& s) {
    const GateState& g = s.gate;
    if (!g.known) {
        gate_dot_->set_state(Level::Idle, QStringLiteral("门禁状态未知"),
                             QStringLiteral("status 未返回 gate，可能是守护进程刚启动"));
        return;
    }
    // detail 优先用后端给的中文说明；没有就把 blocked_by 逐条翻成人话。
    QString why = g.detail;
    if (why.isEmpty() && !g.blocked_by.isEmpty()) {
        QStringList parts;
        for (const QString& code : g.blocked_by) parts << humanize_reason(code);
        why = parts.join(QStringLiteral("；"));
    }
    if (g.allowed) {
        gate_dot_->set_state(Level::Ok,
                             QStringLiteral("门禁允许出光（%1）").arg(humanize_state(g.state)),
                             why);
    } else {
        // 不放行就是"现在点了开始也会被业务层拒绝"。如实说，
        // 但**不把开始按钮禁用掉** —— 按钮可用性只由 laser_running 决定，
        // 那是 render_buttons 的唯一职责，界面不在这里做安全判定。
        gate_dot_->set_state(Level::Danger,
                             QStringLiteral("门禁不放行（%1）").arg(humanize_state(g.state)),
                             why);
    }
}

void MainWindow::render_devices(const MachineSnapshot& s) {
    struct Slot { StatusDot* dot; const char* key; const char* fallback; };
    const Slot entries[] = {
        {laser_dot_,       "laser",       "激光器"},
        {indicator_dot_,   "indicator",   "状态灯"},
        {spectrometer_dot_,"spectrometer","光谱仪"},
    };
    for (const Slot& sl : entries) {
        const QString key = QString::fromLatin1(sl.key);
        const DeviceHealth* d = s.device(key);
        if (d == nullptr) {
            // 守护进程没给这台设备的信息：**未知**，不是"坏了"。
            // fallback 是中文裸字面量，必须按 UTF-8 解码。这条分支只在守护进程
            // 没返回该设备时走（daemon 未启动那一刻），所以"打开后乱码消失"正是
            // 因为那时改走了 d->name。用 fromLatin1 会在这里漏出乱码。
            sl.dot->set_state(Level::Idle, QStringLiteral("%1 · 未知").arg(QString::fromUtf8(sl.fallback)));
            continue;
        }
        if (!d->configured) {
            sl.dot->set_state(Level::Idle, QStringLiteral("%1 · 未配置").arg(d->name),
                              QStringLiteral("未启用此设备"));
        } else if (d->opened) {
            sl.dot->set_state(Level::Ok, QStringLiteral("%1 · 已打开").arg(d->name), d->endpoint);
        } else {
            // configured=true 却没打开 —— 这是**真故障**，不能和"未配置"同色。
            sl.dot->set_state(Level::Danger, QStringLiteral("%1 · 打开失败").arg(d->name),
                              d->note.isEmpty() ? d->endpoint : d->note);
        }
    }
}

void MainWindow::render_measurement(const MachineSnapshot& s) {
    if (!s.measurement_known) {
        measurement_label_->setText(QStringLiteral("采集计数 · 未知"));
        measurement_label_->setToolTip(QStringLiteral(
            "measurement_status 未返回可用计数。守护进程可能还没完成启动，"
            "或者这一轮 status 往返失败了。"));
        return;
    }
    QString text = QStringLiteral("采集 %1 / 失败 %2 · 落盘 %3 / 失败 %4 · 积压 %5")
                       .arg(s.shots_captured).arg(s.shots_failed)
                       .arg(s.disk_writes_ok).arg(s.disk_writes_failed)
                       .arg(s.disk_backlog);
    if (s.measurement_running) text += QStringLiteral(" · 作业运行中");
    measurement_label_->setText(text);

    QString tip = QStringLiteral(
        "落盘计数才是数据存没存盘的凭证。\n"
        "屏幕上有没有曲线只说明内存里有数据。\n"
        "采集 %1 / 失败 %2\n落盘成功 %3 / 落盘失败 %4\n待写积压 %5")
        .arg(s.shots_captured).arg(s.shots_failed)
        .arg(s.disk_writes_ok).arg(s.disk_writes_failed).arg(s.disk_backlog);
    if (!s.measurement_error_code.isEmpty()) {
        tip += QStringLiteral("\n最近一次错误：%1 %2")
                   .arg(s.measurement_error_code, s.measurement_error_message);
    }
    measurement_label_->setToolTip(tip);
}

void MainWindow::render_frame_info(const FrameData& f) {
    // 缺席通道：**列出原因，不补零**。补出来的平线会被读成"信号为 0"。
    QStringList absent;
    for (const ChannelData& c : f.channels) {
        if (!c.present) {
            absent << QStringLiteral("%1（%2）").arg(
                c.serial.isEmpty() ? QStringLiteral("未命名通道") : c.serial,
                c.reason.isEmpty() ? QStringLiteral("无原因") : c.reason);
        }
    }
    absent_label_->setText(absent.isEmpty()
                               ? QString()
                               : QStringLiteral("缺席：%1").arg(absent.join(QStringLiteral("；"))));

    if (!f.ok) {
        frame_label_->setText(QStringLiteral("第 %1 帧 · 采集失败：%2 %3")
                                  .arg(f.frame_index)
                                  .arg(f.capture_error_code, f.capture_error_message));
        return;
    }

    QStringList bits;
    bits << QStringLiteral("第 %1 帧").arg(f.frame_index);
    bits << QStringLiteral("shot %1").arg(f.shot_id);
    if (f.period_ms > 0.0) bits << QStringLiteral("周期 %1 ms").arg(f.period_ms, 0, 'f', 1);
    // 同步事实**原样显示**：请求同步不等于硬件同步了，中间被跳过也看不出来，
    // 所以两句都写出来，不做"善意补全"。
    bits << QStringLiteral("请求同步 %1 / 硬件同步 %2")
                 .arg(f.requested_sync ? QStringLiteral("是") : QStringLiteral("否"),
                      f.hardware_synchronized ? QStringLiteral("是") : QStringLiteral("否"));

    int present = 0;
    for (const ChannelData& c : f.channels) {
        if (c.present) ++present;
    }
    bits << QStringLiteral("通道 %1/%2").arg(present).arg(f.channels.size());
    if (stream_dropped_ > 0) bits << QStringLiteral("界面丢帧 %1").arg(stream_dropped_);
    if (stream_pending_ > 1) bits << QStringLiteral("队列积压 %1").arg(stream_pending_);

    frame_label_->setText(bits.join(QStringLiteral(" · ")));
}

void MainWindow::render_buttons(const MachineSnapshot& s) {
    // 按钮可用性**只有这一个来源**：后端 status。
    //
    //   设备预检   已连接
    //   开始检测   已连接 且 未在出光
    //   停止检测   已连接 且 正在出光
    //
    // 特意**不**把 gate.allowed 掺进来：那是安全判定，判定权在
    // MeasurementJob::start()。界面一预先拦，界面上那颗绿灯/红灯就
    // 变成了第二个判定源，而后端加规则时界面不会跟着变。
    const bool connected = s.valid && s.error.isEmpty();

    if (precheck_btn_) {
        precheck_btn_->setEnabled(connected && !command_in_flight_);
    }
    if (start_btn_) {
        start_btn_->setEnabled(connected && !command_in_flight_ && !s.laser_running);
        start_btn_->setToolTip(s.laser_running
                                   ? QStringLiteral("激光正在出光。先点「停止检测」再重新开始。")
                                   : QStringLiteral(
                                         "启动激光触发光谱采集，持续显示并保存数据。\n"
                                         ""
                                         "条件不满足时业务层一条写指令都不会发。"));
    }
    if (stop_btn_) {
        stop_btn_->setEnabled(connected && !command_in_flight_ && s.laser_running);
        stop_btn_->setToolTip(s.laser_running
                                  ? QStringLiteral(
                                        "停止测量：把剩余帧写完再返回，然后确认激光已关断。\n"
                                        "停止是安全方向的动作，不做二次确认。")
                                  : QStringLiteral("当前未在出光，此按钮无事可做。"));
    }
}

// ── 光谱 ────────────────────────────────────────────────────────────────

void MainWindow::start_stream() {
    // 订阅的是**显示**通道，不是采集。采集的唯一来源是测量作业。
    if (!poller_) return;
    if (poller_->open()) {
        set_banner_default();
        return;
    }
    // open() 失败时 stream_error 已经写过横幅了，这里不重复弹窗。
}

void MainWindow::stop_stream() {
    if (poller_) poller_->close();
}

void MainWindow::rebuild_series(const FrameData& f) {
    QVector<SpectrumPlot::Segment> segments;
    segments.reserve(f.channels.size());
    int index = 0;
    for (const ChannelData& c : f.channels) {
        if (!c.present) {
            // **缺席不画线。** 画一条 0 线等于谎报"这一路接上了、信号是 0"。
            ++index;
            continue;
        }
        SpectrumPlot::Segment seg;
        seg.name = c.serial.isEmpty() ? QStringLiteral("通道 %1").arg(index + 1) : c.serial;
        seg.color = theme::color::series(index);
        seg.points.reserve(c.wavelength_nm.size());
        const int n = qMin(c.wavelength_nm.size(), c.counts.size());
        for (int i = 0; i < n; ++i) {
            seg.points.append(QPointF(c.wavelength_nm.at(i), c.counts.at(i)));
        }
        segments.push_back(std::move(seg));
        ++index;
    }

    // 归一化刻意**不做**：图上画原始计数，口径一改只改 UI。
    plot_->set_segments(std::move(segments));
    plot_->set_auto_y(auto_y_on_);
    plot_->set_lock_y(lock_y_on_);
    if (lock_y_on_) plot_->set_y_max(locked_y_hi_);
}

void MainWindow::on_frame(const FrameData& f) {
    ++frame_seen_;
    render_frame_info(f);

    if (!f.ok) {
        // 保留上一帧曲线，但**置灰 + 红色横幅**写清错误码。
        // 直接清屏会让人以为"检测停了"，静默停在旧数据上会让人
        // 以为是新数据 —— 两者都比置灰更坏。
        plot_->set_dimmed(true);
        set_banner(QStringLiteral("采集失败（第 %1 帧）：%2 %3\n图上保留的是上一帧数据，不是新数据。")
                       .arg(f.frame_index)
                       .arg(f.capture_error_code, f.capture_error_message),
                   Level::Danger);
        return;
    }

    if (paused_) {
        // 暂停只停**画面**，采集和落盘照旧。等价于"我先看别的"。
        frame_label_->setText(frame_label_->text() + QStringLiteral(" · 已暂停刷新"));
        return;
    }

    plot_->set_dimmed(false);
    if (!f.has_any_present_channel()) {
        set_banner(QStringLiteral("第 %1 帧所有通道都缺席，没有任何可画的谱。").arg(f.frame_index),
                   Level::Warn);
    } else {
        set_banner_default();
    }
    rebuild_series(f);

    // CSV 记的是**原始计数**，和图上的一致，且与显示口径解耦：
    // 以后改归一化规则，历史导出的数还是原始数。
    if (!csv_exported_) {
        for (const ChannelData& c : f.channels) {
            if (!c.present) continue;
            const int n = qMin(c.wavelength_nm.size(), c.counts.size());
            for (int i = 0; i < n; ++i) {
                csv_lines_ << QStringLiteral("%1,%2,%3,%4,%5,%6")
                                  .arg(f.frame_index).arg(f.shot_id).arg(f.wall_clock_ms)
                                  .arg(c.serial).arg(c.wavelength_nm.at(i), 0, 'f', 4)
                                  .arg(c.counts.at(i), 0, 'f', 0);
            }
        }
    }
}

void MainWindow::on_capture_failed(const QString& code, const QString& message) {
    plot_->set_dimmed(true);
    set_banner(QStringLiteral("采集失败：%1 %2").arg(code, message), Level::Danger);
}

void MainWindow::on_stream_error(const QString& message) {
    // 传输层出错（守护进程没起、订阅掉线）。**不弹窗**：轮询会自己重试，
    // 一次抖动弹一次窗比不弹更让人麻木。
    set_banner(QStringLiteral("光谱通道异常：%1\n画面可能已停止更新，采集本身不受影响。")
                   .arg(message),
               Level::Danger);
}

void MainWindow::on_stats(int pending, qint64 dropped) {
    stream_pending_ = pending;
    stream_dropped_ = dropped;
}

// ── 状态轮询 ────────────────────────────────────────────────────────────

void MainWindow::on_snapshot() {
    if (!state_poller_) return;
    render_snapshot(state_poller_->snapshot());
}

void MainWindow::tick_clock() {
    if (clock_) clock_->setText(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

// ── 三个按钮 ────────────────────────────────────────────────────────────

void MainWindow::call_async(const QString& command, const QJsonObject& args, int timeout_ms,
                            std::function<void(const QJsonObject&, bool, const QString&)> done) {
    // 工作线程里**只**做管道往返 + JSON 解析，一个 QWidget 都不碰。
    // 结果用 queued invokeMethod 送回 UI 线程再回调。
    //
    // 为什么不直接在 UI 线程里同步发：status/precheck/param_set 都要过
    // 守护进程的 device_mutex，一次往返的耗时**取决于现场设备有多慢**。
    // 锁着界面等串口是工控现场最烦人的一种卡顿。
    //
    // QPointer 兜底：窗口在命令飞行途中被关掉时，这里必须安静地放弃回调，
    // 而不是对一个已析构的 MainWindow 调槽函数。
    QPointer<MainWindow> self(this);
    PipeClient client = client_;

    QThread* worker = QThread::create([client, command, args, timeout_ms, done, self]() mutable {
        QJsonObject payload;
        bool ok = false;
        QString message;
        try {
            payload = client.command_payload(command, args, timeout_ms);
            ok = true;
        } catch (const QString& e) {
            // PipeClient 用 QString 抛业务错误。这里转成 ok=false + 消息，
            // 不让它越过线程边界 —— std::exception 穿线程是未定义行为。
            message = e;
        }
        if (!self) return;
        QMetaObject::invokeMethod(self.data(),
                                  [done, payload, ok, message]() { done(payload, ok, message); },
                                  Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::on_precheck() {
    if (command_in_flight_) return;
    command_in_flight_ = true;
    if (state_poller_) render_buttons(state_poller_->snapshot());
    set_banner(QStringLiteral("正在执行设备预检……（只读检测，不会出光）"), Level::Warn);

    call_async(QStringLiteral("precheck"), QJsonObject(), 20000,
               [this](const QJsonObject& payload, bool ok, const QString& message) {
                   on_precheck_result(payload, ok, message);
               });
}

void MainWindow::on_precheck_result(const QJsonObject& payload, bool ok, const QString& message) {
    command_in_flight_ = false;

    // 失败时报告挂在 payload.report 下（后端 failure_with_payload 的约定），
    // 成功时报告就是 payload 本身。两种形态都要读得到结论。
    QJsonObject report = payload.value(QStringLiteral("report")).toObject();
    if (report.isEmpty()) report = payload;

    QStringList reasons;
    const QJsonArray blocked = report.value(QStringLiteral("blocked_by")).toArray();
    for (const QJsonValue& v : blocked) reasons << humanize_reason(v.toString());
    const QString detail = report.value(QStringLiteral("detail")).toString();
    const bool laser_readable = report.value(QStringLiteral("laser_readable")).toBool(true);
    const QString state = humanize_state(report.value(QStringLiteral("state")).toString());

    if (ok) {
        QString text = QStringLiteral("设备预检通过（%1）。%2").arg(state, detail);
        if (!reasons.isEmpty()) text += QStringLiteral("\n提示：%1").arg(reasons.join(QStringLiteral("；")));
        set_banner(text, Level::Ok);
    } else {
        QString text = QStringLiteral("设备预检未通过：%1").arg(message);
        if (!reasons.isEmpty()) text += QStringLiteral("\n不满足：%1").arg(reasons.join(QStringLiteral("；")));
        else if (!detail.isEmpty()) text += QStringLiteral("\n%1").arg(detail);
        if (!laser_readable) {
            text += QStringLiteral("\n另外：没读到激光器状态，已按不安全处理。");
        }
        set_banner(text, Level::Danger);
    }

    // 预检会刷新 monitor 的缓存结论和状态灯，所以立刻拉一次新状态。
    if (state_poller_) state_poller_->request_refresh();
}

void MainWindow::on_start_detection() {
    if (command_in_flight_) return;
    command_in_flight_ = true;
    if (state_poller_) render_buttons(state_poller_->snapshot());
    set_banner(QStringLiteral("正在下发 measurement_start……（成功即已开光）"), Level::Warn);

    // 超时给足：这条命令要写参数、工作使能、等放电确认、开光闸。
    call_async(QStringLiteral("measurement_start"), QJsonObject(), 90000,
               [this](const QJsonObject& payload, bool ok, const QString& message) {
                   on_start_result(payload, ok, message);
               });
}

void MainWindow::on_start_result(const QJsonObject& payload, bool ok, const QString& message) {
    command_in_flight_ = false;

    if (ok) {
        // 注意这里用的是 Danger 色：此刻**激光真的在出光**。
        // 用绿色显示"开始成功"会让人放松，而放松的代价是眼睛。
        const QString note = payload.value(QStringLiteral("note")).toString();
        set_banner(QStringLiteral("激光已开启，正在持续出光。\n%1\n离开前请点「停止检测」并确认光闸已关闭。")
                       .arg(note),
                   Level::Danger);
    } else {
        // 失败原因**原样上送**，不转述成"设备故障"这类模糊话。
        // 业务层已经写了具体的 error code 与 message，界面上再加工一遍
        // 就等于替现场做了一次翻译，翻译错了没人查得到。
        set_banner(QStringLiteral("开始检测被业务层拒绝：%1\n业务层在拒绝时会保证一条写指令都没发。")
                       .arg(message),
                   Level::Warn);
    }

    if (state_poller_) state_poller_->request_refresh();
}

void MainWindow::on_stop_detection() {
    if (command_in_flight_) return;
    command_in_flight_ = true;
    if (state_poller_) render_buttons(state_poller_->snapshot());
    set_banner(QStringLiteral("正在停止测量（把剩余帧写完再返回）……"), Level::Warn);

    // 不做二次确认：停止是往安全方向走的动作，让操作员点两次才能收光，
    // 那是在**增加**而不是降低风险。
    //
    // 超时给足 60s：stop() 会 join 采集线程，而那个线程可能正卡在
    // capture 上等 TTL。
    call_async(QStringLiteral("measurement_stop"), QJsonObject(), 60000,
               [this](const QJsonObject&, bool ok, const QString& message) {
                   on_stop_result(ok, message);
               });
}

void MainWindow::on_stop_result(bool ok, const QString& message) {
    command_in_flight_ = false;
    if (ok) {
        set_banner(QStringLiteral("测量已停止。%1\n请现场确认光闸已关闭。").arg(message), Level::Ok);
    } else {
        // 停止没确认通过是**必须人工看一眼**的情况，用 Danger 而不是 Warn。
        set_banner(QStringLiteral("停止未确认：%1\n请现场人工确认光闸确实已关闭。").arg(message),
                   Level::Danger);
    }
    if (state_poller_) state_poller_->request_refresh();
}

// ── 参数页 ──────────────────────────────────────────────────────────────

void MainWindow::on_param_save_requested(const QJsonObject& request) {
    call_async(QStringLiteral("param_set"), request, 20000,
               [this](const QJsonObject& payload, bool transport_ok, const QString& message) {
                   if (!transport_ok) {
                       on_param_result(false, QStringLiteral("保存失败（管道/命令层）：%1").arg(message));
                       return;
                   }
                   // **必须检查 completed。** 后端 note 的原话就是
                   // 「调用方必须检查它」—— completed=false 是
                   // "收到请求但未完成"，当成功处理等于骗操作员。
                   const bool completed = payload.value(QStringLiteral("completed")).toBool(false);
                   const QString msg = payload.value(QStringLiteral("message")).toString();
                   const QString group = payload.value(QStringLiteral("group")).toString();

                   QStringList failed;
                   const QJsonArray bad =
                       payload.value(QStringLiteral("failed_fields")).toArray();
                   for (const QJsonValue& v : bad) failed << v.toString();

                   if (!completed) {
                       on_param_result(false,
                                       QStringLiteral("【%1】未完成：%2\n被拒绝的字段：%3")
                                           .arg(group, msg,
                                                failed.isEmpty() ? QStringLiteral("（后端未列出）")
                                                                 : failed.join(QStringLiteral("、"))));
                       return;
                   }
                   // 成功也不许说"现在可以出光了" —— 那是 UI 自己做安全判定。
                   on_param_result(true,
                                   QStringLiteral("【%1】已保存：%2\n光谱参数用于后续采集，激光会话参数在下次启动测量时使用。")
                                       .arg(group, msg));
               });
}

void MainWindow::on_param_reload_requested() {
    call_async(QStringLiteral("param_get"), QJsonObject(), 15000,
               [this](const QJsonObject& payload, bool ok, const QString& message) {
                   if (!ok) {
                       on_param_result(false, QStringLiteral("重新读取失败：%1").arg(message));
                       return;
                   }
                   const QJsonObject groups = payload.value(QStringLiteral("groups")).toObject();
                   // force=true：「放弃修改」的语义就是**明知有改动也覆盖回去**。
                   if (param_page_) param_page_->apply_values(groups, true);
                   on_param_result(true, QStringLiteral("已从守护进程重新读取参数，未保存的改动已丢弃。"));
               });
}

void MainWindow::on_param_result(bool ok, const QString& summary) {
    if (param_page_) param_page_->on_save_result(ok, summary);
    if (state_poller_) state_poller_->request_refresh();
}

void MainWindow::show_main_page() {
    if (pages_) pages_->setCurrentIndex(0);
    if (param_page_btn_) param_page_btn_->setChecked(false);
    if (main_page_btn_) main_page_btn_->setChecked(true);
}

void MainWindow::show_parameter_page() {
    if (pages_) pages_->setCurrentIndex(1);
    if (param_page_btn_) param_page_btn_->setChecked(true);
    if (main_page_btn_) main_page_btn_->setChecked(false);
    // 切进参数页立刻拉一次状态：新参数齐备指示要马上反映出来。
    if (state_poller_) state_poller_->request_refresh();
}

// ── 光谱小工具 ──────────────────────────────────────────────────────────

void MainWindow::on_auto_y_toggled(bool on) {
    auto_y_on_ = on;
    plot_->set_auto_y(on);
    if (!on && lock_y_on_) {
        // 关掉自动的同时如果纵轴是锁的，就先把当前上限钉住，
        // 否则下一步会突然跳到一个没有参照物的量程。
        locked_y_hi_ = plot_->y_max();
        plot_->set_lock_y(true);
    }
}

void MainWindow::on_lock_y_toggled(bool on) {
    lock_y_on_ = on;
    if (on) locked_y_hi_ = plot_->y_max();
    plot_->set_lock_y(on);
    if (on) plot_->set_y_max(locked_y_hi_);
}

void MainWindow::on_pause_toggled(bool on) {
    paused_ = on;
    pause_->setToolTip(on ? QStringLiteral("画面已暂停刷新。采集与落盘仍在继续。")
                          : QStringLiteral("只暂停画面刷新，不影响采集，也不停止落盘。"));
}

void MainWindow::on_export_csv() {
    if (csv_lines_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出 CSV"),
                                 QStringLiteral("本次会话还没有收到任何可导出的帧。\n"
                                                "图上出现曲线之后再导出。"));
        return;
    }
    const QString suggested =
        QDir::temp().filePath(QStringLiteral("libs-spectra-%1.csv")
                                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出本次已见帧"), suggested,
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入 %1：\n%2").arg(path, file.errorString()));
        return;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "# frame_index,shot_id,wall_clock_ms,channel,wavelength_nm,counts\n";
    for (const QString& line : csv_lines_) out << line << '\n';
    file.close();

    // 导出后停止继续追加：再点一次会写出一份只到上次导出为止的文件，
    // 而文件名看不出这个差别，容易被当成"就这些数据"。
    csv_exported_ = true;
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("已导出 %1 行原始计数到：\n%2\n\n"
                                            "说明：这是界面收到的帧，正式数据以守护进程落盘为准。")
                                 .arg(csv_lines_.size()).arg(path));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // 退订必须在子对象析构前做：否则守护进程那边留一个订阅号，
    // 它的队列没人取，每一帧都被丢一次。
    stop_stream();
    if (state_poller_) state_poller_->stop();
    if (prediction_poller_) prediction_poller_->stop();
    event->accept();
}

}  // namespace libs::ui

