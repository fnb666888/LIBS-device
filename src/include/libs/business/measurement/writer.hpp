#pragma once

// 业务二：激光触发光谱检测（measurement）—— 落盘子模块。
//
// ── 它为什么是"子模块"而不是"输出端口" ──────────────────────────────────
//
// 判据只有一条，写在这里免得下次划分时重新想一遍：
//
//   > **能重放的（留档、追溯）是子模块；只是看一眼的是订阅者。**
//
// 落盘数据要留档、要追溯 → 子模块，由 `MeasurementJob` 拥有。
// 实时显示只是看一眼 → 订阅者，所有权在 Runtime（见 `frame_bus.hpp`）。
//
// ── 纪律 4：落盘异步，绝不反压采集节奏 ───────────────────────────────────
//
// 本类有自己的线程 + 有界队列（limit 64）。队列满 → 丢最旧的一帧并计数；
// 磁盘卡住 → 也不能拖住出光节奏。
//
// **安全路径（stop）与数据路径必须解耦**：收光那条命令不能排在磁盘后面。
// 所以写盘在**独立线程**里做，且不在任何设备锁内。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "libs/business/measurement/frame.hpp"
#include "libs/business/shared/data_delivery.hpp"

namespace business::measurement {

// 落盘队列的深度。满了就丢最旧的一帧，如实计数，**绝不反压采集线程**。
inline constexpr std::size_t kDiskQueueLimit = 64;

class Writer {
public:
    // sink 与 station_id 都不拥有：sink 由 Runtime 建好后注入。
    Writer(data_delivery::DeliverySink& sink, std::string station_id);
    ~Writer();

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // 线程生命周期。通常由 `MeasurementJob::start()` / `stop()` 驱动，
    // 但留成公开方法是因为"起一次落盘线程"本身就是这个类的职责，
    // 不该让作业类替它决定。
    void start();
    // 停止并 join。**可重复调用**，幂等。
    void stop();

    // 投递一帧。**O(1)，不阻塞采集线程**。
    void enqueue(const SpectrumFrame& frame);

    // 计数（供 `measurement status` 上报）。
    std::uint64_t writes_ok() const noexcept;
    std::uint64_t writes_failed() const noexcept;
    std::uint64_t backlog() const noexcept;

    // 最近一次落盘失败的原因。空 = 最近没有失败。
    // 由 `MeasurementJob` 读出来并合进自己的 last_error_*。
    std::string last_error_code() const;
    std::string last_error_message() const;

private:
    void loop();
    // 把一帧落盘。**在落盘线程内调用**，不持锁。
    void persist(const SpectrumFrame& frame);

    data_delivery::DeliverySink& sink_;
    std::string station_id_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<SpectrumFrame> queue_;
    std::thread thread_;
    // 停止标志。stop() 置位后，落盘线程把队列里剩下的帧写完才退出 ——
    // 停机不是丢数据的理由。
    bool stopping_ = false;

    std::atomic<std::uint64_t> writes_ok_{0};
    std::atomic<std::uint64_t> writes_failed_{0};
    std::atomic<std::uint64_t> backlog_{0};
    std::string last_error_code_;
    std::string last_error_message_;
};

}  // namespace business::measurement