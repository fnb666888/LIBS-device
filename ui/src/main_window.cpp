#include "main_window.hpp"
#include "debug_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QLineSeries>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QStackedLayout>
#include <QStringConverter>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QtCharts/QAbstractAxis>
#include <QtCharts/QLegend>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <limits>

#include "spectrum_plot.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace libs::ui {
namespace {

// 自由触发预览的**永久**免责声明。放在最显眼处而不是"关于"对话框里：
// 看到一条漂亮的光谱曲线而不知道它没出光，是这个界面最大的误读风险。
const char* kDisclaimer =
    "⚠ 实时预览：自由触发、未出光。这里显示的是背景/杂散光谱，"
    "不是 LIBS 测量谱（真实谱需 laser_fire 走安全门禁后出光采集）。";

// 参数面板的一个字段描述。
//
// lo/hi 取自协议原文；**def 是本机实测标定值**，不是设计文档里的占位数字，
// 也不是量程中点。来源：2026-09-29 用厂家工具跑通、并经本项目 laser_fire
// 真机复现的那一套（厂家 Log_20260922：80 A / 200 μs / 210 μs Q 延时）。
struct FieldSpec {
    const char* key;      // param_set 的字段名（业务参数库）
    const char* over;     // laser_fire 覆盖参数名；空 = 本字段不参与出光
    const char* label;
    double lo;
    double hi;
    double def;
    int decimals;
    const char* unit;
    const char* tooltip;
};

// 只保留**每次检测可能需要按样品调**的会话参数。
//
// 刻意不放 0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护：那是**整机标定值**，
// 存在设备 EEPROM 里被所有工具共享，本程序只读不写。把它们摆到界面上，
// 现场迟早会有人去调 —— 而调坏那一个数，整台设备就不出光了（真发生过）。
const FieldSpec kLaserFields[] = {
    {"discharge_current", "discharge_current_a", "放电电流", 15.0, 100.0, 80.0, 1, "A",
     "协议 0x11：15~100 A。本机标定 80 A。超过 30 A 属高功率区，提交前二次确认。"},
    {"discharge_pulse_width", "pulse_width_us", "放电脉宽", 50.0, 300.0, 200.0, 0, "μs",
     "协议 0x12：50~300 μs。本机标定 200 μs。"},
    {"discharge_frequency", "frequency_hz", "放电频率", 1.0, 10.0, 1.0, 0, "Hz",
     "协议 0x13：1~10 Hz。本机标定 1 Hz。"},
    {"discharge_delay", "discharge_delay_us", "放电延时", 0.0, 500.0, 0.0, 0, "μs",
     "协议 0x14：0~500 μs。本机标定 0 μs。"},
    {"q_delay", "q_delay_us", "Q 延时", 2.0, 500.0, 210.0, 0, "μs",
     "协议 0x16：2~500 μs。本机标定 210 μs —— Q 开关靠这个值决定巨脉冲，"
     "取量程下限(2 μs)等于不调 Q、不出光。"},
};

// 光谱仪只留两个真正需要按样品调的。
//
// 刻意不放 integration_delay_cycles / laser_delay_cycles / laser_width_cycles：
// 单位是 FPGA 计数而不是 μs/ns，属于驱动内部换算；后两个只在反向模式由
// 后端根据激光脉宽生成，避免现场直接编辑原始计数造成错误脉冲。
const FieldSpec kSpecFields[] = {
    {"integration_ms", "integration_ms", "积分时间", 0.002, 60000.0, 10.0, 3, "ms",
     "驱动硬校验 0.002~60000 ms。超范围会被业务层直接拒绝。"},
    {"timeout_ms", "capture_timeout_ms", "采集超时", 1.0, 600000.0, 10000.0, 0, "ms",
     "触发等待与采集收尾的超时；本机配置为 10 s。"},
};

// 取某个参数分组对应的 FieldSpec 表。UI 不在这里"发明"范围：每一个
// lo/hi 都对着协议或驱动的硬校验抄来的，抄错就会在提交时被后端拒绝。
const FieldSpec* specs_for(const QString& group, std::size_t& count) {
    if (group == QLatin1String("laser")) {
        count = sizeof(kLaserFields) / sizeof(FieldSpec);
        return kLaserFields;
    }
    count = sizeof(kSpecFields) / sizeof(FieldSpec);
    return kSpecFields;
}

// 门禁拦截原因码 -> 人话。
//
// 后端 GateReason 枚举给的是英文 snake_case（parameters_incomplete、
// laser_unavailable……）。直接把它显示给现场操作员，等于让人去读 C++ 枚举名；
// 现场需要的是"激光器没接上"这种一眼能懂的话。码表抄自
// include/libs/business/policy/gate.hpp 的 GateReason，**不认识的码原样透传**——
// 宁可显示一个后端新加的英文码，也不要静默吞掉一个自己猜错的原因。
QString humanize_gate_reason(const QString& code) {
    static const QHash<QString, QString> kNames = {
        {QStringLiteral("ready"),                  QStringLiteral("已就绪")},
        {QStringLiteral("parameters_incomplete"),  QStringLiteral("核心参数未齐备")},
        {QStringLiteral("laser_unavailable"),      QStringLiteral("读不到激光器状态")},
        {QStringLiteral("interlock_open"),         QStringLiteral("联锁断开")},
        {QStringLiteral("emergency_stopped"),      QStringLiteral("急停已触发")},
        {QStringLiteral("q_gate_open"),            QStringLiteral("光闸处于打开位置")},
        {QStringLiteral("laser_fault"),            QStringLiteral("激光器报故障")},
        {QStringLiteral("over_temperature"),       QStringLiteral("激光器超温")},
        {QStringLiteral("not_enabled"),            QStringLiteral("设备未使能工作")},
        {QStringLiteral("air_conditioner_interlock"), QStringLiteral("空调联锁未通过")},
    };
    const auto it = kNames.constFind(code);
    return it == kNames.constEnd() ? code : it.value();
}

QString humanize_gate_state(const QString& s) {
    if (s == QLatin1String("standby")) return QStringLiteral("待机");
    if (s == QLatin1String("ready"))    return QStringLiteral("就绪");
    if (s == QLatin1String("armed"))    return QStringLiteral("已布防");
    if (s == QLatin1String("acquiring"))return QStringLiteral("采集中");
    if (s == QLatin1String("fault"))    return QStringLiteral("故障");
    if (s.isEmpty())                    return QStringLiteral("未知");
    return s;
}

QString fmt_fixed(double v, int decimals) {
    return QString::number(v, 'f', decimals);
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("LIBS 固废筛查仪 · 实时光谱与控制"));
    resize(1600, 1000);
    setMinimumSize(1180, 720);

    poller_ = new StreamPoller(client_, this);
    connect(poller_, &StreamPoller::frame_ready, this, &MainWindow::on_frame);
    connect(poller_, &StreamPoller::capture_failed, this, &MainWindow::on_capture_failed);
    connect(poller_, &StreamPoller::stream_error, this, &MainWindow::on_stream_error);
    connect(poller_, &StreamPoller::stats_changed, this, &MainWindow::on_stats);

    state_poller_ = new StatusPoller(client_, this);
    connect(state_poller_, &StatusPoller::snapshot_ready, this, &MainWindow::on_snapshot);
    state_poller_->start(800);

    build_chart();

    central_ = new QWidget(this);
    auto* root = new QVBoxLayout(central_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    build_header(central_);

    // 免责声明紧贴顶栏：它是常驻提示，不是页脚的小字。
    banner_ = new QLabel(QString(), central_);
    banner_->setWordWrap(true);
    set_banner(preview_provenance_text(), 1);
    auto* banner_wrap = new QWidget(central_);
    auto* bw = new QVBoxLayout(banner_wrap);
    bw->setContentsMargins(12, 8, 12, 0);
    bw->addWidget(banner_);
    root->addWidget(banner_wrap);

    // 主体用 QStackedLayout 而不是叠控件：离线时切到第 1 页（遮罩页），
    // 它会真的**接管**所有鼠标事件——半透明控件做不到"禁用操作"这件事，
    // 一旦设备离线，遮罩下面那些按钮还点得动。
    auto* stack_host = new QWidget(central_);
    auto* stack = new QStackedLayout(stack_host);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(0);

    auto* split = new QSplitter(Qt::Horizontal, stack_host);
    split->setObjectName(QStringLiteral("rootSplit"));
    split->setChildrenCollapsible(false);
    split->setHandleWidth(8);
    split->addWidget(build_left_column());
    split->addWidget(build_right_column());
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({420, 1000});
    stack->addWidget(split);

    overlay_ = new QWidget(stack_host);
    overlay_->setObjectName(QStringLiteral("offlineOverlay"));
    auto* ov = new QVBoxLayout(overlay_);
    ov->setAlignment(Qt::AlignCenter);
    overlay_label_ = new QLabel(overlay_);
    overlay_label_->setObjectName(QStringLiteral("offlineOverlayText"));
    overlay_label_->setAlignment(Qt::AlignCenter);
    ov->addWidget(overlay_label_);
    stack->addWidget(overlay_);
    stack->setCurrentWidget(split);
    stack_ = stack;

    root->addWidget(stack_host, 1);
    root->addWidget(build_bottom_bar(central_));

    setCentralWidget(central_);
    statusBar()->hide();

    auto* clock = new QTimer(this);
    connect(clock, &QTimer::timeout, this, &MainWindow::tick_clock);
    clock->start(1000);
    tick_clock();

    log_line(Level::Idle, QStringLiteral("界面已启动，正在连接守护进程……"));
    update_offline_overlay();
}

MainWindow::~MainWindow() = default;

// ── 顶栏 ──────────────────────────────────────────────────────────────

void MainWindow::build_header(QWidget* host) {
    auto* bar = new QWidget(host);
    bar->setObjectName(QStringLiteral("headerBar"));
    bar->setFixedHeight(56);

    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(18, 8, 18, 8);
    row->setSpacing(14);

    title_ = new QLabel(QStringLiteral("LIBS 固废筛查仪"), bar);
    title_->setObjectName(QStringLiteral("appTitle"));
    subtitle_ = new QLabel(QStringLiteral("控制系统 · 纯管道客户端"), bar);
    subtitle_->setObjectName(QStringLiteral("appSubtitle"));
    row->addWidget(title_);
    row->addWidget(subtitle_);
    row->addSpacing(10);

    auto* dot_col = new QVBoxLayout;
    dot_col->setSpacing(0);
    daemon_dot_ = new StatusDot(bar);
    daemon_dot_->set_state(Level::Idle, QStringLiteral("守护进程 未连接"),
                           client_.pipe_name());
    dot_col->addWidget(daemon_dot_);
    row->addLayout(dot_col);

    row->addStretch(1);

    gate_dot_ = new StatusDot(bar);
    gate_dot_->set_state(Level::Idle, QStringLiteral("门禁 未知"), QString());
    row->addWidget(gate_dot_);

    gate_pill_ = new QLabel(QStringLiteral("GATE —"), bar);
    gate_pill_->setObjectName(QStringLiteral("gatePill"));
    gate_pill_->setAlignment(Qt::AlignCenter);
    row->addWidget(gate_pill_);

    auto* debug_button = new QPushButton(QStringLiteral("调试台"), bar);
    debug_button->setToolTip(QStringLiteral("打开手动设备与接口调试台；不会启动自动联动编排"));
    connect(debug_button, &QPushButton::clicked, this, [this] {
        if (!debug_window_) debug_window_ = new DebugWindow(this);
        debug_window_->show();
        debug_window_->raise();
        debug_window_->activateWindow();
    });
    row->addWidget(debug_button);

    clock_ = new QLabel(bar);
    clock_->setObjectName(QStringLiteral("appClock"));
    row->addWidget(clock_);

    (host->layout())->addWidget(bar);
}


// ── 左列：控制 + 参数 ─────────────────────────────────────────────────

QWidget* MainWindow::build_left_column() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(380);

    auto* panel = new QWidget;
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(12, 12, 8, 12);
    col->setSpacing(12);

    // ── 检测控制 ────────────────────────────────────────────────────
    auto* ctrl = new Card(QStringLiteral("检测控制"), panel);
    run_dot_ = new StatusDot(ctrl);
    ctrl->add_header_widget(run_dot_);
    run_dot_->set_state(Level::Idle, QStringLiteral("待机"));

    auto* btn_row = new QHBoxLayout;
    btn_row->setSpacing(10);
    start_btn_ = new QPushButton(QStringLiteral("启动检测"), ctrl);
    start_btn_->setObjectName(QStringLiteral("startBig"));
    start_btn_->setMinimumHeight(62);
    start_btn_->setToolTip(QStringLiteral(
        "启动**实时光谱预览**（自由触发、不出光）：看到的是背景/杂散光谱。\n"
        "同时向业务层发一个 detection_start 业务信号 —— 那个信号目前没有下游"
        "（检测编排还没做），所以这个按钮的实际作用就是开预览。\n"
        "要看 LIBS 测量谱请用「出光测量」，要看边打边看请用「连续出光」。"));
    stop_btn_ = new QPushButton(QStringLiteral("停止"), ctrl);
    stop_btn_->setObjectName(QStringLiteral("stopBig"));
    stop_btn_->setMinimumHeight(62);
    stop_btn_->setEnabled(false);
    stop_btn_->setToolTip(QStringLiteral(
        "停止检测。UI 不判定能否停止，指令由业务层执行并回报。"));
    btn_row->addWidget(start_btn_, 3);
    btn_row->addWidget(stop_btn_, 1);
    ctrl->body()->addLayout(btn_row);
    connect(start_btn_, &QPushButton::clicked, this, &MainWindow::on_start_detection);
    connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::on_stop_detection);

    run_state_label_ = new QLabel(QStringLiteral("状态：—"), ctrl);
    run_state_label_->setObjectName(QStringLiteral("dim"));
    stage_label_ = new QLabel(QStringLiteral("阶段：—"), ctrl);
    stage_label_->setObjectName(QStringLiteral("dim"));
    belt_label_ = new QLabel(QStringLiteral("目标带：—"), ctrl);
    belt_label_->setObjectName(QStringLiteral("dim"));
    seq_label_ = new QLabel(QStringLiteral("带序号：—"), ctrl);
    seq_label_->setObjectName(QStringLiteral("dim"));
    count_label_ = new QLabel(QStringLiteral("日志条数：—"), ctrl);
    count_label_->setObjectName(QStringLiteral("dim"));
    for (QLabel* l : {run_state_label_, stage_label_, belt_label_, seq_label_, count_label_}) {
        ctrl->body()->addWidget(l);
    }
    col->addWidget(ctrl);

    // ── LIBS 测量（真出光）──────────────────────────────────────────
    //
    // 与"启动检测"分开是有意的：启动检测只发业务信号，**这个按钮会让激光
    // 真的出光**。两者共用一个按钮，现场迟早会有人以为点的是"预览"。
    //
    // 出光按钮的可用性只由 gate.allowed 决定 —— UI 不自己判安全，
    // 门禁说不许就是不许，按钮直接灰掉，门禁药丸上写着为什么。
    auto* measure = new Card(QStringLiteral("LIBS 测量（真出光）"), panel);
    measure->set_hint(QStringLiteral(
        "走 laser_fire：门禁放行 → 写会话参数 → 工作使能 → 等设备进入放电 → "
        "开光闸连续出光 → 收光停机 → 四通道同步采集 → 落盘。\n"
        "**先点「预检」布防**：门禁放行后「出光测量」才会亮。"
        "门禁不放行时，业务层一条写指令都不会发。"));

    // 触发方向是物理接线的一部分，必须在真正出光的卡片里显式显示。
    // 默认值保持现场当前已经验证的方向：激光器 TTL → 光谱仪 Pin 6。
    // 选择反向时只改变 laser_fire 的意图参数，最终是否允许由后端根据
    // 已确认的激光器输入接口和配置决定；UI 不自行宣称硬件已经联通。
    auto* trigger_row = new QHBoxLayout;
    trigger_row->setSpacing(10);
    auto* trigger_label = new QLabel(QStringLiteral("联合触发方向"), measure);
    trigger_label->setObjectName(QStringLiteral("dim"));
    trigger_direction_ = new QComboBox(measure);
    trigger_direction_->addItem(QStringLiteral("激光器触发光谱仪（当前接线）"),
                                 QStringLiteral("laser_to_spectrometer"));
    trigger_direction_->addItem(QStringLiteral("光谱仪触发激光器（反向）"),
                                 QStringLiteral("spectrometer_to_laser"));
    trigger_direction_->setCurrentIndex(0);
    trigger_direction_->setToolTip(QStringLiteral(
        "当前接线：激光器 Q OUT → Avantes 主通道 Pin 6。\n"
        "反向接线：Avantes 主通道 Pin 23 LASER OUT → 激光器 CLK IN；"
        "RS-232/COM10 仍只负责参数与状态通信。\n"
        "切换后要重新预检，不能在连续出光期间切换。"));
    trigger_row->addWidget(trigger_label);
    trigger_row->addWidget(trigger_direction_, 1);
    measure->body()->addLayout(trigger_row);

    trigger_path_label_ = new QLabel(QString(), measure);
    trigger_path_label_->setObjectName(QStringLiteral("dim"));
    trigger_path_label_->setWordWrap(true);
    measure->body()->addWidget(trigger_path_label_);
    on_trigger_direction_changed(0);
    connect(trigger_direction_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::on_trigger_direction_changed);

    auto* mrow = new QHBoxLayout;
    mrow->setSpacing(10);
    precheck_btn_ = new QPushButton(QStringLiteral("预检"), measure);
    precheck_btn_->setMinimumHeight(38);
    precheck_btn_->setToolTip(QStringLiteral(
        "只读预检：读激光器状态并让门禁判定，**不发任何写指令**。"));
    fire_btn_ = new QPushButton(QStringLiteral("出光测量"), measure);
    fire_btn_->setObjectName(QStringLiteral("startBig"));
    fire_btn_->setMinimumHeight(62);
    fire_btn_->setEnabled(false);
    fire_btn_->setToolTip(QStringLiteral(
        "真实出光并采集。需要先点「预检」让门禁布防并放行；"
        "点击后还要二次确认才发指令。"));
    mrow->addWidget(precheck_btn_, 1);
    mrow->addWidget(fire_btn_, 2);
    measure->body()->addLayout(mrow);
    connect(precheck_btn_, &QPushButton::clicked, this, &MainWindow::on_precheck_fire);
    connect(fire_btn_, &QPushButton::clicked, this, &MainWindow::on_fire_measure);

    // 连续出光单独一行：它会**一直打到你点停止**，跟"出光几秒后自动收光"的
    // 测量是两件事，放在同一行里迟早点错。
    auto* crow = new QHBoxLayout;
    crow->setSpacing(10);
    run_btn_ = new QPushButton(QStringLiteral("连续出光"), measure);
    run_btn_->setMinimumHeight(48);
    run_btn_->setEnabled(false);
    run_btn_->setToolTip(QStringLiteral(
        "开始连续出光：门禁放行 → 写参数 → 工作使能 → 等放电 → 开光闸。\n"
        "之后激光会一直按设定频率出光，直到点「停止出光」。"));
    stop_run_btn_ = new QPushButton(QStringLiteral("停止出光"), measure);
    stop_run_btn_->setObjectName(QStringLiteral("stopBig"));
    stop_run_btn_->setMinimumHeight(48);
    // **停止按钮永远可点**。停止是安全动作，不该有任何前置条件 ——
    // 不看门禁、不看"后端说在不在跑"，哪怕状态轮询断了、界面显示的是旧数据，
    // 操作员也必须能按下去。后端 laser_stop 是无条件执行 0x24=0 + 0x21=0，
    // 没在出光时按也只是把设备置回安全的停止态。
    stop_run_btn_->setEnabled(true);
    stop_run_btn_->setToolTip(QStringLiteral(
        "收光 + 停机，并读回确认。任何时候都可以点 —— 它不看门禁。"));
    crow->addWidget(run_btn_, 1);
    crow->addWidget(stop_run_btn_, 1);
    measure->body()->addLayout(crow);
    connect(run_btn_, &QPushButton::clicked, this, &MainWindow::on_run_continuous);
    connect(stop_run_btn_, &QPushButton::clicked, this, &MainWindow::on_stop_continuous);

    fire_state_label_ = new QLabel(QStringLiteral("状态：未测量"), measure);
    fire_detail_label_ = new QLabel(QStringLiteral("放电次数 / 光谱组数 / 自适应耗时 会显示在这里"), measure);
    fire_detail_label_->setObjectName(QStringLiteral("dim"));
    fire_detail_label_->setWordWrap(true);
    fire_path_label_ = new QLabel(QString(), measure);
    fire_path_label_->setObjectName(QStringLiteral("dim"));
    fire_path_label_->setWordWrap(true);
    fire_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 路径要能选中复制
    measure->body()->addWidget(fire_state_label_);
    measure->body()->addWidget(fire_detail_label_);
    measure->body()->addWidget(fire_path_label_);
    col->addWidget(measure);

    // ── 激光器参数 ──────────────────────────────────────────────────
    laser_card_ = new Card(QStringLiteral("激光器参数"), panel);
    laser_apply_dot_ = new StatusDot(laser_card_);
    laser_apply_dot_->set_state(Level::Idle, QStringLiteral("未提交"));
    laser_card_->add_header_widget(laser_apply_dot_);
    laser_card_->set_hint(QStringLiteral(
        "范围取自风冷激光器 RS-232 协议（0x11~0x19）。参数写入业务层参数库并"
        "驱动门禁齐备性判定；是否已真正下发到硬件由后端 ACK 决定，UI 不代答。"));
    build_param_card(laser_card_, QStringLiteral("laser"), laser_fields_);
    col->addWidget(laser_card_);

    // ── 光谱仪参数 ──────────────────────────────────────────────────
    spec_card_ = new Card(QStringLiteral("光谱仪参数"), panel);
    spec_apply_dot_ = new StatusDot(spec_card_);
    spec_apply_dot_->set_state(Level::Idle, QStringLiteral("未提交"));
    spec_card_->add_header_widget(spec_apply_dot_);
    spec_card_->set_hint(QStringLiteral(
        "积分时间由设备驱动硬校验在 0.002~60000 ms，超范围会被业务层直接拒绝。"));
    build_param_card(spec_card_, QStringLiteral("spectrometer"), spec_fields_);
    col->addWidget(spec_card_);

    // 触发时序参数整张卡片已移除：laser_delay / laser_width 是驱动内部
    // FPGA 计数。反向模式由后端根据已标定激光脉宽生成，避免 UI 直接编辑原始计数。

    col->addStretch(1);
    scroll->setWidget(panel);
    return scroll;
}

void MainWindow::build_param_card(Card* card, const QString& group, ParamMap& fields) {
    // 表单：左标签右输入，行距 6。所有自旋框都允许键盘输入 + 上下微调。
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(7);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    std::size_t count = 0;
    const FieldSpec* specs = specs_for(group, count);

    int row = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const FieldSpec& fs = specs[i];
        const QString key = QString::fromUtf8(fs.key);
        const QString unit = QString::fromUtf8(fs.unit);

        auto* l = new QLabel(QString::fromUtf8(fs.label) +
                                 (unit.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(unit)),
                             card);
        l->setObjectName(QStringLiteral("dim"));
        if (fs.tooltip[0]) l->setToolTip(QString::fromUtf8(fs.tooltip));

        auto* box = new QDoubleSpinBox(card);
        box->setDecimals(fs.decimals);
        box->setRange(fs.lo, fs.hi);
        box->setSingleStep((fs.hi - fs.lo) / 1000.0 > 0.0 ? (fs.hi - fs.lo) / 1000.0 : 1.0);
        box->setKeyboardTracking(false);   // 边打边改会污染正在输入的数字
        box->setToolTip(QStringLiteral("范围 %1 ~ %2 %3")
                            .arg(fmt_fixed(fs.lo, fs.decimals),
                                 fmt_fixed(fs.hi, fs.decimals), unit)
                            .trimmed());
        // 初值 = 本机实测标定值（FieldSpec::def），既不是 0 也不是量程中点。
        // 现场打开界面看到的就是"这台设备实际在用的那套参数"，不用自己回想；
        // 后端一旦回传真值会覆盖它（见 sync_params_from_snapshot）。
        box->setValue(fs.def);
        box->setEnabled(true);
        fields.insert(key, box);

        grid->addWidget(l, row, 0);
        grid->addWidget(box, row, 1);
        ++row;
    }
    card->body()->addLayout(grid);

    auto* btn = new QPushButton(QStringLiteral("应用到 %1").arg(group), card);
    btn->setObjectName(QStringLiteral("primary"));
    btn->setEnabled(false);
    btn->setToolTip(QStringLiteral("把这一组参数提交给业务层（param_set）。"));
    card->body()->addWidget(btn);
    // "applyButton" 属性让 sync_params_from_snapshot 能回头解锁这个按钮
    // （Qt6 的 QVariant<QPushButton*> 不再需要 qVariantFromValue）。
    card->setProperty("applyButton", QVariant::fromValue(btn));

    StatusDot* status = (card == laser_card_) ? laser_apply_dot_ : spec_apply_dot_;
    connect(btn, &QPushButton::clicked, this, [this, group, fields, status] {
        apply_group(group, fields, status);
    });
}
// ── 右列：光谱 + 辅助监控 ─────────────────────────────────────────────

QWidget* MainWindow::build_right_column() {
    auto* panel = new QWidget;
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(4, 12, 12, 12);
    col->setSpacing(12);

    // ── 光谱卡 ──────────────────────────────────────────────────────
    auto* chart_card = new Card(QStringLiteral("光谱实时显示"), panel);
    auto* chart_hint = new QLabel(QStringLiteral("原始计数，未归一化"), chart_card);
    chart_hint->setObjectName(QStringLiteral("cardHint"));
    chart_card->add_header_widget(chart_hint);
    frame_label_ = new QLabel(QStringLiteral("帧 —"), chart_card);
    frame_label_->setObjectName(QStringLiteral("caption"));
    chart_card->add_header_widget(frame_label_);

    // 工具栏：自动 Y / 锁 Y / 暂停 / 截图。
    auto* tools = new QHBoxLayout;
    tools->setSpacing(8);
    auto_y_ = new QCheckBox(QStringLiteral("自动 Y 轴"), chart_card);
    // **默认不自动量程** —— 这是照厂家参考实现（Qt_demo_multichannel/plot.cpp）来的：
    // 它把纵轴**固定**在 -500 ~ 66000，也就是 ADC 满量程那一档，整场都不动。
    // 对 LIBS 来说这是对的：峰有多高本身就是信息，纵轴一跳，帧与帧之间就没法比了。
    auto_y_->setChecked(false);
    auto_y_->setToolTip(QStringLiteral(
        "默认关闭：纵轴固定在 0~64000（满量程），帧与帧之间可比。\n"
        "勾上则随数据自动缩放（上限仍锁 64000）。"));
    lock_y_ = new QCheckBox(QStringLiteral("锁定 Y 轴"), chart_card);
    lock_y_->setToolTip(QStringLiteral("锁定当前 Y 范围，防止单次强信号把后续弱信号压没。"));
    pause_ = new QCheckBox(QStringLiteral("暂停绘制"), chart_card);
    pause_->setToolTip(QStringLiteral("暂停只影响画曲线，数据流照常在后台取。"));
    // 预览要不要跟着激光的 pin6 TTL 采。
    //   不勾（默认）：自由触发，看的是**背景/杂散光**，也是最安全的预览；
    //   勾上：光谱仪等激光触发，每发激光采一帧 —— 出光时能看到**干净的 LIBS 谱**，
    //         不出光时会一帧都等不到（这是物理事实，不是缺陷）。
    follow_trigger_ = new QCheckBox(QStringLiteral("预览跟随激光触发"), chart_card);
    follow_trigger_->setToolTip(QStringLiteral(
        "勾上后光谱仪等激光的 pin6 TTL，每发激光采一帧。\n"
        "连续出光时能看到干净的 LIBS 谱；不出光时会一直等不到数据（正常）。\n"
        "不勾 = 自由触发，看背景谱。"));
    auto* shot = new QPushButton(QStringLiteral("截图 (PNG+CSV)"), chart_card);
    shot->setToolTip(QStringLiteral("保存当前视图为 PNG，同时把本帧原始计数导出为 CSV。"));
    tools->addWidget(auto_y_);
    tools->addWidget(lock_y_);
    tools->addWidget(pause_);
    tools->addWidget(follow_trigger_);
    tools->addWidget(shot);
    tools->addStretch(1);
    absent_label_ = new QLabel(QString(), chart_card);
    absent_label_->setObjectName(QStringLiteral("cardHint"));
    absent_label_->setWordWrap(true);
    tools->addWidget(absent_label_, 1);
    chart_card->body()->addLayout(tools);

    plot_->setParent(chart_card);
    chart_card->body()->addWidget(plot_, 1);
    col->addWidget(chart_card, 1);
    connect(auto_y_, &QCheckBox::toggled, this, &MainWindow::on_auto_y_toggled);
    connect(lock_y_, &QCheckBox::toggled, this, &MainWindow::on_lock_y_toggled);
    connect(pause_, &QCheckBox::toggled, this, &MainWindow::on_pause_toggled);
    connect(shot, &QPushButton::clicked, this, &MainWindow::on_screenshot);

    // ── 辅助监控条 ──────────────────────────────────────────────────
    auto* aux = new Card(QStringLiteral("辅助监控"), panel);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(10);

    dist_metric_ = new Metric(QStringLiteral("测距"), aux);
    // 后端当前只保证协议 raw，不做工程单位换算；标成 raw，避免现场把
    // 设备寄存器值误当成 mm。
    dist_metric_->set_unit(QStringLiteral("raw"));
    temp_metric_ = new Metric(QStringLiteral("室内温度"), aux);
    temp_metric_->set_unit(QStringLiteral("°C"));
    motion_metric_ = new Metric(QStringLiteral("正转标定时间"), aux);
    motion_metric_->set_unit(QStringLiteral("s"));
    count_metric_ = new Metric(QStringLiteral("有矿观测"), aux);

    int c = 0;
    for (Metric* m : {dist_metric_, temp_metric_, motion_metric_, count_metric_}) {
        grid->addWidget(m, 0, c++);
    }
    aux->body()->addLayout(grid);

    auto* dot_row = new QHBoxLayout;
    dot_row->setSpacing(22);
    ac_dot_ = new StatusDot(aux);
    ac_dot_->set_state(Level::Idle, QStringLiteral("空调 未知"));
    motion_dot_ = new StatusDot(aux);
    motion_dot_->set_state(Level::Idle, QStringLiteral("运动 未知"));
    ore_dot_ = new StatusDot(aux);
    ore_dot_->set_state(Level::Idle, QStringLiteral("有矿 未知"));
    for (StatusDot* d : {ac_dot_, motion_dot_, ore_dot_}) dot_row->addWidget(d);
    dot_row->addStretch(1);
    aux->body()->addLayout(dot_row);
    col->addWidget(aux);

    // ── 算法预留窗口 ────────────────────────────────────────────────
    composition_card_ = new Card(QStringLiteral("成分分析（算法窗口）"), panel);
    auto* ph = new QLabel(QStringLiteral("算法接口未连接"), composition_card_);
    ph->setObjectName(QStringLiteral("placeholderText"));
    ph->setAlignment(Qt::AlignCenter);
    ph->setMinimumHeight(96);
    composition_card_->body()->addWidget(ph);
    composition_card_->set_hint(QStringLiteral(
        "后端 composition 命令的前置条件尚未满足（无算法接口/无网络层），"
        "这里保留窗口等算法接入。空白是有意留的，不是没画完。"));
    col->addWidget(composition_card_);

    return panel;
}

// ── 底栏：设备条 + 日志 ───────────────────────────────────────────────

QWidget* MainWindow::build_bottom_bar(QWidget* host) {
    auto* bar = new QWidget(host);
    bar->setObjectName(QStringLiteral("bottomBar"));
    bar->setFixedHeight(178);

    auto* col = new QVBoxLayout(bar);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(6);

    // 设备连接条
    device_strip_ = new QLabel(bar);
    device_strip_->setObjectName(QStringLiteral("deviceStrip"));
    device_strip_->setTextFormat(Qt::RichText);
    device_strip_->setText(QStringLiteral(
        "<span style='color:#93A3B2'>设备：</span>"
        "<span style='color:#6E7B8A'>等待守护进程状态……</span>"));
    col->addWidget(device_strip_);

    auto* head = new QHBoxLayout;
    auto* t = new QLabel(QStringLiteral("系统消息"), bar);
    t->setObjectName(QStringLiteral("cardTitle"));
    head->addWidget(t);
    head->addStretch(1);
    auto* export_btn = new QPushButton(QStringLiteral("导出日志"), bar);
    auto* clear_btn = new QPushButton(QStringLiteral("清空"), bar);
    clear_btn->setObjectName(QStringLiteral("ghost"));
    head->addWidget(export_btn);
    head->addWidget(clear_btn);
    col->addLayout(head);

    log_view_ = new QListWidget(bar);
    log_view_->setObjectName(QStringLiteral("logView"));
    // QListWidget 没有 setMaximumBlockCount，手工裁剪到 1000 条（见 log_line）。
    col->addWidget(log_view_, 1);

    connect(export_btn, &QPushButton::clicked, this, &MainWindow::on_export_log);
    connect(clear_btn, &QPushButton::clicked, this, &MainWindow::on_clear_log);

    return bar;
}

// ── 光谱图 ────────────────────────────────────────────────────────────

void MainWindow::build_chart() {
    // 自绘光谱图。**刻意不用 QtCharts** —— QLineSeries 每帧必须把点拷贝进
    // 控件内部（四路 2048 点 = 8192 次拷贝），显示端因此跟不上采集。
    // 换成 QPainter::drawPolyline 直接画数组，与厂家参考实现的 Qwt 同一条路。
    plot_ = new SpectrumPlot();
    plot_->setStyleSheet(QStringLiteral("background:#0A0E13;border-radius:6px;"));
}



void MainWindow::rebuild_series(const QVector<ChannelData>& channels) {
    // 四路覆盖 185~565 nm，是**首尾相接的四个波段**（相邻有 ≈7 nm 重叠），
    // 本来就是一条谱。重叠区归后一路：把前一路截到后一路的起始波长为止 ——
    // 直接按波长排序合并会在重叠区来回折成锯齿（两路的采样栅格不同）。
    // 每一段带自己的颜色，所以图上一眼能看出哪一段是哪台光谱仪采的。
    QVector<const ChannelData*> ordered;
    for (const ChannelData& c : channels)
        if (c.present && !c.wavelength_nm.isEmpty()) ordered.push_back(&c);
    std::sort(ordered.begin(), ordered.end(), [](const ChannelData* a, const ChannelData* b) {
        return a->wavelength_nm.first() < b->wavelength_nm.first();
    });

    QVector<SpectrumPlot::Segment> segments;
    segments.reserve(ordered.size());
    for (int k = 0; k < ordered.size(); ++k) {
        const ChannelData& c = *ordered[k];
        const double upper = (k + 1 < ordered.size())
                                 ? ordered[k + 1]->wavelength_nm.first()
                                 : std::numeric_limits<double>::infinity();
        SpectrumPlot::Segment seg;
        seg.name = c.serial;
        seg.color = theme::color::series(k);
        const int n = std::min<int>(c.wavelength_nm.size(), c.counts.size());
        seg.points.reserve(n);
        for (int i = 0; i < n; ++i) {
            const double wl = c.wavelength_nm[i];
            if (wl >= upper) break;
            // **原始计数，不归一化**：四台灵敏度不同，归一化之后曲线看着
            // 一样高，反而把"哪一段信号强"这个最该看的信息抹掉了。
            seg.points.append(QPointF(wl, c.counts[i]));
        }
        if (!seg.points.isEmpty()) segments.push_back(std::move(seg));
    }

    if (plot_) {
        plot_->set_dimmed(false);   // 有数据了，恢复彩色
        plot_->set_segments(std::move(segments));
    }
}
// ── 流控制 ────────────────────────────────────────────────────────────

void MainWindow::start_stream() {
    // 先让业务层把流起起来。UI **只发意图**，不自己决定什么时候采——
    // 什么时候能采、能采几路，是业务层看着设备和配置定的。
    try {
        client_.command(QStringLiteral("spectrum_stream"),
                        QJsonObject{{QStringLiteral("action"), QStringLiteral("start")},
                                    {QStringLiteral("follow_site_trigger_mode"),
                                     follow_trigger_ != nullptr && follow_trigger_->isChecked()},
                                    // 一帧四通道 2048 点，序列化 + 过管道实测约 800 ms。
                                    // 按默认 200 ms 产帧客户端永远追不上（队列一路丢帧、
                                    // 界面一直空）。这里按 1 秒一拍，前端稳稳跟得上。
                                    {QStringLiteral("min_period_ms"), 1000}});
    } catch (const QString& e) {
        set_banner(QStringLiteral("启动失败：%1").arg(e), 2);
        QMessageBox::warning(this, QStringLiteral("启动流失败"), e);
        return;
    }
    if (poller_->open()) {
        set_banner(preview_provenance_text(), 1);
        log_line(Level::Ok, QStringLiteral("已订阅光谱预览流。"));
    } else {
        // **订阅失败不能说成"预览已停止"** —— 那是另一回事，会把真正的原因盖住。
        // （曾经这里显示的正是"预览已停止，点击启动检测重新开始"，而实际原因是
        //   订阅号取错层级、open() 直接失败、定时器从没启动。）
        set_banner(QStringLiteral("订阅预览流失败：拿不到订阅号或守护进程不可达（见日志）。"), 2);
        log_line(Level::Danger, QStringLiteral("订阅预览流失败；请检查守护进程与 spectrum_stream。"));
    }
}

void MainWindow::stop_stream() {
    poller_->close();
    try {
        client_.command(QStringLiteral("spectrum_stream"),
                        QJsonObject{{QStringLiteral("action"), QStringLiteral("stop")}});
    } catch (const QString&) {
        // 停止失败无所谓：守护进程重启后流自然不在了。
    }
    set_banner(disconnect_banner_text(), 0);
    log_line(Level::Idle, QStringLiteral("预览流已停止。"));
}

void MainWindow::on_frame(const FrameData& frame) {
    frame_seen_ = frame.frame_index;
    render_chart_frame_info(frame);
    if (paused_) return;   // 暂停只冻结绘制，帧号等信息照常更新

    QStringList absent;
    for (const ChannelData& c : frame.channels) {
        if (!c.present) absent << QStringLiteral("%1（%2）").arg(c.serial, c.reason);
    }
    absent_label_->setText(absent.isEmpty()
                               ? QString()
                               : QStringLiteral("缺席通道：%1").arg(absent.join(QStringLiteral("、"))));

    if (frame.ok && frame.has_any_present_channel()) {
        rebuild_series(frame.channels);
        QString tail;
        if (!absent.isEmpty()) {
            tail = QStringLiteral("  ｜ 缺席：%1").arg(absent.join(QStringLiteral("、")));
        }
        set_banner(preview_provenance_text() + tail, absent.isEmpty() ? 1 : 2);
    } else if (frame.ok) {
        set_banner(QStringLiteral("采集返回，但没有通道有数据：%1")
                       .arg(absent.isEmpty() ? QStringLiteral("(无通道)")
                                             : absent.join(QStringLiteral("、"))),
                   2);
    }
}

void MainWindow::render_chart_frame_info(const FrameData& f) {
    QString sync_req = f.requested_sync ? QStringLiteral("请求同步=是") : QStringLiteral("请求同步=否");
    // 同步事实与请求分开展示：请求了不等于做到了（§3.10.1）。
    frame_label_->setText(QStringLiteral("帧 #%1 ｜ 采样点 %2 ｜ %3 ｜ 硬件同步=%4")
                              .arg(f.frame_index)
                              .arg(f.channels.isEmpty() ? 0 : f.channels.first().counts.size())
                              .arg(sync_req)
                              .arg(f.hardware_synchronized ? QStringLiteral("已达成")
                                                           : QStringLiteral("未达成")));
}

void MainWindow::on_capture_failed(const QString& code, const QString& message) {
    // 先置灰曲线：不管下面走哪条分支，屏幕上留着的都是**上一帧**，不是新数据。
    // 置灰很关键 —— 只弹个提示然后留一条彩色曲线，看的人会以为那是新数据。
    if (plot_) plot_->set_dimmed(true);

    // **跟随激光触发模式下等不到触发，是"在等"，不是"失败"。**
    //
    // 勾上那个开关后，光谱仪在 pin6 上等激光的 TTL；没出光时它当然等不到,
    // 超时是**唯一正确的结果**。把它报成红色"采集失败"，操作员会以为设备坏了,
    // 而真正要做的是去出光（或把这个开关取消）。
    //
    // 其它错误码（通信断、SDK 报错、通道缺失）仍然是真失败，照旧红色。
    const bool following = follow_trigger_ != nullptr && follow_trigger_->isChecked();
    if (following && code == QLatin1String("capture_timeout")) {
        set_banner(QStringLiteral(
            "正在等激光触发（pin6）… 出光后这里会显示逐发的 LIBS 测量谱。"
            "现在没出光，所以采不到数据 —— 这是正常的。"), 1);
        return;
    }

    // 其余一律当真失败：红色横幅 + 日志。
    set_banner(QStringLiteral("采集失败：%1 — %2（保留上一帧曲线并置灰，不是新数据）")
                   .arg(code, message),
               2);
    log_line(Level::Danger, QStringLiteral("采集失败 %1：%2").arg(code, message));
}

void MainWindow::on_stream_error(const QString& message) {
    set_banner(QStringLiteral("与守护进程通信出错：%1").arg(message), 2);
    log_line(Level::Danger, QStringLiteral("流通信出错：%1").arg(message));
}

void MainWindow::on_stats(int pending, qint64 dropped) {
    if (dropped > 0) {
        set_banner(QStringLiteral("显示跟不上采集：已丢 %1 帧（队列按背压丢弃最旧帧）").arg(dropped), 2);
    }
    Q_UNUSED(pending)
}

QString MainWindow::disconnect_banner_text() const {
    return QStringLiteral("预览已停止。点击「启动检测」重新开始。");
}

QString MainWindow::preview_provenance_text() const {
    const bool follow = follow_trigger_ != nullptr && follow_trigger_->isChecked();
    if (follow) {
        return QStringLiteral(
            "⚠ 预览跟随激光触发：显示的是**逐发的 LIBS 测量谱**（门禁放行后出光才有）。"
            "未出光时它会一直等 pin6 触发，看不到数据是正常的。");
    }
    return QString::fromUtf8(kDisclaimer);
}

// ── 检测控制 ──────────────────────────────────────────────────────────

void MainWindow::on_start_detection() {
    // 高风险动作要二次确认：误触一次就是一次出光请求。
    const auto r = QMessageBox::question(
        this, QStringLiteral("确认启动检测"),
        QStringLiteral("确定要发送「开始检测」业务信号吗？\n\n"
                       "· 这是业务信号，不是物理触发；\n"
                       "· 出光许可由门禁决定，门禁拒绝时不会有光；\n"
                       "· 当前会先启动自由触发预览（未出光）。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;

    bool preview_started = false;
    try {
        const bool follow = follow_trigger_ != nullptr && follow_trigger_->isChecked();
        client_.command(QStringLiteral("spectrum_stream"),
                        QJsonObject{{QStringLiteral("action"), QStringLiteral("start")},
                                    {QStringLiteral("follow_site_trigger_mode"), follow},
                                    {QStringLiteral("min_period_ms"), 1000}});
        preview_started = poller_->open();
        if (!preview_started) {
            // 后端流已启动但订阅失败时，不能留下无人消费的生产者。
            try {
                client_.command(QStringLiteral("spectrum_stream"),
                                QJsonObject{{QStringLiteral("action"), QStringLiteral("stop")}});
            } catch (const QString&) {
            }
            set_banner(QStringLiteral("订阅预览流失败，已回滚流服务。"), 2);
            log_line(Level::Danger, QStringLiteral("启动检测失败：预览流订阅未建立。"));
            return;
        }
        set_banner(preview_provenance_text(), 1);
    } catch (const QString& e) {
        log_line(Level::Danger, QStringLiteral("启动预览失败：%1").arg(e));
        set_banner(QStringLiteral("启动预览失败：%1").arg(e), 2);
        return;
    }
    try {
        client_.command(QStringLiteral("detection_start"),
                        QJsonObject{{QStringLiteral("requested_by"), QStringLiteral("ui")}});
        log_line(Level::Ok, QStringLiteral("已发送 detection_start 业务信号（尚非物理触发）。"));
    } catch (const QString& e) {
        // 预览已经启动但业务信号失败时必须回滚订阅；否则按钮看似没启动，
        // 后端却一直保留一个无人消费的 spectrum_stream 订阅。
        stop_stream();
        set_banner(QStringLiteral("启动检测失败：%1").arg(e), 2);
        QMessageBox::warning(this, QStringLiteral("启动检测失败"), e);
        return;
    }
    start_btn_->setEnabled(false);
    stop_btn_->setEnabled(true);
    if (state_poller_) state_poller_->request_refresh();
}

void MainWindow::on_stop_detection() {
    try {
        client_.command(QStringLiteral("gate"),
                        QJsonObject{{QStringLiteral("action"), QStringLiteral("disarm")}});
        log_line(Level::Warn, QStringLiteral("已请求门禁撤防（gate disarm）。"));
    } catch (const QString& e) {
        log_line(Level::Warn, QStringLiteral("撤防失败：%1").arg(e));
    }
    stop_stream();
    start_btn_->setEnabled(true);
    stop_btn_->setEnabled(false);
    if (state_poller_) state_poller_->request_refresh();
}

// ── LIBS 测量（真出光）───────────────────────────────────────────────

void MainWindow::on_precheck_fire() {
    if (firing_) return;

    const QString trigger_direction =
        trigger_direction_ == nullptr
            ? QStringLiteral("laser_to_spectrometer")
            : trigger_direction_->currentData().toString();

    // 业务层的"核心参数齐备"要求 laser / spectrometer / trigger 三组都存在
    // （见 runtime.cpp 的 default_required_groups）。这里写入 trigger 组只是
    // 创建/刷新参数分组以满足门禁的齐备性；反向触发的实际 Pin 23 时序由
    // 后端根据 laser_width_ns/laser_delay_ns 和激光器脉宽在本次 capture 请求
    // 中计算，不读取这两个持久字段，因此不会被下面的 0 覆盖。
    try {
        client_.command(QStringLiteral("param_set"),
                        QJsonObject{{QStringLiteral("group"), QStringLiteral("trigger")},
                                    {QStringLiteral("values"),
                                     QJsonObject{{QStringLiteral("laser_delay_cycles"), 0.0},
                                                 {QStringLiteral("laser_width_cycles"), 0.0}}},
                                    {QStringLiteral("timing"), QStringLiteral("immediately")}});
    } catch (const QString& e) {
        // 写不进去也不必中断：门禁会在预检结果里如实报"参数未齐备"。
        log_line(Level::Warn, QStringLiteral("写入 trigger 组（门禁占位字段）失败：%1").arg(e));
    }

    try {
        // 业务层的 precheck 只读：它会读一次激光器状态让门禁判定，
        // 但**一条写指令都不发**（laser_fire.cpp 的 precheck 里钉死了）。
        // 注意 PipeClient::command 返回的是**整个响应** {ok, payload}，
        // 真正的结果在 payload 里 —— 这里取错一层，gate 会解析成空对象，
        // 于是"预检永远显示不通过"，而后端其实是放行的。
        const QJsonObject response = client_.command(
            QStringLiteral("laser_fire"),
            QJsonObject{{QStringLiteral("precheck_only"), true},
                        {QStringLiteral("trigger_direction"), trigger_direction}}, 20000);
        const QJsonObject payload = response.value(QStringLiteral("payload")).toObject();
        const QJsonObject gate = payload.value(QStringLiteral("gate")).toObject();
        const bool allowed = gate.value(QStringLiteral("allowed")).toBool();
        fire_state_label_->setText(allowed ? QStringLiteral("状态：预检通过")
                                           : QStringLiteral("状态：预检未通过"));
        fire_detail_label_->setText(gate.value(QStringLiteral("detail")).toString());
        log_line(allowed ? Level::Ok : Level::Warn,
                 QStringLiteral("出光预检：%1").arg(gate.value(QStringLiteral("detail")).toString()));
        if (state_poller_) state_poller_->request_refresh();
    } catch (const QString& e) {
        fire_state_label_->setText(QStringLiteral("状态：预检未通过"));
        fire_detail_label_->setText(e);
        log_line(Level::Warn, QStringLiteral("出光预检未通过：%1").arg(e));
        if (state_poller_) state_poller_->request_refresh();
    }
}

void MainWindow::on_fire_measure() {
    if (firing_) return;

    // 出光是物理动作，必须操作员明确确认。业务层**刻意不做**操作员确认
    // （任务书把"物理接线确认、就绪门禁"划给了现场），所以那个责任的落点
    // 就是这里：不点"是"，一条指令都不会发出去。
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认出光测量"),
        QStringLiteral("即将执行完整的 LIBS 测量流程：\n\n"
                       "　写会话参数 → 工作使能 → 等设备进入放电\n"
                       "　→ 开光闸连续出光 → 收光停机 → 四通道采集 → 落盘\n\n"
                       "· 激光会**真实出光**；\n"
                       "· 请确认光路封闭、样品在位、护目镜已佩戴；\n"
                       "· 门禁不放行时业务层一条写指令都不会发。\n\n"
                       "确定继续吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        log_line(Level::Idle, QStringLiteral("已取消出光测量。"));
        return;
    }

    // 预览流与外触发采集抢同一条 pin 6 的 TTL，出光前先把预览停掉。
    stop_stream();

    firing_ = true;
    if (trigger_direction_) trigger_direction_->setEnabled(false);
    fire_btn_->setEnabled(false);
    precheck_btn_->setEnabled(false);
    fire_state_label_->setText(QStringLiteral("状态：出光中…"));
    fire_detail_label_->setText(QStringLiteral(
        "正在跑完整流程，约 15~25 秒（含 4~6 秒自适应）。期间状态轮询已暂停。"));
    fire_path_label_->setText(QString());
    // 出光期间 device_mutex 被激光流程握着，status 轮询只会排队到超时，
    // 那会让界面误报"守护进程离线"。停掉它，结束后再起。
    if (state_poller_) state_poller_->stop();
    log_line(Level::Warn, QStringLiteral("开始出光测量：激光将真实出光。"));

    const QJsonObject fire_args = current_fire_args();

    // 一次 laser_fire 是分钟级的同步往返，**必须在工作线程做**：
    // 在 UI 线程同步调用会把这 20 秒窗口整个冻住，工控触摸屏上不可接受。
    // 工作线程里另起一个 PipeClient，不 cross-thread 用 UI 线程那个。
    const QString pipe = PipeClient::default_pipe_name();
    const QPointer<MainWindow> guard(this);
    QThread* worker = QThread::create([guard, pipe, fire_args]() {
        PipeClient client(pipe);
        try {
            // command() 返回整个响应；结果在 payload 里。
            const QJsonObject response = client.command(
                QStringLiteral("laser_fire"), fire_args, 120000);
            const QJsonObject payload = response.value(QStringLiteral("payload")).toObject();
            if (!guard) return;
            QMetaObject::invokeMethod(guard.data(), [guard, payload]() {
                if (guard) guard->on_fire_result(payload);
            }, Qt::QueuedConnection);
        } catch (const QString& e) {
            if (!guard) return;
            QMetaObject::invokeMethod(guard.data(), [guard, e]() {
                if (guard) guard->on_fire_failed(e);
            }, Qt::QueuedConnection);
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::on_fire_result(const QJsonObject& payload) {
    firing_ = false;
    if (trigger_direction_) trigger_direction_->setEnabled(true);
    precheck_btn_->setEnabled(true);
    fire_btn_->setEnabled(fire_allowed_);

    const int discharges = payload.value(QStringLiteral("discharges")).toInt();
    const int groups = payload.value(QStringLiteral("spectrum_groups")).toInt();
    const int settle = payload.value(QStringLiteral("settle_ms")).toInt();
    const int before = payload.value(QStringLiteral("discharge_counter_before")).toInt();
    const int after = payload.value(QStringLiteral("discharge_counter_after")).toInt();
    const QJsonObject delivery = payload.value(QStringLiteral("delivery")).toObject();
    const bool saved = delivery.value(QStringLiteral("local_saved")).toBool();
    const QString path = delivery.value(QStringLiteral("local_path")).toString();

    // 后端回显实际采用的触发路径。这个字段是现场核对“UI 选择”和
    // 业务层实际编排是否一致的证据，不能只显示下拉框当前值。
    const QString trigger_path = payload.value(QStringLiteral("trigger_path")).toString();
    if (!trigger_path.isEmpty()) {
        const bool programmed = payload.value(QStringLiteral("trigger_output_programmed"))
                                     .toBool(false);
        trigger_path_label_->setText(
            QStringLiteral("本次实际触发路径：%1 · 光谱仪输出已编程：%2")
                .arg(trigger_path, fmt_bool_zh(programmed)));
    }

    fire_state_label_->setText(saved ? QStringLiteral("状态：出光完成，光谱已落盘")
                                     : QStringLiteral("状态：出光完成，但落盘失败"));
    fire_detail_label_->setText(
        QStringLiteral("放电 %1 次（%2 → %3）· 光谱 %4 组 · 自适应 %5 ms")
            .arg(discharges).arg(before).arg(after).arg(groups).arg(settle));
    fire_path_label_->setText(saved
        ? QStringLiteral("文件：%1").arg(path)
        : QStringLiteral("落盘错误：%1").arg(delivery.value(QStringLiteral("local_error")).toString()));

    if (saved) {
        log_line(Level::Ok, QStringLiteral("出光测量完成：放电 %1 次，光谱 %2 组，已落盘 %3")
                                .arg(discharges).arg(groups).arg(path));
    } else {
        log_line(Level::Danger, QStringLiteral("出光测量完成但数据未落盘：%1")
                                    .arg(delivery.value(QStringLiteral("local_error")).toString()));
        set_banner(QStringLiteral("出光已完成，但本次光谱没有落盘。"), 2);
    }

    // 把**本次测到的谱画出来**。
    //
    // 出光前已经停掉了预览流，所以这条曲线会一直留在图上，不会被下一帧
    // 背景谱覆盖掉。以前只显示"采集完成 + 文件名"，操作员看不到自己刚打出来的谱。
    {
        const QJsonObject spectra = payload.value(QStringLiteral("spectra")).toObject();
        const QJsonArray scan_groups = spectra.value(QStringLiteral("scan_groups")).toArray();
        if (!scan_groups.isEmpty()) {
            QVector<ChannelData> channels;
            const QJsonArray chs = scan_groups.first().toObject()
                                       .value(QStringLiteral("channels")).toArray();
            for (const QJsonValue& cv : chs) {
                const QJsonObject co = cv.toObject();
                ChannelData c;
                c.serial = co.value(QStringLiteral("serial")).toString();
                c.present = true;
                for (const QJsonValue& v : co.value(QStringLiteral("wavelength_nm")).toArray())
                    c.wavelength_nm.push_back(v.toDouble());
                for (const QJsonValue& v : co.value(QStringLiteral("counts")).toArray())
                    c.counts.push_back(v.toDouble());
                channels.push_back(c);
            }
            if (!channels.isEmpty()) {
                rebuild_series(channels);
                frame_label_->setText(QStringLiteral("本次 LIBS 测量谱 · %1 通道")
                                          .arg(channels.size()));
                absent_label_->setText(QString());
                set_banner(QStringLiteral(
                    "本次显示的是 LIBS 测量谱：真实出光后同步采集，Y 轴为原始计数。"), 0);
            }
        }
    }

    if (state_poller_) {
        state_poller_->start();
        state_poller_->request_refresh();
    }
}

void MainWindow::on_fire_failed(const QString& message) {
    firing_ = false;
    if (trigger_direction_) trigger_direction_->setEnabled(true);
    precheck_btn_->setEnabled(true);
    fire_btn_->setEnabled(fire_allowed_);
    fire_state_label_->setText(QStringLiteral("状态：出光测量失败"));
    fire_detail_label_->setText(message);
    // 失败信息里已经带了"出光已执行但采集失败"这类事实，原样上屏，
    // UI 不替业务层改写结论。
    log_line(Level::Danger, QStringLiteral("出光测量失败：%1").arg(message));
    set_banner(QStringLiteral("出光测量失败：%1").arg(message), 2);
    if (state_poller_) {
        state_poller_->start();
        state_poller_->request_refresh();
    }
}

QJsonObject MainWindow::current_fire_args() const {
    // 本次参数：**界面上显示的就是实际要用的**。
    // 键名映射写在 FieldSpec::over 里，只有那一处需要同步。
    const auto collect = [](const FieldSpec* specs, std::size_t count, const ParamMap& fields) {
        QJsonObject out;
        for (std::size_t i = 0; i < count; ++i) {
            const FieldSpec& fs = specs[i];
            if (!fs.over[0]) continue;
            const auto it = fields.constFind(QString::fromUtf8(fs.key));
            if (it == fields.constEnd() || !it.value()) continue;
            out.insert(QString::fromUtf8(fs.over), it.value()->value());
        }
        return out;
    };
    QJsonObject args;
    args.insert(QStringLiteral("laser"),
                collect(kLaserFields, sizeof(kLaserFields) / sizeof(FieldSpec), laser_fields_));
    args.insert(QStringLiteral("spectrometer"),
                collect(kSpecFields, sizeof(kSpecFields) / sizeof(FieldSpec), spec_fields_));
    // 与后端约定的显式触发方向。缺省值保持旧版本行为，避免升级 UI 后
    // 误把既有“激光器 → 光谱仪”接线切到反向。
    args.insert(QStringLiteral("trigger_direction"),
                trigger_direction_ == nullptr
                    ? QStringLiteral("laser_to_spectrometer")
                    : trigger_direction_->currentData().toString());
    return args;
}

void MainWindow::on_run_continuous() {
    if (firing_) return;

    // 连续出光是**会一直出光**的动作，确认框要把这一点说白。
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认连续出光"),
        QStringLiteral("即将开始**连续出光**：\n\n"
                       "　写会话参数 → 工作使能 → 等设备进入放电 → 开光闸\n\n"
                       "· 之后激光会**一直按设定频率出光**，直到你点「停止出光」；\n"
                       "· 请确认光路封闭、样品在位、护目镜已佩戴；\n"
                       "· 离开设备前务必先停止。守护进程退出时也会自动收光，"
                       "但那是兜底，不是你可以依赖的东西。\n\n"
                       "确定继续吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        log_line(Level::Idle, QStringLiteral("已取消连续出光。"));
        return;
    }

    // **把预览流拉起来**。
    //
    // 连续出光的用处就是"一边打一边看谱"；不自动启动的话，操作员点了连续出光
    // 却对着一个空图打光 —— 现场已经这么踩过一次（流的 running 是 false、
    // 一帧都没有，而激光其实打得好好的）。
    //
    // 预览是自由触发，不和外触发抢 pin6 的 TTL，所以两者可以同时跑。
    // （出光测量反过来：那一次要独占 TTL，所以必须先把预览停掉。）
    try {
        client_.command(QStringLiteral("spectrum_stream"),
                        QJsonObject{{QStringLiteral("action"), QStringLiteral("start")},
                                    {QStringLiteral("follow_site_trigger_mode"),
                                     follow_trigger_ != nullptr && follow_trigger_->isChecked()},
                                    {QStringLiteral("min_period_ms"), 1000}});
        if (!poller_->open()) {
            log_line(Level::Warn, QStringLiteral("连续出光：订阅预览流失败，本次只能出光、看不到谱。"));
        }
    } catch (const QString& e) {
        log_line(Level::Warn, QStringLiteral("连续出光前启动预览流失败：%1").arg(e));
    }

    const QJsonObject args = current_fire_args();
    if (trigger_direction_) trigger_direction_->setEnabled(false);
    run_btn_->setEnabled(false);
    fire_state_label_->setText(QStringLiteral("状态：正在启动连续出光…"));
    fire_detail_label_->setText(QStringLiteral("等待设备进入「正在工作且放电」，约 5~7 秒。"));
    log_line(Level::Warn, QStringLiteral("开始连续出光：激光将持续出光，直到手动停止。"));

    // 5~7 秒的同步往返，仍在工作线程做，别冻住界面。
    const QString pipe = PipeClient::default_pipe_name();
    const QPointer<MainWindow> guard(this);
    QThread* worker = QThread::create([guard, pipe, args]() {
        PipeClient client(pipe);
        QString error;
        QJsonObject payload;
        try {
            const QJsonObject response = client.command(QStringLiteral("laser_run"), args, 60000);
            payload = response.value(QStringLiteral("payload")).toObject();
        } catch (const QString& e) {
            error = e;
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, payload, error]() {
            if (!guard) return;
            if (!error.isEmpty()) {
                guard->fire_state_label_->setText(QStringLiteral("状态：连续出光启动失败"));
                guard->fire_detail_label_->setText(error);
                guard->log_line(Level::Danger, QStringLiteral("连续出光启动失败：%1").arg(error));
                guard->set_banner(QStringLiteral("连续出光启动失败：%1").arg(error), 2);
                guard->run_btn_->setEnabled(guard->fire_allowed_);
                if (guard->trigger_direction_) guard->trigger_direction_->setEnabled(true);
                if (guard->state_poller_) guard->state_poller_->request_refresh();
                return;
            }
            const int settle = payload.value(QStringLiteral("settle_ms")).toInt();
            guard->fire_state_label_->setText(QStringLiteral("状态：★ 正在连续出光"));
            guard->fire_detail_label_->setText(
                QStringLiteral("自适应 %1 ms 后已开光闸。激光在持续出光 —— 点「停止出光」收光。")
                    .arg(settle));
            guard->log_line(Level::Warn, QStringLiteral(
                "连续出光已开始（自适应 %1 ms）。**离开前必须点停止出光。**").arg(settle));
            guard->set_banner(QStringLiteral("⚠ 激光正在**持续出光** —— 点「停止出光」收光。"), 1);
            guard->run_btn_->setEnabled(false);
            guard->stop_run_btn_->setEnabled(true);
            if (guard->state_poller_) guard->state_poller_->request_refresh();
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::on_stop_continuous() {
    // 停止**不看门禁**：门禁管的是"能不能开始出光"，停止永远该能停。
    stop_run_btn_->setEnabled(false);
    run_btn_->setEnabled(false);
    if (trigger_direction_) trigger_direction_->setEnabled(false);
    fire_state_label_->setText(QStringLiteral("状态：正在停止连续出光…"));
    fire_detail_label_->setText(QStringLiteral("正在收光、停机并读回确认；界面仍可操作。"));
    // laser_stop 可能等待串口确认，不能在 UI 线程同步调用，否则点击停止后
    // 触摸屏会冻结最多 30 秒。与 laser_fire/laser_run 使用同一异步模式。
    const QString pipe = PipeClient::default_pipe_name();
    const QPointer<MainWindow> guard(this);
    QThread* worker = QThread::create([guard, pipe]() {
        PipeClient client(pipe);
        QString error;
        try {
            client.command(QStringLiteral("laser_stop"), QJsonObject(), 30000);
        } catch (const QString& e) {
            error = e;
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, error]() {
            if (!guard) return;
            if (error.isEmpty()) {
                guard->fire_state_label_->setText(QStringLiteral("状态：连续出光已停止"));
                guard->fire_detail_label_->setText(QStringLiteral("光闸已关、已停机，读回确认通过。"));
                guard->log_line(Level::Ok, QStringLiteral("连续出光已停止：收光 + 停机 + 读回确认。"));
                guard->set_banner(QStringLiteral("连续出光已停止，光闸已关闭。"), 0);
            } else {
                // 停止没确认通过是最严重的一种失败 —— 必须把话说到人脸上。
                guard->fire_state_label_->setText(QStringLiteral("状态：⚠ 停止未确认"));
                guard->fire_detail_label_->setText(error);
                guard->log_line(Level::Danger, QStringLiteral("连续出光停止未确认：%1").arg(error));
                guard->set_banner(QStringLiteral("⚠ 停止出光未确认：%1 —— 请现场人工确认光闸已关闭").arg(error), 2);
                QMessageBox::critical(guard.data(), QStringLiteral("停止出光未确认"),
                                      QStringLiteral("已下发收光与停机，但读回确认没通过：\n\n%1\n\n"
                                                      "请到设备前人工确认光闸确实已关闭。").arg(error));
            }
            // 无论成败都把按钮放回去：失败时操作员更需要能再按一次。
            guard->stop_run_btn_->setEnabled(true);
            if (error.isEmpty()) {
                const bool reverse = guard->trigger_direction_ &&
                    guard->trigger_direction_->currentData().toString() ==
                        QLatin1String("spectrometer_to_laser");
                guard->run_btn_->setEnabled(guard->fire_allowed_ && !reverse);
            }
            if (guard->trigger_direction_) guard->trigger_direction_->setEnabled(true);
            if (guard->state_poller_) guard->state_poller_->request_refresh();
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

// ── 参数提交 ──────────────────────────────────────────────────────────

void MainWindow::apply_group(const QString& group, const ParamMap& fields, StatusDot* status) {
    QJsonObject values;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        if (!it.value()) continue;
        values.insert(it.key(), it.value()->value());
    }
    if (values.isEmpty()) return;

    // 高功率区二次确认：30 A 以上是设计文档里明确的"高危参数"。
    const QJsonValue cur = values.value(QLatin1String("discharge_current"));
    if (cur.isDouble() && cur.toDouble() > 30.0) {
        const auto r = QMessageBox::warning(
            this, QStringLiteral("高功率参数确认"),
            QStringLiteral("放电电流将设为 %1 A，属于高功率区（>30 A）。\n\n"
                           "请确认现场已做好防护、同步与排风检查。")
                .arg(fmt_fixed(cur.toDouble(), 1)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) {
            log_line(Level::Warn, QStringLiteral("用户取消了高功率电流设置。"));
            return;
        }
    }

    status->set_state(Level::Warn, QStringLiteral("下发中…"), QStringLiteral("等待业务层回执"));
    try {
        const QJsonObject r = client_.command(
            QStringLiteral("param_set"),
            QJsonObject{{QStringLiteral("group"), group},
                        {QStringLiteral("values"), values},
                        {QStringLiteral("timing"), QStringLiteral("immediately")}});
        const QJsonObject payload = r.value("payload").toObject();
        // 契约：completed=false 表示"收到请求但未完成"。**不能当成功**（任务书 §4.2）。
        const bool completed = payload.value("completed").toBool(false);
        const QString message = payload.value("message").toString();
        if (completed) {
            status->set_state(Level::Ok, QStringLiteral("已接受"), message);
            log_line(Level::Ok, QStringLiteral("参数 %1 已接受：%2").arg(group, message));
        } else {
            status->set_state(Level::Danger, QStringLiteral("未完成"), message);
            log_line(Level::Danger, QStringLiteral("参数 %1 未完成：%2").arg(group, message));
        }
    } catch (const QString& e) {
        status->set_state(Level::Danger, QStringLiteral("失败"), e);
        log_line(Level::Danger, QStringLiteral("参数 %1 提交失败：%2").arg(group, e));
    }
    if (state_poller_) state_poller_->request_refresh();
}
void MainWindow::on_apply_laser() { apply_group(QStringLiteral("laser"), laser_fields_, laser_apply_dot_); }
void MainWindow::on_apply_spectrometer() { apply_group(QStringLiteral("spectrometer"), spec_fields_, spec_apply_dot_); }

void MainWindow::sync_params_from_snapshot(const MachineSnapshot& s) {
    // 输入框已经带着**本机实测标定值**（FieldSpec::def）打开；这里做的是
    // "后端真值可用时覆盖它"。取不到就保留标定值，并把状态写清楚是
    // "没取到"而不是"还没设置"——两者的排障方向完全不同。
    struct Group { const char* name; ParamMap* fields; Card* card; StatusDot* dot; };
    const Group groups[] = {
        {"laser", &laser_fields_, laser_card_, laser_apply_dot_},
        {"spectrometer", &spec_fields_, spec_card_, spec_apply_dot_},
    };

    for (const Group& g : groups) {
        const QJsonObject gj = s.param_values.value(QLatin1String(g.name)).toObject();
        const QJsonObject vals = gj.value("values").toObject();
        if (vals.isEmpty()) {
            // 这组一个字段都没取到：保持禁用，并且把状态说清楚是"没取到"
            // 而不是"还没设置"——两者的排障方向完全不同。
            g.dot->set_state(g.dot->level() == Level::Ok ? Level::Ok : Level::Idle,
                             QStringLiteral("未取到"), QStringLiteral("param_get 未返回该组"));
            continue;
        }
        bool any = false;
        for (auto it = g.fields->begin(); it != g.fields->end(); ++it) {
            const QJsonValue v = vals.value(it.key());
            if (!v.isDouble() || !it.value()) continue;
            // 状态轮询每 800 ms 一次。用户正在编辑时不能被后台真值抢写，
            // 否则输入框会跳回旧值，现场看起来像“键盘坏了”。失焦后下一轮
            // 再同步设备真值。
            if (it.value()->hasFocus()) continue;
            it.value()->setValue(v.toDouble());
            it.value()->setEnabled(true);
            any = true;
        }
        if (!any) continue;

        // 组齐备性（complete）由后端算，UI 只显示，不自己推断。
        const bool complete = gj.value("complete").toBool(false);
        if (complete) {
            g.dot->set_state(Level::Ok, QStringLiteral("齐备"));
        } else if (g.dot->level() == Level::Ok) {
            // 别把刚成功的"已接受"覆盖成"不完整"：齐备性和"这次提交成没成"
            // 是两件事。
        } else {
            g.dot->set_state(Level::Warn, QStringLiteral("不完整"));
        }
        if (auto* btn = g.card->property("applyButton").value<QPushButton*>()) btn->setEnabled(true);
    }
}
// ── 状态快照渲染 ──────────────────────────────────────────────────────

void MainWindow::on_snapshot() { render_snapshot(state_poller_->snapshot()); }

void MainWindow::render_snapshot(const MachineSnapshot& s) {
    // 三态第一层：管道都不通，就明说"数据已过期"，而不是把上一帧当成现在。
    if (!s.valid) {
        daemon_dot_->set_state(Level::Danger, QStringLiteral("守护进程 未连接"),
                               s.error.isEmpty() ? client_.pipe_name()
                                                 : s.error);
        daemon_dot_->set_pulsing(false);
        set_banner(QStringLiteral("⚠ 与守护进程通信中断，画面数据可能已过期：%1")
                       .arg(s.error.isEmpty() ? QStringLiteral("(无错误信息)") : s.error),
                   2);
        update_offline_overlay();
        return;
    }
    daemon_dot_->set_state(Level::Ok, QStringLiteral("守护进程 已连接"), client_.pipe_name());
    daemon_dot_->set_pulsing(false);

    render_gate(s);
    render_detection(s);
    render_aux(s);
    render_devices(s);
    sync_params_from_snapshot(s);
    log_snapshot_delta(s);
    update_offline_overlay();
    prev_snapshot_ = s;
    have_prev_snapshot_ = true;
}

void MainWindow::render_gate(const MachineSnapshot& s) {
    const GateState& g = s.gate;
    // 出光按钮的可用性**只**由门禁决定。UI 不自己判"能不能出光"，
    // 也不做任何"我觉得这次应该行"的放宽 —— 门禁是唯一判定入口。
    fire_allowed_ = g.allowed;
    if (fire_btn_ && !firing_) fire_btn_->setEnabled(g.allowed);
    // 连续出光：**开始**看门禁；**停止**永远可点（见下面 build 里的注释与
    // on_stop_continuous —— 停止不该有任何前置条件）。
    const bool reverse_trigger = trigger_direction_ &&
        trigger_direction_->currentData().toString() == QLatin1String("spectrometer_to_laser");
    if (run_btn_ && !firing_)
        run_btn_->setEnabled(g.allowed && !s.laser_running && !reverse_trigger);
    if (stop_run_btn_ && !firing_) stop_run_btn_->setEnabled(true);
    Level lvl;
    QString detail = g.detail;
    if (g.state == QLatin1String("fault")) {
        lvl = Level::Danger;
    } else if (g.allowed) {
        lvl = Level::Ok;
    } else if (!g.parameters_ready) {
        lvl = Level::Warn;
    } else {
        lvl = Level::Idle;
    }
    gate_dot_->set_state(lvl, QStringLiteral("门禁 %1").arg(humanize_gate_state(g.state)), detail);

    QString text;
    QString color;
    if (g.allowed) {
        text = QStringLiteral("允许出光");
        color = theme::color::ok().name();
    } else if (g.blocked_by.isEmpty()) {
        text = QStringLiteral("禁止出光");
        color = theme::color::danger().name();
    } else {
        // 逐条翻译，而不是 join 原始码；药丸只有这么宽，码多的时候只显示前两条，
        // 剩下的挂 tooltip —— 全塞进去会把顶栏挤变形。
        QStringList reasons;
        for (const QString& code : g.blocked_by) reasons << humanize_gate_reason(code);
        const int shown = 2;
        text = reasons.size() > shown
                   ? QStringLiteral("禁止：%1 等 %2 项")
                         .arg(reasons.mid(0, shown).join(QStringLiteral("、")))
                         .arg(reasons.size())
                   : QStringLiteral("禁止：%1").arg(reasons.join(QStringLiteral("、")));
        color = theme::color::danger().name();
        gate_pill_->setToolTip(reasons.join("\n"));
    }
    gate_pill_->setText(text);
    gate_pill_->setStyleSheet(QStringLiteral(
        "QLabel{background-color:%1;color:#0E1319;font-weight:700;font-size:13px;"
        "border-radius:11px;padding:4px 14px;}").arg(color));
}

void MainWindow::render_detection(const MachineSnapshot& s) {
    const bool running = (s.running == QLatin1String("true"));
    run_dot_->set_state(running ? Level::Ok : Level::Idle,
                        running ? QStringLiteral("运行中") : QStringLiteral("待机"));
    run_state_label_->setText(QStringLiteral("状态：%1").arg(running ? QStringLiteral("运行")
                                                                     : QStringLiteral("停止")));
    stage_label_->setText(QStringLiteral("阶段：%1").arg(detect_stage_text(s.stage)));
    belt_label_->setText(QStringLiteral("目标带：%1")
                             .arg(s.target_belt.isEmpty() ? QStringLiteral("—")
                                                           : s.target_belt));
    seq_label_->setText(QStringLiteral("带序号：%1")
                            .arg(s.belt_sequence < 0 ? QStringLiteral("—")
                                                      : QString::number(s.belt_sequence)));
    count_label_->setText(QStringLiteral("日志条数：%1").arg(s.log_records));
    // detection_start 与 spectrum_stream 是两条独立协议。即使后端的
    // running 尚未切换，预览订阅已经打开时也不能再次点“启动检测”，
    // 否则会不断重订阅、界面状态与后端状态不一致。
    const bool active = running || (poller_ && poller_->is_open());
    start_btn_->setEnabled(!active);
    stop_btn_->setEnabled(active);
}

QString MainWindow::detect_stage_text(const QString& stage) const {
    if (stage == QLatin1String("idle"))          return QStringLiteral("空闲");
    if (stage == QLatin1String("selected_belt")) return QStringLiteral("已选带");
    if (stage == QLatin1String("approaching"))   return QStringLiteral("接近");
    if (stage == QLatin1String("measuring"))     return QStringLiteral("测量中");
    if (stage == QLatin1String("evaluating"))    return QStringLiteral("判定中");
    if (stage == QLatin1String("reporting"))     return QStringLiteral("上报中");
    if (stage == QLatin1String("done"))          return QStringLiteral("完成");
    return stage.isEmpty() ? QStringLiteral("—") : stage;
}

void MainWindow::render_aux(const MachineSnapshot& s) {
    // ── 测距 / 有矿 ─────────────────────────────────────────────────
    // distance_raw 是 optional：通信失败时后端会带占位 0，但 UI 不把它
    // 当成真实读数。raw 尚未完成工程单位标定，因此单位保持 raw。
    if (s.ore.distance_raw.has_value()) {
        const Level level = (s.ore.device_valid && s.ore.in_range) ? Level::Ok : Level::Warn;
        dist_metric_->set_value(fmt_fixed(*s.ore.distance_raw, 0), level);
        dist_metric_->set_unit(QStringLiteral("raw"));
    } else {
        const QString why = !s.ore.read_error.isEmpty()
                                ? QStringLiteral("测距读取失败：%1").arg(s.ore.read_error)
                                : (s.ore.has_observation
                                       ? QStringLiteral("测距无有效 raw 读数")
                                       : QStringLiteral("暂无测距观测"));
        dist_metric_->set_unavailable(why);
    }
    count_metric_->set_unit(QString());
    if (s.ore.decision_known && s.ore.ore_present.has_value()) {
        count_metric_->set_value(*s.ore.ore_present ? QStringLiteral("有矿")
                                                     : QStringLiteral("无矿"),
                                 *s.ore.ore_present ? Level::Ok : Level::Idle);
    } else {
        QString why;
        if (!s.ore.has_observation) {
            why = QStringLiteral("未观测，判定未知（这不等于「无矿」）");
        } else if (!s.ore.read_ok) {
            why = QStringLiteral("测距通信失败，判定未知");
        } else if (!s.ore.device_valid) {
            why = QStringLiteral("距离 raw 无效，判定未知");
        } else if (!s.ore.in_range) {
            why = QStringLiteral("距离 raw 超出量程，判定未知");
        } else {
            why = QStringLiteral("判定规则未标定，判定未知");
        }
        count_metric_->set_unavailable(why);
    }

    QStringList ore_detail;
    if (s.ore.confidence_raw.has_value()) {
        ore_detail.push_back(QStringLiteral("confidence raw=%1")
                                 .arg(fmt_fixed(*s.ore.confidence_raw, 0)));
    }
    if (s.ore.distance_register.has_value()) {
        ore_detail.push_back(QStringLiteral("距离寄存器 0x%1")
                                 .arg(QString::number(*s.ore.distance_register, 16).toUpper()));
    }
    if (s.ore.distance_resolution_mm.has_value() && *s.ore.distance_resolution_mm > 0.0) {
        ore_detail.push_back(QStringLiteral("分辨率 %1 mm/LSB")
                                 .arg(fmt_fixed(*s.ore.distance_resolution_mm, 1)));
    }
    if (s.ore.has_observation && s.ore.read_age_ms >= 0) {
        ore_detail.push_back(QStringLiteral("读数%1 ms前").arg(s.ore.read_age_ms));
    }
    if (s.ore.read_ok && (!s.ore.device_valid || !s.ore.in_range)) {
        ore_detail.push_back(QStringLiteral("raw无效或量程外"));
    }
    if (!s.ore.note.isEmpty()) ore_detail.push_back(s.ore.note);
    if (!s.ore.read_error.isEmpty()) ore_detail.push_back(s.ore.read_error);
    const QString ore_detail_text = ore_detail.join(QStringLiteral("；"));
    ore_dot_->set_state(s.ore.decision_known
                            ? (s.ore.ore_present.value_or(false) ? Level::Ok : Level::Idle)
                            : Level::Warn,
                        s.ore.decision_known
                            ? (s.ore.ore_present.value_or(false) ? QStringLiteral("有矿 已判定")
                                                                 : QStringLiteral("无矿 已判定"))
                            : QStringLiteral("有矿 判定未知"),
                        s.ore.basis.isEmpty() ? ore_detail_text
                                              : s.ore.basis + (ore_detail_text.isEmpty()
                                                                   ? QString()
                                                                   : QStringLiteral("；") + ore_detail_text));

    // ── 空调 ───────────────────────────────────────────────────────
    if (s.ac.indoor_temp_c.has_value()) {
        const double t = *s.ac.indoor_temp_c;
        Level lvl = Level::Ok;
        if (t > 28.0) lvl = Level::Warn;   // 设计方案 §7.1：>28℃ 提示过热
        temp_metric_->set_value(fmt_fixed(t, 1), lvl);
        temp_metric_->set_unit(QStringLiteral("°C"));
    } else {
        temp_metric_->set_unavailable(s.ac.read_error.isEmpty()
                                          ? QStringLiteral("空调数据不可用")
                                          : s.ac.read_error);
    }
    Level ac_lvl = Level::Idle;
    QString ac_text = QStringLiteral("空调 未接入");
    if (s.ac.availability == QLatin1String("available")) {
        ac_lvl = s.ac.active_alarms.isEmpty() ? Level::Ok : Level::Danger;
        ac_text = QStringLiteral("空调 %1%2")
                      .arg(s.ac.mode.isEmpty() ? QStringLiteral("运行") : s.ac.mode,
                           s.ac.active_alarms.isEmpty()
                               ? QString()
                               : QStringLiteral(" 报警：%1").arg(s.ac.active_alarms.join(QStringLiteral("、"))));
    } else if (s.ac.availability == QLatin1String("stale")) {
        ac_lvl = Level::Warn;
        ac_text = QStringLiteral("空调 数据过期");
    }
    ac_dot_->set_state(ac_lvl, ac_text, s.ac.note);
    ac_dot_->set_pulsing(ac_lvl == Level::Warn);

    // ── 运动 ────────────────────────────────────────────────────────
    if (s.motion.forward_time_s.has_value()) {
        motion_metric_->set_value(fmt_fixed(*s.motion.forward_time_s, 3), Level::Ok);
    } else {
        motion_metric_->set_unavailable(QStringLiteral("未标定"));
    }
    motion_dot_->set_state(s.motion.calibrated ? Level::Ok : Level::Warn,
                           s.motion.calibrated ? QStringLiteral("运动 已标定")
                                               : QStringLiteral("运动 未标定"),
                           s.motion.note);
}

void MainWindow::render_devices(const MachineSnapshot& s) {
    QString html = QStringLiteral("<span style='color:#93A3B2'>设备：</span>");
    for (const DeviceHealth& d : s.devices) {
        QString icon;
        QString color;
        if (!d.configured) {
            icon = QStringLiteral("○");   // 灰圈：没接这台设备，属于正常
            color = theme::color::idle().name();
        } else if (d.opened) {
            icon = QStringLiteral("●");   // 实心绿：已连接
            color = theme::color::ok().name();
        } else {
            icon = QStringLiteral("▲");   // 三角红：配了但打不开，真故障
            color = theme::color::danger().name();
        }
        const QString tip = d.note.isEmpty() ? d.endpoint : (d.endpoint + " " + d.note);
        html += QStringLiteral("<span style='color:%1'>%2 %3</span>&nbsp;&nbsp;&nbsp;")
                    .arg(color, icon, d.name);
        if (!tip.isEmpty()) {
            html += QStringLiteral("<span style='color:#647280'>%1</span>&nbsp;&nbsp;")
                        .arg(tip.toHtmlEscaped());
        }
    }
    device_strip_->setText(html);
}

void MainWindow::log_snapshot_delta(const MachineSnapshot& s) {
    if (have_prev_snapshot_) {
        if (s.gate.allowed != prev_snapshot_.gate.allowed) {
            log_line(s.gate.allowed ? Level::Ok : Level::Warn,
                     s.gate.allowed ? QStringLiteral("门禁转为：允许出光")
                                    : QStringLiteral("门禁转为：禁止出光（%1 项，详见顶栏药丸）")
                                          .arg(s.gate.blocked_by.size()));
        }
        for (const DeviceHealth& d : s.devices) {
            const DeviceHealth* old = prev_snapshot_.device(d.name);
            if (old && old->opened != d.opened) {
                log_line(d.opened ? Level::Ok : Level::Danger,
                         QStringLiteral("%1 %2").arg(d.name,
                                                     d.opened ? QStringLiteral("已连接")
                                                              : QStringLiteral("断开")));
            }
        }
    }
    // 空调报警只在出现/消失的瞬间记一条，不逐帧刷屏。
    if (have_prev_snapshot_ && s.ac.active_alarms != prev_snapshot_.ac.active_alarms) {
        for (const QString& a : s.ac.active_alarms) {
            if (!prev_snapshot_.ac.active_alarms.contains(a)) {
                log_line(Level::Danger, QStringLiteral("空调报警：%1").arg(a));
            }
        }
    }
}

// ── 日志 ──────────────────────────────────────────────────────────────

void MainWindow::log_line(Level level, const QString& text) {
    if (!log_view_) return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString tag = QStringLiteral("[%1] %2").arg(stamp, text);
    auto* item = new QListWidgetItem(tag);
    item->setForeground(color_for(level));
    log_view_->addItem(item);
    // 保留最近 1000 条：超出就丢最旧的。QListWidget 不自带这个上限。
    while (log_view_->count() > 1000) delete log_view_->takeItem(0);
    log_view_->scrollToBottom();
}

void MainWindow::on_export_log() {
    if (log_view_->count() == 0) {
        QMessageBox::information(this, QStringLiteral("导出日志"),
                                 QStringLiteral("日志为空，无可导出内容。"));
        return;
    }
    const QString path = QStringLiteral("D:/LIBS_Data/logs/ui-%1.log")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入：%1").arg(path));
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    for (int i = 0; i < log_view_->count(); ++i) ts << log_view_->item(i)->text() << "\n";
    f.close();
    log_line(Level::Ok, QStringLiteral("日志已导出：%1").arg(path));
    QMessageBox::information(this, QStringLiteral("导出完成"), path);
}

void MainWindow::on_clear_log() {
    if (log_view_->count() == 0) return;
    const auto r = QMessageBox::question(
        this, QStringLiteral("清空日志"),
        QStringLiteral("确定清空当前 %1 条界面日志？\n\n只清界面显示，不影响守护进程的日志库。")
            .arg(log_view_->count()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r == QMessageBox::Yes) {
        log_view_->clear();
        log_line(Level::Idle, QStringLiteral("界面日志已清空。"));
    }
}

// ── 截图 ──────────────────────────────────────────────────────────────

void MainWindow::on_screenshot() {
    if (!plot_ || plot_->grab().isNull()) {
        QMessageBox::information(this, QStringLiteral("截图"),
                                 QStringLiteral("当前没有可保存的光谱曲线。"));
        return;
    }
    const QString dir = QStringLiteral("D:/LIBS_Data/Snapshots");
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, QStringLiteral("截图"),
                             QStringLiteral("无法创建目录：%1").arg(dir));
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString png = QStringLiteral("%1/spectrum-%2.png").arg(dir, stamp);
    const QString csv = QStringLiteral("%1/spectrum-%2.csv").arg(dir, stamp);

    // PNG：抓整个 chart 视图。grab() 走的是实际渲染结果，跟屏幕所见一致。
    if (!plot_->grab().save(png)) {
        QMessageBox::warning(this, QStringLiteral("截图"), QStringLiteral("PNG 保存失败。"));
        return;
    }

    // CSV：导**原始计数**，不是图上归一化后的值。归一化只是显示口径，
    // 拿它去标定/建模会得到错的数据，所以这里从最近一帧的原始通道取。
    QFile f(csv);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << "# frame_index,shot_id,wall_clock_ms,hardware_synchronized\n";
        ts << last_frame_.frame_index << "," << last_frame_.shot_id << ","
           << last_frame_.wall_clock_ms << ","
           << (last_frame_.hardware_synchronized ? "true" : "false") << "\n";
        for (const ChannelData& c : last_frame_.channels) {
            if (!c.present) {
                ts << "# channel " << c.serial << " absent: " << c.reason << "\n";
                continue;
            }
            ts << "# channel " << c.serial << " peak_counts=" << c.peak_counts() << "\n";
            ts << "wavelength_nm,counts\n";
            const int n = std::min<int>(c.wavelength_nm.size(), c.counts.size());
            for (int i = 0; i < n; ++i) {
                ts << c.wavelength_nm[i] << "," << c.counts[i] << "\n";
            }
        }
        f.close();
    }
    log_line(Level::Ok, QStringLiteral("截图已保存：%1 / %2").arg(png, csv));
    QMessageBox::information(this, QStringLiteral("截图完成"), QStringLiteral("%1\n%2").arg(png, csv));
}


// ── 遮罩 / 标题 / 关闭 ────────────────────────────────────────────────

void MainWindow::update_offline_overlay() {
    if (!stack_) return;
    // 核心设备（激光/光谱）离线才拦操作。其它设备缺失只是灰圈，不挡。
    const MachineSnapshot s = state_poller_->snapshot();
    QStringList reasons;
    if (!s.valid) {
        reasons << QStringLiteral("与守护进程通信中断，请确认 businessd 正在运行");
    } else {
        for (const char* n : {"激光器", "光谱仪"}) {
            const DeviceHealth* d = s.device(QString::fromUtf8(n));
            if (d && d->configured && !d->opened) {
                reasons << QStringLiteral("%1 未就绪（%1）")
                             .arg(d->name, d->endpoint.isEmpty() ? QStringLiteral("端点未知")
                                                                 : d->endpoint);
            }
        }
    }

    if (reasons.isEmpty()) {
        stack_->setCurrentIndex(0);   // 回到正常页面
        return;
    }
    overlay_label_->setText(
        QStringLiteral("<div style='font-size:26px;font-weight:700;color:#F85149;'>设备离线</div>"
                       "<div style='font-size:14px;color:#93A3B2;margin-top:10px;'>%1<br/>"
                       "请检查设备连接，确认后再继续操作。</div>")
            .arg(reasons.join(QStringLiteral("<br/>"))));
    stack_->setCurrentWidget(overlay_);   // 切到遮罩页：按钮真的点不动
}

void MainWindow::tick_clock() {
    clock_->setText(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

void MainWindow::set_banner(const QString& text, int severity) {
    banner_->setText(text);
    const QString bg = (severity == 2) ? QStringLiteral("#3A1D1D")
                        : (severity == 1) ? QStringLiteral("#3A3220")
                                           : QStringLiteral("#16281C");
    const QString fg = (severity == 0) ? QStringLiteral("#A6E3B0")
                        : (severity == 1) ? QStringLiteral("#F0D9A0")
                                           : QStringLiteral("#FFB4AE");
    banner_->setStyleSheet(QStringLiteral(
        "QLabel{background-color:%1;color:%2;border:1px solid %3;border-radius:7px;"
        "font-size:13px;padding:9px 12px;}").arg(bg, fg, fg));
}

void MainWindow::on_auto_y_toggled(bool on) {
    auto_y_on_ = on;
    if (plot_) plot_->set_auto_y(on);
}

void MainWindow::on_lock_y_toggled(bool on) {
    lock_y_on_ = on;
    if (plot_) plot_->set_lock_y(on);
    if (on && plot_) locked_y_hi_ = plot_->y_max();
}

void MainWindow::on_pause_toggled(bool on) {
    paused_ = on;
    log_line(Level::Idle, on ? QStringLiteral("绘制已暂停（数据流仍在后台采集）。")
                             : QStringLiteral("绘制已恢复。"));
}

void MainWindow::on_trigger_direction_changed(int index) {
    Q_UNUSED(index)
    if (!trigger_direction_) return;

    const QString direction = trigger_direction_->currentData().toString();
    const bool reverse = direction == QLatin1String("spectrometer_to_laser");

    // 连续出光没有光谱仪的逐发 AVS_Measure 作为触发源，反向模式只能走
    // laser_fire 有界测量；后端也会拒绝 laser_run，前端提前禁用避免误操作。
    if (run_btn_ && !firing_) run_btn_->setEnabled(!reverse);

    // “跟随激光触发”只适用于激光器输出 → 光谱仪 Pin 6 的预览。
    // 反向模式由光谱仪的 LASER OUT 产生触发，继续让预览等待 Pin 6
    // 会造成看似“没有光谱”的假故障，因此自动关闭并锁定该复选框。
    if (follow_trigger_) {
        if (reverse && follow_trigger_->isChecked()) {
            const QSignalBlocker blocker(follow_trigger_);
            follow_trigger_->setChecked(false);
        }
        follow_trigger_->setEnabled(!reverse);
        follow_trigger_->setToolTip(reverse
            ? QStringLiteral("反向模式使用光谱仪 LASER OUT 触发激光器；预览不能等待 Pin 6，"
                             "请使用出光测量查看逐发光谱。")
            : QStringLiteral("勾上后光谱仪等激光的 pin6 TTL，每发激光采一帧。\n"
                             "连续出光时能看到干净的 LIBS 谱；不出光时会一直等不到数据（正常）。\n"
                             "不勾 = 自由触发，看背景谱。"));
    }

    if (trigger_path_label_) {
        trigger_path_label_->setText(reverse
            ? QStringLiteral("预期触发路径：Avantes 主通道 Pin 23 LASER OUT → 激光器 CLK IN。"
                             "请确认现场电平、极性和接地后再预检。")
            : QStringLiteral("预期触发路径：激光器 Q OUT → Avantes 主通道 Pin 6；"
                             "四通道由主通道内部 SYNC 同步。"));
    }
    if (!firing_ && fire_state_label_) {
        fire_state_label_->setText(reverse
            ? QStringLiteral("状态：已选择反向触发，等待预检")
            : QStringLiteral("状态：已选择激光器触发光谱仪，等待预检"));
    }
    if (!firing_) {
        log_line(Level::Idle, reverse
            ? QStringLiteral("联合触发方向切换为：光谱仪触发激光器。")
            : QStringLiteral("联合触发方向切换为：激光器触发光谱仪。"));
    }
}


void MainWindow::closeEvent(QCloseEvent* event) {
    if (state_poller_) state_poller_->stop();
    if (poller_ && poller_->is_open()) stop_stream();
    event->accept();
}

}  // namespace libs::ui




