#pragma once

// 主窗口（`ui/src/main_window.hpp`）。
//
// 布局遵循《LIBS 激光固废筛查仪检测系统 UI 设计方案》的"左控 + 右监 + 底态"
// 工控骨架，但**不照抄**：方案里 §5/§6 的参数表（0~50 A、10 ns、200 Hz、
// 增益、能量、通道勾选）与真实硬件协议/后端命令不符，这里一律以
// `device-references/laser/air_cooled_laser_rs232_protocol.txt` 与
// `param_get` 实际暴露的分组为准。**不造写不出去的控件**——一个下拉框
// 点了没反应，比没有这个下拉框更伤现场。
//
// 绘制纪律（来自业务层的三态语义，不是本文件的发明）：
//   1. 通道 absent → **不画线**，在图例旁写"缺席：<原因>"。补零画平线
//      会让"没接"看起来像"信号为 0"，这是排障时最坑的一种误导。
//   2. 整次采集失败（ok=false）→ 保留上一帧曲线但**置灰 + 红色横幅**
//      写清错误码，不静默停在旧数据上让人以为是新数据。
//   3. 永久免责声明：自由触发预览拍到的是**背景谱**，不是 LIBS 测量谱。
//   4. UI **不做安全判定**。启动/出光许可全部以 `gate.allowed` 为准，
//      UI 只把 allowed/blocked_by 翻译成人话。

#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QVector>

#include "machine_state.hpp"
#include "spectrum_plot.hpp"
#include "status_poller.hpp"
#include "stream_poller.hpp"
#include "widgets.hpp"

class QChart;
class QChartView;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineSeries;
class QListWidget;
class QPushButton;
class QCheckBox;
class QStackedLayout;
class QTimer;
class QValueAxis;

namespace libs::ui {

class DebugWindow;


class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // ── 光谱流 ───────────────────────────────────────────────────────
    void start_stream();
    void stop_stream();
    void on_frame(const libs::ui::FrameData& frame);
    void on_capture_failed(const QString& code, const QString& message);
    void on_stream_error(const QString& message);
    void on_stats(int pending, qint64 dropped);

    // ── 状态轮询 ─────────────────────────────────────────────────────
    void on_snapshot();
    void tick_clock();

    // ── 操作 ─────────────────────────────────────────────────────────
    void on_start_detection();
    void on_stop_detection();
    // LIBS 测量（真出光）。出光是物理动作，流程里**必须**有操作员确认这一步：
    // 业务层刻意不做确认（那是现场的责任），所以确认的落点就是这两个槽。
    void on_precheck_fire();
    void on_fire_measure();
    void on_run_continuous();
    void on_stop_continuous();
    void on_fire_result(const QJsonObject& payload);
    void on_fire_failed(const QString& message);
    void on_apply_laser();
    void on_apply_spectrometer();
    void on_screenshot();
    void on_export_log();
    void on_clear_log();
    void on_auto_y_toggled(bool on);
    void on_lock_y_toggled(bool on);
    void on_pause_toggled(bool on);
    void on_trigger_direction_changed(int index);

private:
    // ── 布局构建 ─────────────────────────────────────────────────────
    void build_header(QWidget* host);
    QWidget* build_left_column();
    QWidget* build_right_column();
    QWidget* build_bottom_bar(QWidget* host);
    void build_chart();

    // ── 参数表单 ─────────────────────────────────────────────────────
    // 键是 param_set 的字段名（laser.discharge_current 等），
    // 值是对应的自旋框。分组和范围都来自 FieldSpec（真实协议），不是文档。
    using ParamMap = QHash<QString, QDoubleSpinBox*>;
    void build_param_card(Card* card, const QString& group, ParamMap& fields);
    void sync_params_from_snapshot(const MachineSnapshot& s);
    void apply_group(const QString& group, const ParamMap& fields, StatusDot* status);
    ParamMap laser_fields_;
    ParamMap spec_fields_;

    // ── 状态渲染 ─────────────────────────────────────────────────────
    void render_snapshot(const MachineSnapshot& s);
    void render_gate(const MachineSnapshot& s);
    void render_devices(const MachineSnapshot& s);
    void render_aux(const MachineSnapshot& s);
    void render_detection(const MachineSnapshot& s);
    void render_chart_frame_info(const FrameData& f);
    void update_offline_overlay();

    // 把界面上当前这几个参数框的值，按 FieldSpec::over 的键名收集成
    // laser_fire / laser_run 的 args。**两个出光动作共用**：一处改了另一处
    // 没改，"界面能调、实际不生效"就是这么来的。
    QJsonObject current_fire_args() const;

    // ── 日志 ─────────────────────────────────────────────────────────
    void log_line(Level level, const QString& text);
    void log_snapshot_delta(const MachineSnapshot& s);

    // ── 工具 ─────────────────────────────────────────────────────────
    void set_banner(const QString& text, int severity);  // 0=信息 1=警告 2=错误
    void rebuild_series(const QVector<ChannelData>& channels);
    void closeEvent(QCloseEvent* event) override;  // 关窗前退订，别把订阅号悬着
    QString disconnect_banner_text() const;
    // 预览横幅。**它讲的是"这条谱是什么"，不是许可状态** —— 门禁管的是
    // 出光动作，预览根本不经过门禁。文案随触发模式变：自由触发拍到的是
    // 环境/杂散光，跟随触发拍到的才是逐发的测量谱。
    // 每帧都会重设这条横幅（见 render_chart_frame_info），所以不能各调用点
    // 各写一套，否则总有一处把别的覆盖掉。
    QString preview_provenance_text() const;
    QString detect_stage_text(const QString& stage) const;
    QString fmt_bool_zh(bool b) const { return b ? QStringLiteral("是") : QStringLiteral("否"); }

    PipeClient client_;
    QPointer<DebugWindow> debug_window_;
    StreamPoller* poller_ = nullptr;
    StatusPoller* state_poller_ = nullptr;

    // ── 顶栏 ─────────────────────────────────────────────────────────
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* clock_ = nullptr;
    StatusDot* daemon_dot_ = nullptr;
    StatusDot* gate_dot_ = nullptr;
    QLabel* gate_pill_ = nullptr;
    QLabel* banner_ = nullptr;

    // ── 检测控制 ─────────────────────────────────────────────────────
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;

    // ── LIBS 测量（真出光，走 laser_fire）────────────────────────────
    // 与"启动检测"彻底分开：那个只发业务信号，这个会真的让激光出光。
    // 出光按钮的可用性**只**由 gate.allowed 决定（UI 不自己判安全）。
    QPushButton* precheck_btn_ = nullptr;
    QPushButton* fire_btn_ = nullptr;
    // 连续出光：一直打，直到点停止。与"出光测量"是两个动作，按钮也分开。
    QPushButton* run_btn_ = nullptr;
    QPushButton* stop_run_btn_ = nullptr;
    // 联合触发方向：默认保持现场已经验证的“激光器触发光谱仪”。
    // 反向模式由后端 laser_fire 的 trigger_direction 字段执行，
    // UI 只负责选择、显示和把值原样传递。
    QComboBox* trigger_direction_ = nullptr;
    QLabel* trigger_path_label_ = nullptr;
    QLabel* fire_state_label_ = nullptr;
    QLabel* fire_detail_label_ = nullptr;
    QLabel* fire_path_label_ = nullptr;
    bool firing_ = false;
    bool fire_allowed_ = false;
    StatusDot* run_dot_ = nullptr;
    QLabel* stage_label_ = nullptr;
    QLabel* run_state_label_ = nullptr;
    QLabel* belt_label_ = nullptr;
    QLabel* seq_label_ = nullptr;
    QLabel* count_label_ = nullptr;

    // ── 光谱 ─────────────────────────────────────────────────────────
    // 自绘光谱图。**刻意不用 QtCharts**：QLineSeries 每帧要把点拷进控件，
    // 四路 2048 点就是 8192 次拷贝，显示端跟不上采集。详见 spectrum_plot.hpp。
    SpectrumPlot* plot_ = nullptr;
    QLabel* frame_label_ = nullptr;
    QLabel* absent_label_ = nullptr;
    QCheckBox* auto_y_ = nullptr;
    QCheckBox* lock_y_ = nullptr;
    QCheckBox* pause_ = nullptr;
    // 预览是否跟着激光的 pin6 TTL 采（见 build 里的注释）。
    QCheckBox* follow_trigger_ = nullptr;
    bool paused_ = false;
    bool auto_y_on_ = true;
    bool lock_y_on_ = false;
    double locked_y_hi_ = 1.0;

    // ── 辅助监控 ─────────────────────────────────────────────────────
    Metric* dist_metric_ = nullptr;
    Metric* temp_metric_ = nullptr;
    Metric* motion_metric_ = nullptr;
    Metric* count_metric_ = nullptr;
    StatusDot* ac_dot_ = nullptr;
    StatusDot* motion_dot_ = nullptr;
    StatusDot* ore_dot_ = nullptr;
    Card* composition_card_ = nullptr;

    // ── 设备条 / 日志 ───────────────────────────────────────────────
    QLabel* device_strip_ = nullptr;
    QListWidget* log_view_ = nullptr;
    qint64 last_log_records_ = -1;
    bool have_prev_snapshot_ = false;
    MachineSnapshot prev_snapshot_;

    Card* laser_card_ = nullptr;
    Card* spec_card_ = nullptr;
    StatusDot* laser_apply_dot_ = nullptr;
    StatusDot* spec_apply_dot_ = nullptr;
    qint64 frame_seen_ = 0;
    FrameData last_frame_;   // 截图导 CSV 用：最近一帧的**原始**计数
    QWidget* central_ = nullptr;
    QWidget* overlay_ = nullptr;
    QStackedLayout* stack_ = nullptr;
    QLabel* overlay_label_ = nullptr;
};

}  // namespace libs::ui
