#pragma once

// 帧轮询器（`ui/src/stream_poller.hpp`）。
//
// 业务逻辑就落在这一层：**订阅 → 定时取帧 → 交给绘制层**。
// 轮询而不是长连接推流，是刻意的：守护进程的 accept 循环一次只服务
// 一个客户端（见 docs 附录里记的传输层约束），如果 UI 用长轮询占着连接，
// 别的客户端（Python CLI、诊断脚本）就连不上。所以这里每次 `latest`
// 都是**短请求、立即返回**，不占连接。见 spectrum_stream 设计说明。

#include <QObject>
#include <QThread>

#include "frame_model.hpp"
#include "pipe_client.hpp"

namespace libs::ui {

class StreamPoller : public QObject {
    Q_OBJECT
public:
    explicit StreamPoller(PipeClient client, QObject* parent = nullptr);
    ~StreamPoller() override;

    // 订阅并启动后台取帧线程。订阅本身失败返回 false；第一帧通过
    // frame_ready 异步送回，避免大 JSON 往返阻塞界面。
    bool open();
    // 退订并停表。幂等。
    void close();
    bool is_open() const { return open_; }

signals:
    // 采到一帧（哪怕 ok=false，UI 要把失败原因显示出来而不是静默）。
    void frame_ready(const libs::ui::FrameData& frame);
    // 整次采集失败（ok=false）：单独一个信号，方便 UI 用醒目色提示，
    // 而不是把"失败"和"这一帧的数据"混在一处。
    void capture_failed(const QString& code, const QString& message);
    // 传输层/业务层报错（守护进程没起、订阅失败……）。
    void stream_error(const QString& message);
    // 取帧的节奏信息变化（pending / dropped），用于"跟不上"提示。
    void stats_changed(int pending, qint64 dropped);

private slots:
    // Worker 线程只做管道往返；这些槽在 UI 线程执行，负责丢弃旧会话的
    // 延迟结果并转发已有信号。
    void deliver_frame(qint64 session, const libs::ui::FrameData& frame);
    void deliver_error(qint64 session, const QString& message);
    void deliver_stats(qint64 session, int pending, qint64 dropped);

private:
    class Worker;

    PipeClient client_;
    bool open_ = false;
    qint64 subscriber_ = -1;   // 守护进程分配的订阅号；-1 = 未订阅
    qint64 last_frame_index_ = -1;
    qint64 dropped_ = 0;
    qint64 session_ = 0;
    QString last_stream_error_;
    Worker* worker_ = nullptr;
};

}  // namespace libs::ui
