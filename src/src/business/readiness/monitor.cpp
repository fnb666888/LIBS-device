#include "libs/business/readiness/monitor.hpp"

#include "libs/business/shared/time_util.hpp"
#include "libs/error.hpp"

namespace business::readiness {

ReadinessMonitor::ReadinessMonitor(FactSource source) : source_(std::move(source)) {}

void ReadinessMonitor::set_parameters_ready(bool ready) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    parameters_ready_ = ready;
}

void ReadinessMonitor::set_laser_enabled(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    laser_enabled_ = enabled;
}

void ReadinessMonitor::set_laser_commissioned(bool commissioned) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    laser_commissioned_ = commissioned;
}

void ReadinessMonitor::set_devices_opened(bool opened) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_opened_ = opened;
}

void ReadinessMonitor::set_spectrometer_ready(bool ready) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    spectrometer_ready_ = ready;
}

libs::VoidResult ReadinessMonitor::arm() {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled = laser_enabled_;
    }
    if (!enabled) {
        return libs::Error::make(libs::ErrorKind::device, "gate_laser_disabled",
                                 "激光器在配置中未启用，无法布防");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        armed_ = true;
    }
    // 布防后立刻反映当前条件：条件不满足时状态仍是 standby/fault。
    evaluate_now();
    return libs::VoidResult();
}

libs::VoidResult ReadinessMonitor::disarm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = false;
    latest_state_ = MachineState::standby;
    return libs::VoidResult();
}

bool ReadinessMonitor::armed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_;
}


ReadinessFacts ReadinessMonitor::collect_facts() const {
    ReadinessFacts facts;
    // ── 第一步：回填**配置事实**（必须早于事实源）──────────────────────
    //
    // 这些是装机配置与参数库决定的事实，不是从设备里读出来的。
    // 曾经漏了这一步：事实源填完只回填了 armed，配置事实永远停在
    // ReadinessFacts 的结构体默认值上 —— 于是每次判定都是
    // parameters_complete=false / laser_commissioned=false，
    // 哪怕参数库早已齐备、commissioned 早已为真。
    // 现场表现：Runtime::start 里 set_parameters_ready() /
    // set_laser_commissioned() 全部白设，参数改了也不放行，
    // 判定永远说"参数不齐 + 未标定"。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        facts.parameters_complete = parameters_ready_;
        facts.laser_enabled = laser_enabled_;
        facts.laser_commissioned = laser_commissioned_;
        facts.devices_opened = devices_opened_;
        facts.spectrometer_ready = spectrometer_ready_;
    }

    // ── 第二步：设备事实要真的去读 ──────────────────────────────────────
    // 读不到就保持 available=false，判定会据此拦截（"不知道" != "安全"）。
    if (source_) source_(facts);
    // **armed 只属于本类**：它是状态机的位置（按了布防 = 第一步），
    // 不是从设备里读出来的事实。事实源的签名是 bool(ReadinessFacts&)，
    // 它完全可以整个赋值（测试就是这么做的），而一旦它顺手覆掉 armed，
    // 门禁会在没人睡的情况下突然拒绝放行 —— 现场表现就是“按了布防还是不出光”。
    // 在这里强制归位，才能保证“事实源不可能意外改掉门禁状态”。
    //
    // 其余配置事实（parameters_complete / laser_enabled / laser_commissioned /…）
    // 则允许事实源覆写：Runtime 只写 facts.laser，而测试需要能直接注入各种组合。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        facts.armed = armed_;
    }
    facts.sampled_at_ms = static_cast<std::uint64_t>(monotonic_ms());
    return facts;
}

GateVerdict ReadinessMonitor::evaluate_now() {
    const ReadinessFacts facts = collect_facts();
    const GateVerdict verdict = evaluate(facts);
    const MachineState state = state_of(facts, verdict);
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = verdict;
    latest_state_ = state;
    laser_readable_ = facts.laser.available;
    return verdict;
}

void ReadinessMonitor::start_continuous(unsigned interval_ms) {
    // **接口先定死，实现以后补。**
    //
    // 线程逻辑（周期、异常处理、与业务的交互）依赖现场调试计划，
    // 现在写等于凭空猜参数。但签名现在就摆在这里，意味着将来加线程时
    // 调用方一个字都不用改 —— 这才是"不返工"的真实含义。
    (void)interval_ms;
    interval_ms_ = interval_ms;
    continuous_.store(false);
}

void ReadinessMonitor::stop_continuous() {
    continuous_.store(false);
}

bool ReadinessMonitor::laser_readable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return laser_readable_;
}

const GateVerdict& ReadinessMonitor::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

MachineState ReadinessMonitor::latest_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_state_;
}

}  // namespace business::readiness
