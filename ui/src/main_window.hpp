#pragma once

// 主窗口（`ui/src/main_window.hpp`）。
//
// ── 本文件对应后端重构后的真实业务面 ────────────────────────────────────
//
// 整个项目现在只有**两条**业务流程，界面就只做这两件事 + 参数配置：
//
//   业务一 设备预检（readiness）  -> 按钮「设备预检」
//   业务二 LIBS 全流程（measurement）-> 按钮「开始检测」/「停止检测」
//   参数管理（ParameterService）  -> 独立页面「测量参数」
//
// 旧版界面里还有的「连续出光」「出光测量」「空调」「测距」「传送带」
// 「选矿」「运动标定」，对应的后端服务都已经删了，命令面也一起删了
// （`laser_fire` / `laser_run` / `laser_stop` 三条命令**不存在**，
// 出光只剩 `measurement_start` 一条路径）。留着那些按钮只会得到
// 「unknown_command」—— 一个点了必失败的按钮比没有这个按钮更伤现场。
//
// ── 绘制纪律（来自业务层的三态语义，不是本文件的发明）─────────────────
//
//   1. 通道 absent -> **不画线**，在图例旁写"缺席：<原因>"。补零画平线
//      会让"没接"看起来像"信号为 0"，这是排障时最坑的一种误导。
//   2. 整次采集失败（ok=false）-> 保留上一帧曲线但**置灰 + 红色横幅**
//      写清错误码，不静默停在旧数据上让人以为是新数据。
//   3. 永久免责声明：图上是**落光射度谱**，但内存里有曲线不等于已落盘。
//   4. UI **不做安全判定**。绝不"先 gate 拿绿灯再 start"——
//      判定执行点只有 MeasurementJob::start() 一个。

#include <functional>

#include <QJsonObject>
#include <QMainWindow>
#include <QStringList>
#include <QString>
#include <QVector>

#include "machine_state.hpp"
#include "spectrum_plot.hpp"
#include "status_poller.hpp"
#include "prediction_poller.hpp"
#include "stream_poller.hpp"
#include "widgets.hpp"

class QCheckBox;
class QComboBox;
class QCloseEvent;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QThread;
class QTimer;

namespace libs::ui {

class ParameterPage;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    // ── 光谱显示 ─────────────────────────────────────────────────────
    // 订阅/退订光谱展示通道。**这里不启动采集**：
    // 采集的唯一来源是测量作业（`measurement_start`）。
    void start_stream();
    void stop_stream();
    void on_frame(const libs::ui::FrameData& frame);
    void on_capture_failed(const QString& code, const QString& message);
    void on_stream_error(const QString& message);
    void on_stats(int pending, qint64 dropped);

    // ── 状态轮询 ─────────────────────────────────────────────────────
    void on_snapshot();
    void tick_clock();

    // ── 三个按钮 ─────────────────────────────────────────────────────
    // ① 设备预检：只读检测。一条激光写指令都不发。
    void on_precheck();
    // ② 开始检测：发 `measurement_start`。**成功即已开光**，
    //    失败原因看返回码 —— 界面不预先判断，也不预先拦。
    void on_start_detection();
    // ③ 停止检测：发 `measurement_stop`。**不做确认弹窗** ——
    //    停止是安全方向的动作，让人点两次才能收光是危险设计。
    void on_stop_detection();

    // 预检的返回（跑在工作线程）。
    void on_precheck_result(const QJsonObject& payload, bool ok, const QString& message);
    // `measurement_start` 的返回。
    void on_start_result(const QJsonObject& payload, bool ok, const QString& message);
    // `measurement_stop` 的返回。
    void on_stop_result(bool ok, const QString& message);

    // ── 参数页 ───────────────────────────────────────────────────────
    void on_param_save_requested(const QJsonObject& request);
    void on_param_reload_requested();
    void on_param_result(bool ok, const QString& summary);
    void show_main_page();
    void show_parameter_page();

    void on_auto_y_toggled(bool on);
    void on_lock_y_toggled(bool on);
    void on_pause_toggled(bool on);
    void on_export_csv();

private:
    // ── 布局构建 ─────────────────────────────────────────────────────
    void build_header(QVBoxLayout* root);
    QWidget* build_main_page();
    void build_action_bar(QVBoxLayout* root);
    Card* chart_card_ = nullptr;
    void build_chart(QVBoxLayout* root);
    Card* build_prediction_card();
    void configure_prediction();
    void import_prediction_spectrum();
    void render_prediction(const QJsonObject& status);
    QComboBox* prediction_branch_ = nullptr;
    QPushButton* prediction_retry_ = nullptr;
    QPushButton* prediction_import_ = nullptr;
    QLabel* prediction_status_ = nullptr;
    QLabel* prediction_info_ = nullptr;
    QVector<QLabel*> prediction_values_;
    bool prediction_configuring_ = false;
    void build_device_bar(QVBoxLayout* root);

    // ── 状态渲染 ─────────────────────────────────────────────────────
    void render_snapshot(const MachineSnapshot& s);
    void render_gate(const MachineSnapshot& s);
    void render_devices(const MachineSnapshot& s);
    void render_measurement(const MachineSnapshot& s);
    void render_frame_info(const FrameData& f);
    // 按钮可用性。**唯一来源是后端状态**，不是本地推理。
    void render_buttons(const MachineSnapshot& s);

    // ── 工具 ─────────────────────────────────────────────────────────
    void set_banner(const QString& text, Level level);
    void set_banner_default();
    void rebuild_series(const FrameData& f);
    void closeEvent(QCloseEvent* event) override;

    // 在工作线程里发一条命令，回 UI 线程回调。
    // 所有耗时命令都走它：管道往返的耗时取决于现场设备，不取决于本机，
    // 锁着界面等串口是工控现场最烦人的一种卡顿。
    void call_async(const QString& command, const QJsonObject& args, int timeout_ms,
                    std::function<void(const QJsonObject&, bool, const QString&)> done);

    // blocked_by 原因码 -> 人话。**不认识的码原样透传** ——
    // 宁可显示一个后端新加的英文码，也不要静默吞掉一个自己猜错的原因。
    static QString humanize_reason(const QString& code);
    static QString humanize_state(const QString& s);
    static Level level_for_state(const MachineSnapshot& s);

    PipeClient client_;
    StreamPoller* poller_ = nullptr;
    StatusPoller* state_poller_ = nullptr;
    PredictionPoller* prediction_poller_ = nullptr;

    // ── 顶栏（两页共用）───────────────────────────────────────────────
    QLabel* title_ = nullptr;
    QLabel* clock_ = nullptr;
    StatusDot* daemon_dot_ = nullptr;
    StatusDot* gate_dot_ = nullptr;
    QPushButton* param_page_btn_ = nullptr;
    QPushButton* main_page_btn_ = nullptr;

    // ── 页面容器 ─────────────────────────────────────────────────────
    QStackedWidget* pages_ = nullptr;
    ParameterPage* param_page_ = nullptr;
    QLabel* banner_ = nullptr;

    // ── 三按钮 ───────────────────────────────────────────────────────
    QPushButton* precheck_btn_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    // 预检/出光期间的后端往返还在飞。用它禁用重复点击：
    // 两次 measurement_start 会起两个作业，而界面上只看得见一个。
    bool command_in_flight_ = false;

    // ── 光谱 ─────────────────────────────────────────────────────────
    SpectrumPlot* plot_ = nullptr;
    QLabel* frame_label_ = nullptr;
    QLabel* absent_label_ = nullptr;
    QCheckBox* auto_y_ = nullptr;
    QCheckBox* lock_y_ = nullptr;
    QCheckBox* pause_ = nullptr;
    bool paused_ = false;
    // 队列节奏：只在帧信息行里提示，不单独弹窗（丢了十几帧没人关心，丢了上万帧才是问题）。
    int stream_pending_ = 0;
    qint64 stream_dropped_ = 0;
    bool auto_y_on_ = true;
    bool lock_y_on_ = false;
    double locked_y_hi_ = 1.0;
    qint64 frame_seen_ = 0;

    // ── 底部设备状态条 ───────────────────────────────────────────────
    StatusDot* laser_dot_ = nullptr;
    StatusDot* indicator_dot_ = nullptr;
    StatusDot* spectrometer_dot_ = nullptr;
    QLabel* measurement_label_ = nullptr;

    // ── 导出 ─────────────────────────────────────────────────────────
    FrameData last_frame_;   // 导 CSV 用：最近一帧的**原始**计数
    QStringList csv_lines_;
    bool csv_exported_ = false;
};

}  // namespace libs::ui

