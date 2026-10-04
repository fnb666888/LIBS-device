#pragma once

// 守护进程：命名管道 JSON 服务（范围报告 §5.2，第 9 步落地项）。
//
// 进程模型（已裁决，见范围报告 §5.2）：C++ 守护进程**独占设备**，
// Python CLI 经命名管道通信。因此：
//   * 设备独占性在多客户端场景下才成立，本文件就是那个场景的落地；
//   * 每个连接一个线程，但**业务层的设备访问串行化**（Runtime::device_mutex），
//     因为驱动是同步阻塞的，多线程交错事务会串帧。
//
// 协议：一次请求一行 JSON，一次响应一行 JSON（JSON Lines over 命名管道）。
// 简单、可跨进程、Python 端十几行就能实现。
//
// **安全边界**：管道 ACL 限定当前用户。设备是独占的，管道也应如此。

#include <atomic>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "libs/business/app/runtime.hpp"
#include "libs/error.hpp"

namespace business::businessd {

using app::Runtime;

struct DaemonOptions {
    std::string pipe_name = "\\\\.\\pipe\\libs-businessd";
};

class NamedPipeServer {
public:
    NamedPipeServer(Runtime& runtime, DaemonOptions options);
    ~NamedPipeServer();

    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    // 阻塞运行，直到 stop()。返回时所有客户端线程已 join。
    libs::VoidResult run();

    // 请求停止。可从另一个线程调用。
    void stop();

    // 处理一行请求（不含换行）。**公开是为了让离线测试能直接调用**，
    // 不必真的开管道。
    std::string handle_line(const std::string& line);

    const DaemonOptions& options() const noexcept { return options_; }

private:
    // 参数是 Win32 HANDLE（就是 void*），避免在头文件里拉 windows.h。
    // ClientState 由 clients_ 持有；工作线程结束前会关闭自己的管道句柄。
    struct ClientState {
        explicit ClientState(void* handle) : pipe_handle(handle) {}

        void* pipe_handle = nullptr;
        std::atomic<bool> done{false};
        std::thread worker;
    };

    void serve_client_with(ClientState* client);

    // 回收已经结束的连接线程，避免短连接把 clients_ 无限堆大。
    void reap_clients();

    // 断开所有活动客户端，使阻塞的 ReadFile 返回。
    void disconnect_clients();

    // join 所有还在服务的连接线程。只在 run() 退出前调用。
    void join_clients();

    Runtime& runtime_;
    DaemonOptions options_;
    std::atomic<bool> stopping_{false};
    // 手动复用 void*，避免头文件依赖 windows.h。实际值是手动复位事件 HANDLE。
    void* stop_event_ = nullptr;

    // accept 线程负责增加/回收客户端；stop() 会从其它线程断开活动句柄。
    // 工作线程结束时也需要拿这把锁完成句柄关闭，避免 stop() 与 CloseHandle 竞态。
    std::mutex clients_mutex_;
    std::vector<std::unique_ptr<ClientState>> clients_;
};

}  // namespace business::businessd
