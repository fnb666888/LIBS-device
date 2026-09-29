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
    void serve_client_with(void* pipe_handle);

    // join 所有还在服务的连接线程。只在 run() 退出前调用一次。
    void join_clients();

    Runtime& runtime_;
    DaemonOptions options_;
    std::atomic<bool> stopping_{false};
    // 每个连接一个线程。**只有 accept 循环这一个线程会 push_back**，
    // 所以不需要额外加锁；join_clients() 在 run() 退出路径上跑，
    // 那时 accept 循环已经不再 push。
    std::vector<std::thread> clients_;
};

}  // namespace business::businessd
