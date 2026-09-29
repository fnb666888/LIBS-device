#pragma once

// 状态轮询器（`ui/src/status_poller.hpp`）。
//
// 与 `StreamPoller` 的关键区别：**它跑在自己的线程里**。
//
// 为什么必须有线程：`status` / `param_get` 每次都要开关一次命名管道，
// 而守护进程里碰设备的命令（gate/capture/health）持有一把全局
// `device_mutex`。也就是说，一次 `status` 往返的耗时**取决于现场设备
// 有多慢**，不是取决于本机。若在 UI 线程里同步轮询，激光器串口一卡，
// 整个界面（含按钮响应和动画）就跟着冻住——工控现场这是不可接受的。
//
// 线程纪律：
//   * 工作线程只做管道往返 + JSON 解析，**不碰任何 QWidget**；
//   * 结果经queued 信号送回 UI 线程，UI 再自己取快照；
//   * 解析失败/管道断开**不抛到 UI 线程**，只更新快照里的 error 字段，
//     由 UI 决定怎么表现（一次抖动不该弹窗）。

#include <QMutex>
#include <QObject>
#include <QThread>

#include "machine_state.hpp"
#include "pipe_client.hpp"

namespace libs::ui {

class StatusPoller : public QObject {
    Q_OBJECT
public:
    explicit StatusPoller(PipeClient client, QObject* parent = nullptr);
    ~StatusPoller() override;

    // 启停轮询。interval_ms 是心跳节奏，参考设计文档 §8.1 的 500ms，
    // 实际默认放慢到 800ms：status 要过 device_mutex，跟太紧只是
    // 抢锁，不会让界面更"新"。
    void start(int interval_ms = 800);
    void stop();
    bool is_running() const { return running_; }

    // 线程安全的快照读取：在 UI 线程被 snapshot_ready 唤醒后调用。
    MachineSnapshot snapshot() const;

    // 请求一次立即刷新（"刷新"按钮）。仍是异步的。
    void request_refresh();

signals:
    // 有新快照（成功或失败都发；失败时 snapshot().error 非空、
    // valid=false，但**保留了上一帧的好数据**，UI 据此显示"数据已过期"）。
    void snapshot_ready();

private slots:
    // 由工作线程排队调用，只做一件事：把结果通知出去。
    void publish();

private:
    // 工作线程。实现放在 cpp，避免头文件暴露线程细节。
    class Worker;

    PipeClient client_;
    QThread* thread_ = nullptr;
    Worker* worker_ = nullptr;

    mutable QMutex mutex_;
    MachineSnapshot snapshot_;
    bool running_ = false;
    bool poke_requested_ = false;   // 原子性由 mutex_ 提供
};

}  // namespace libs::ui
