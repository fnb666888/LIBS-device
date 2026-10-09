#pragma once

// 测量参数页（`ui/src/parameter_page.hpp`）。
//
// ── 这页为什么存在 ────────────────────────────────────────────────────
//
// 参数曾经散在主窗口左栏的两张卡片里，而它们发出去的字段名全是旧的：
// `discharge_current`、`timeout_ms`……而后端参数库里的真名是
// `discharge_current_a`、`capture_timeout_ms`。两边对不上，
// 后端 `take_uint(values, "discharge_current_a", ...)` 取不到值，
// 每次保存都回 `parameter_field_bad`；同一批错名也让
// `sync_params_from_snapshot()` 永远显示「param_get 未返回该组」。
//
// 也就是说：**重构前那个参数面板，保存按钮 100% 是坏的**。
// 这页把字段名收敛成**唯一一份真值**（`FieldSpec::key`），
// 且它抄的是 `src/src/business/app/parameter_service.cpp` 里的常量，
// 不是文档里的占位数字。
//
// ── 纪律 ──────────────────────────────────────────────────────────────
//
//   1. **只发改动过的字段**，不整组覆盖。param_set 是**立即生效**的，
//      测量进行中整组覆盖会直接改掉下一次出光的行为。
//   2. **必须检查 `completed`。** completed=false 是"收到请求但未完成"，
//      不能当成功 —— 后端 note 字段的原话就是「调用方必须检查它」。
//   3. **逐字段报 failed_fields**，且**保留操作员输入**。
//      一次改 9 个错 1 个就整页回滚，等于让人重填 9 个。
//   4. 保存成功后只说「参数已更新，下次判定生效」，
//      **不要**说"现在可以出光了" —— 那是 UI 自己做安全判定。
//   5. UI 不链接 libs_business，跨进程只走 JSON（见 pipe_client.hpp）。

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QPushButton;

#include "machine_state.hpp"

namespace libs::ui {

class Card;
class StatusDot;

class ParameterPage : public QWidget {
    Q_OBJECT
public:
    explicit ParameterPage(QWidget* parent = nullptr);

    // 把一次 `param_get` 的结果铺到输入框上。
    // **只在没有未保存改动时铺**（见 has_pending_changes）——
    // 否则 800ms 一次的轮询会把操作员正在输的值抹掉。
    void apply_snapshot(const MachineSnapshot& s);

    // 从守护进程回读一次 param_get 并铺上（「放弃修改」按钮走这条）。
    // 注意它**不在** status 轮询里：回读是用户动作，要能立刻看到结果。
    // force=true 用于「放弃修改」：**明知道**有未保存改动也要覆盖回去，
    // 那正是这个按钮的语义。其它入口一律不许覆盖（见 apply_snapshot）。
    void apply_values(const QJsonObject& groups, bool force = false);
    // 正在等用户输入：此时不许轮询结果覆盖输入框。
    bool has_pending_changes() const;
    // 保存/回读进行中：整个表单锁住。
    bool busy() const { return save_in_flight_ || reloading_; }

signals:
    // 保存请求。实参是 {"group": "...", "values": {...}}。
    //
    // 发信号而不是自己发管道请求，是为了**不阻塞 UI 线程**：
    // param_set 要过 device_mutex，一次往返的耗时取决于现场设备有多慢，
    // 不取决于本机。锁着界面等串口是现场最烦人的一种卡顿。
    void save_requested(const QJsonObject& request);
    // 「放弃修改」：请求回读。实参为空。
    void reload_requested();
    // 保存/回读之后请立刻刷新一次状态轮询，让顶栏的参数指示跟上。
    void refresh_requested();

public slots:
    // 保存返回了。summary 是给人看的一句话（已含成功/失败与失败字段）。
    void on_save_result(bool ok, const QString& summary);

private slots:
    void on_field_changed();
    void on_save();
    void on_discard();

private:
    // 一个可编辑字段。
    //
    // `key` 是 **param_set 用的字段名，也是界面的唯一真值来源** ——
    // 以前这里有第二份（`over` 列，给已删除的 laser_fire 覆盖参数用），
    // 两份名字对不上正是保存必坏的根因。现在只有一份。
    struct FieldSpec {
        const char* key;
        const char* label;
        int decimals;
        const char* unit;
        const char* tooltip;
    };

    struct Field {
        QString key;
        QDoubleSpinBox* box = nullptr;
        QLabel* error = nullptr;    // 该字段被后端拒绝时的标红提示
        QString range_text;          // "15 ~ 100 A"，写在下划线提示里
        double original = 0.0;       // 最近一次从守护进程读到的值
        bool known = false;         // 参数库里到底有没有这个字段（false = 未标定）
        // 是否**至少铺过一次**守护进程的数据。
        //
        // 这个标志不是可有可无的：QDoubleSpinBox 没有"空"状态，setRange()
        // 会把初值夹到**下限**（积分时间 0.002 ms、放电电流 15 A……），
        // 而 original 的初值是 0.0。于是开页瞬间
        //     value() != original   →  "0.002 != 0"  →  true
        // has_pending_changes() 就恒为 true，第一次 param_get 的结果被
        // 当成"用户在编辑"挡掉，apply_values 永远不跑 —— 表现就是
        // 整页停在量程下限，一个真实值都没显示。
        // 只有"铺过之后"的差异才算用户改动。
        bool loaded_once = false;    };

    struct Group {
        QString name;               // "laser" / "spectrometer"
        QString title;
        Card* card = nullptr;
        StatusDot* dot = nullptr;   // 该组保存结果的状态点
        QVector<Field> fields;
    };

    // 只保留**每次检测可能需要按样品调**的字段。
    //
    // 刻意不放 0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护：那是**整机标定值**，
    // 存在设备 EEPROM 里被所有工具共享，本程序只读不写。把它们摆到界面上，
    // 现场迟早有人去调 —— 而调坏那一个数，整台设备就不出光了（真发生过）。
    //
    // 字段名与量程逐条抄自 parameter_service.cpp；**不是**抄自设计文档。
    static const FieldSpec* laser_specs(std::size_t& count);
    static const FieldSpec* spectrometer_specs(std::size_t& count);

    void build_group(Group& g);
    // 收集某组里"用户改过"的字段。**没有改动就不发**：
    // 发空 values 等于让后端白跑一趟 apply，而现场看不出任何区别。
    QJsonObject collect_changed(const Group& g) const;
    void clear_errors(Group& g);
    // 把一组标成"已保存"，让 original 跟输入框对齐。
    void commit_group(Group& g);
    void set_editing_enabled(bool on);
    void render_summary(const MachineSnapshot& s);
    // 本页是否还有"该由操作员去填的空框"（参数库里没有的字段）。
    //
    // **刻意与 has_pending_changes() 分开**：两者回答的是不同问题。
    //   * has_pending_changes()  = "用户正在输入吗" —— 用来挡住轮询覆盖；
    //   * has_uncalibrated_fields() = "有该填还没填的框吗" —— 用来提示。
    // 曾经用同一个函数回答两个问题，后果是：未标定的 laser 组让
    // has_pending_changes() 恒为 true，于是 apply_snapshot 每轮都被挡，
    // **光谱组那三个有真实值的框也铺不上去**，整页停在量程下限。
    bool has_uncalibrated_fields() const;
    // setValue() 会触发 valueChanged -> on_field_changed()，而那时候
    // original 还没落定，会被当成"用户改了"。这个标志让槽函数早退一次。
    void set_suppress_events_(bool on) { suppress_events_ = on; }

    QVector<Group> groups_;
    StatusDot* readiness_dot_ = nullptr;
    QLabel* readiness_text_ = nullptr;
    QLabel* save_result_ = nullptr;
    QPushButton* save_btn_ = nullptr;
    QPushButton* discard_btn_ = nullptr;
    // 正在等一次保存的返回。用来防止连点：param_set 会过 device_mutex，
    // 连点几次只会把参数互相覆盖，而且界面还来不及显示第一次的结果。
    bool save_in_flight_ = false;
    // 「放弃修改」正在进行。回读是一次管道往返，期间不接受编辑。
    bool reloading_ = false;
    // 铺值时抑制 on_field_changed（见 set_suppress_events_）。
    bool suppress_events_ = false;
};

}  // namespace libs::ui

