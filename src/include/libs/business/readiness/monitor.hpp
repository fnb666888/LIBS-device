#pragma once

// 业务一：就绪判定（readiness）—— 触发时机层。
//
// ── monitor 只管"什么时候判"，不管"怎么判" ──────────────────────────────
//
// 这层存在的理由很具体：本项目先做 UI 触发，后来要常驻进程。
// 如果"何时触发"与"如何判定"混在一起写，加常驻线程时就必须重写判定 ——
// 而重写安全逻辑正是最不能接受的事。
//
// 所以接口签名现在就按两种模式设计好：
//
// Runtime 周期刷新门禁。MeasurementJob 在开始和开闸前使用同一入口复核。
//
// 最新结论 `latest()` 只读缓存 —— UI 的 status 面板读它，
// **不自己判**。UI 收到"开始检测"的失败，就当成"没开光"，
// 原因看返回码；它绝不能"先问门禁、拿到绿灯再自己决定要不要开始"
// —— 那会让判定出现第二个执行点。

#include <functional>
#include <mutex>
#include <string>

#include "libs/business/readiness/facts.hpp"
#include "libs/business/readiness/verdict.hpp"

namespace business::readiness {

// 读设备事实的回调。返回 false = 读不到（按不安全处理）。
//
// **为什么用回调而不是持设备指针**：本类只负责"何时判"，
// 不该知道设备怎么打开、怎么关。事实怎么来由 Runtime 注入。
using FactSource = std::function<bool(ReadinessFacts&)>;

class ReadinessMonitor {
public:
    struct Snapshot {
        GateVerdict verdict;
        MachineState state;
        bool armed;
        bool laser_readable;
    };

    explicit ReadinessMonitor(FactSource source);

    ReadinessMonitor(const ReadinessMonitor&) = delete;
    ReadinessMonitor& operator=(const ReadinessMonitor&) = delete;

    // 由后台监控和测量作业调用，UI 读取判定快照。
    GateVerdict evaluate_now(GateCheckPhase phase = GateCheckPhase::before_emission);

    Snapshot snapshot() const;

    // UI 只读这个，不自己判。
    GateVerdict latest() const;

    // 最近一次判定对应的机器状态（灯映射与 UI 共用同一份）。
    MachineState latest_state() const;

    // 最近一次采样是否读到了 B2（false = 读不到，按不安全处理）。
    bool laser_readable() const;

    // 事实层配置：这些是**配置事实**（装机/参数库），不是设备读数。
    // 它们与设备事实一起构成 verdict() 的输入。
    void set_parameters_ready(bool ready) noexcept;
    void set_laser_enabled(bool enabled) noexcept;
    void set_laser_commissioned(bool commissioned) noexcept;
    void set_devices_opened(bool opened) noexcept;
    void set_spectrometer_ready(bool ready) noexcept;

    // 显式布防/撤防。撤防是运维/调试用，正常流程由 evaluate() 自动判定。
    libs::VoidResult arm();
    libs::VoidResult disarm();
    bool armed() const;

private:
    // 取一份事实：设备事实来自 source，配置事实来自已设置的值。
    ReadinessFacts collect_facts() const;

    FactSource source_;

    mutable std::mutex mutex_;
    bool parameters_ready_ = false;
    bool laser_enabled_ = true;
    bool laser_commissioned_ = false;
    bool devices_opened_ = false;
    bool spectrometer_ready_ = false;
    bool armed_ = false;

    GateVerdict latest_;
    MachineState latest_state_ = MachineState::offline;
    // 最近一次采样是否真的读到了 B2。**它是一个事实，不是判据**：
    // 判定已经用 laser_unavailable 表达过"读不到"，这里只是让预检
    // 报告能如实说清"是没读到"还是"读到了但不安全"。
    bool laser_readable_ = false;

};

}  // namespace business::readiness
