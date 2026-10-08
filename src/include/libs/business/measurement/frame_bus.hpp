#pragma once

// 业务二：激光触发光谱检测（measurement）—— 帧出口。
//
// ── 这个类是什么 ────────────────────────────────────────────────────────
//
// **纯内存的发布-订阅通道**。生产者是采集方（`MeasurementJob`），
// 消费者是 UI（经命名管道轮询）。本类**不碰任何设备**：没有激光器，
// 没有光谱仪，没有采集线程，没有 `device_mutex`。
//
// ── 谁拥有、谁写、谁读 ────────────────────────────────────────────────────
//
//   谁拥有：Runtime。因为 UI 必须在 `measurement_start` 之前就能订阅。
//   谁写  ：只有 `MeasurementJob`。
//   谁读  ：任何订阅者，纯内存、零设备、零线程。
//
// 放 `measurement/` 目录是因为它是业务二的输出端口，
// 但**所有权在 Runtime** —— 这个区别很重要，
// 它就是"实时显示不是子模块"在代码上的体现。
//
// ── 搬过来的纪律（一条都不能丢）──────────────────────────────────────────
//
//  1. **背压策略：丢最旧帧，不阻塞生产者**。
//     订阅者各有自己的队列。队列满时**丢最旧的一帧** —— 宁可 UI 少看几帧，
//     也不能让一个慢客户端把采集节奏拖住。丢帧如实记 `dropped_frames`。
//
//  2. **订阅者的队列不属于任何 GUI 线程**，因此 UI 崩了/卡了都不会影响采集。
//
//  3. **帧号与丢帧分开**：帧号由采集侧（`MeasurementJob`）单调自增，
//     与这里的丢弃计数完全无关。早先把它算成 `dropped + captured`，
//     结果是漏掉一帧显示会让后续所有帧号错位。

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

#include "libs/business/measurement/frame.hpp"
#include "libs/business/shared/json/json.hpp"

namespace business::measurement {

class FrameQueue {
public:
    struct Options {
        // 每个订阅者的队列深度。队列满时丢最旧的一帧。
        //
        // 8 帧是这么定的：一帧四通道 2048 点，序列化 + 过命名管道实测 ~800 ms，
        // UI 轮询周期 200 ms。深度小于 4，客户端正常轮询也追不上；
        // 再大只是把内存和"看到过期谱"的时间一起放大。
        std::size_t subscriber_queue_frames = 8;
    };

    // 默认参数**故意不给** `= Options{}`：嵌套类的默认成员初始化器在
    // 外层类还没解析完时是不可用的（GCC 直接报 required before the end of
    // its enclosing class）。所以提供两个构造函数。
    FrameQueue();
    explicit FrameQueue(Options options);

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // ── 订阅 ────────────────────────────────────────────────────────────
    //
    // 订阅句柄就是一个数字 id，不持有任何设备或线程资源。
    // 退订（unsubscribe）可重复调用。disconnect 之前建议先 unsubscribe，
    // 否则退订后残留的帧会在客户端断开时被白白丢掉。
    std::uint64_t subscribe();
    void unsubscribe(std::uint64_t id);

    // 取出一帧。队列空返回 false，**不阻塞**。
    //
    // **注意**：这是唯一的取帧入口，且**不**加任何锁去碰设备 ——
    // 帧已经离开设备了，取帧不该和设备访问互相排队。
    bool try_pop(std::uint64_t id, SpectrumFrame& out);

    std::size_t pending(std::uint64_t id) const;

    // 累计丢帧数（因为订阅者队列满而丢的）。UI 可用它提示"显示跟不上采集"。
    std::uint64_t dropped_frames() const noexcept;

    // ── 生产 ────────────────────────────────────────────────────────────
    //
    // 把一帧投递给所有订阅者（各自丢最旧的一帧）。
    // **O(订阅者数)**，不阻塞、不碰设备 —— 采集循环调它不会拖慢节奏。
    void publish(const SpectrumFrame& frame);

    // 停流时清空所有订阅者队列：留着这些帧没有意义。
    // **不**重置 frame_index（那是采集侧单调的，跨 start/stop 保持连续
    // 更利于 UI 去重）。
    void clear_pending();

    // 运行状态快照。
    json::Value status() const;

private:
    // 把一帧投递给所有订阅者。调用方已持有 mutex_。
    void publish_locked(const SpectrumFrame& frame);

    mutable std::mutex mutex_;
    Options options_;

    std::uint64_t next_frame_index_ = 0;
    std::uint64_t next_subscriber_id_ = 1;
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::uint64_t delivered_frames_ = 0;

    struct Subscriber {
        std::deque<SpectrumFrame> queue;
    };
    std::map<std::uint64_t, Subscriber> subscribers_;
};

}  // namespace business::measurement