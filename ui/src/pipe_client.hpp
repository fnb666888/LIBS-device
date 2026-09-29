#pragma once

// 命名管道客户端（`ui/src/pipe_client.hpp`）。
//
// 纪律（和 `remote/lpc.py` 同一套）：
//   **这个文件只讲 JSON，不碰设备。** 它不开串口、不加载 SDK、不判定
//   "设备是不是安全"——那些判断是业务层的活。UI 一旦开始自己判安全，
//   判定权就从第 4 层漏到了 UI 层，docs/architecture-and-workflows.md §4.2
//   的分层纪律随之失效。
//
// 为什么不复用 `src/business/json/json.hpp`：
//   复用它会让 UI 链接 `libs_business`，而 `libs_business` 公开链接
//   `libs_devices`（C++ 里 PUBLIC 依赖会传染到下游的链接命令行）。那就等于
//   UI 反向依赖了设备层，是纪律里明令禁止的。所以这里用 Qt 自带的
//   QJsonDocument——**Qt6 Core 官方公开库**，零新增依赖，还省掉手写解析器。

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace libs::ui {

// 一次"写一行请求、读一行响应"的调用。同步、带超时，失败抛 TransportError。
// 之所以做成**每次调用自己开关管道**（而不是常连一条），是因为守护进程那边
// 一个连接一次只处理一条请求；开长连接还得处理断线重连，而 UI 的
// `latest` 是短轮询，每次重开反而更简单也更诚实。
class PipeClient {
public:
    // 管道名可用环境变量 LIBS_BUSINESSD_PIPE 覆盖，便于多实例联调。
    static QString default_pipe_name();
    explicit PipeClient(QString pipe_name = QString()) : pipe_name_(std::move(pipe_name)) {}

    // 底层往返：写入一行 JSON，返回解析好的响应对象。
    QJsonObject call(const QJsonObject& request, int timeout_ms = 5000);

    // 业务糖：构造 {"command":..., "args":...} 并把 ok=false 翻成异常，
    // 让调用点读起来像"要么成功要么抛"。
    QJsonObject command(const QString& name, const QJsonObject& args = QJsonObject(),
                        int timeout_ms = 5000);

    // 只要 payload。**推荐用这个。**
    //
    // command() 返回的是**整个响应** `{ok, payload}`，取错一层会拿到一个空
    // 对象，而且失败是静默的 —— 这个坑本项目踩过两次：
    //   * 预检永远"不通过"（gate 取成空对象，allowed 恒为 false）
    //   * 预览订阅号永远是 -1（取不到 subscriber，定时器根本没启动，
    //     于是订阅了却从来不取帧，守护进程那边一路丢帧）
    QJsonObject command_payload(const QString& name, const QJsonObject& args = QJsonObject(),
                                int timeout_ms = 5000);

    const QString& pipe_name() const { return pipe_name_; }
    void set_pipe_name(QString name) { pipe_name_ = std::move(name); }

private:
    QString pipe_name_;
};

}  // namespace libs::ui
