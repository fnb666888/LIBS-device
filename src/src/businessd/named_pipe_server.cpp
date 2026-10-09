#include "libs/businessd/named_pipe_server.hpp"

#include <windows.h>

#include <aclapi.h>

#include <sddl.h>

#include <string>
#include <vector>

#include "libs/business/app/facade.hpp"

namespace business::businessd {

namespace {

// 管道安全：只允许当前用户读写，并且**显式打上中等完整性的强制标签**。
//
// 为什么两条都要：
//   1. DACL 限定当前用户 —— 设备是独占资源，管道同理。不用 nullptr
//      （那是"所有人可读写"）。
//   2. **强制标签必须是 Medium**。这一条是现场踩出来的：
//      守护进程若由**高完整性**进程启动（管理员 SSH、Windows 服务），
//      它创建的管道对象会带上高完整性标签，而**中等完整性的普通桌面进程
//      写不进去**（no-write-up）。现象极具误导性 ——
//      "同一个用户，桌面上跑的 UI 报 Access Denied，管理员会话里却连得上"，
//      看上去像 ACL 写错了，其实 DACL 完全正确。
//      SDDL 里的 S:(ML;;NW;;;ME) 就是"中等完整性、不允许向上写"：
//      中等及以上的客户端可读写，低完整性仍被挡。
//      （曾经还在这里有个 use-after-free：token 缓冲放在内层作用域，
//       递给 SetEntriesInAcl 时已经析构。改成整串 SDDL 由系统解析，
//       不再手工拼 ACL。）
class PipeSecurity {
public:
    PipeSecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            fail("OpenProcessToken", GetLastError());
            return;
        }
        std::vector<unsigned char> sid_buffer;
        DWORD size = 0;
        // 第一趟故意传空指针"问需要多大"：**这次调用按设计就是失败的**
        // （返回 FALSE + ERROR_INSUFFICIENT_BUFFER），只需要它填出来的 size。
        // 曾经写成 `if (GetTokenInformation(..., nullptr, 0, &size) && size != 0)`，
        // 那个条件永远为假 —— 于是取 SID 的整段代码从未执行过，安全描述符
        // 一直是空的，管道用上了令牌默认 ACL。默认 ACL 里只有 SYSTEM 和
        // Administrators、没有当前用户，普通桌面进程（令牌里 Administrators
        // 是"仅用于拒绝"）因此被拒，而提权会话能连。这就是那个 bug 的全貌。
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size != 0) {
            sid_buffer.resize(size);
            if (!GetTokenInformation(token, TokenUser, sid_buffer.data(), size, &size)) {
                sid_buffer.clear();
            }
        }
        CloseHandle(token);
        if (sid_buffer.empty()) {
            fail("GetTokenInformation(TokenUser)", GetLastError());
            return;
        }

        LPWSTR sid_text = nullptr;
        if (!ConvertSidToStringSidW(
                reinterpret_cast<const TOKEN_USER*>(sid_buffer.data())->User.Sid, &sid_text)) {
            fail("ConvertSidToStringSidW", GetLastError());
            return;
        }
        std::wstring sddl = L"D:(A;;FRFW;;;";   // FR/FW = FILE_GENERIC_READ / WRITE
        sddl += sid_text;
        sddl += L")S:(ML;;NW;;;ME)";             // 中等完整性强制标签，no-write-up
        LocalFree(sid_text);

        // 直接按自相对格式建整份描述符：手工拼 ACL 容易在权限位和生命周期上出错。
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            fail("ConvertStringSecurityDescriptorToSecurityDescriptorW", GetLastError());
            return;
        }
        sd_ = sd;
    }
    ~PipeSecurity() { if (sd_ != nullptr) ::LocalFree(sd_); }

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    bool ok() const noexcept { return sd_ != nullptr; }
    const std::string& error() const noexcept { return error_; }

    SECURITY_ATTRIBUTES* attributes() {
        sa_.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa_.lpSecurityDescriptor = sd_;
        sa_.bInheritHandle = FALSE;
        return &sa_;
    }

private:
    void fail(const char* what, unsigned long code) {
        error_ = std::string(what) + " failed, win32 " + std::to_string(code);
    }

    PSECURITY_DESCRIPTOR sd_ = nullptr;
    SECURITY_ATTRIBUTES sa_ = {};
    std::string error_;
};

// ConnectNamedPipe 在 run() 里用 overlapped 事件等待 stop_event_，否则
// stop() 无法可靠地唤醒一个还卡在 ConnectNamedPipe 的 accept 线程。
constexpr DWORD kPipeDirection = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
constexpr DWORD kPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
constexpr DWORD kPipeInstances = PIPE_UNLIMITED_INSTANCES;
constexpr DWORD kPipeOutBuffer = 64 * 1024;
constexpr DWORD kPipeInBuffer = 64 * 1024;
constexpr DWORD kPipeTimeout = 5000;   // 客户端不读写就断开，不留半开连接

// 管道句柄使用 FILE_FLAG_OVERLAPPED（accept 需要它来等待 stop_event_），
// 因而客户端线程也必须用 overlapped I/O；对这种句柄传 nullptr 给 ReadFile
// 会让 Windows 的完成语义不可靠，甚至出现“读请求偶尔不返回”。每个操作
// 都有自己的事件，并同时等待全局 stop_event_，所以 stop() 能取消正在读写
// 的客户端。
bool read_pipe(HANDLE pipe, HANDLE stop_event, char* buffer, DWORD capacity, DWORD* read) {
    *read = 0;
    OVERLAPPED overlapped{};
    overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;

    BOOL complete = ::ReadFile(pipe, buffer, capacity, read, &overlapped);
    if (!complete) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_IO_PENDING) {
            ::CloseHandle(overlapped.hEvent);
            return false;
        }
        HANDLE waits[2] = {stop_event, overlapped.hEvent};
        const DWORD wait_result = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0) {
            ::CancelIoEx(pipe, &overlapped);
            ::WaitForSingleObject(overlapped.hEvent, INFINITE);
            ::CloseHandle(overlapped.hEvent);
            return false;
        }
        if (wait_result != WAIT_OBJECT_0 + 1 ||
            !::GetOverlappedResult(pipe, &overlapped, read, FALSE)) {
            ::CancelIoEx(pipe, &overlapped);
            ::WaitForSingleObject(overlapped.hEvent, INFINITE);
            ::CloseHandle(overlapped.hEvent);
            return false;
        }
    }
    ::CloseHandle(overlapped.hEvent);
    return *read != 0;
}

bool write_pipe(HANDLE pipe, HANDLE stop_event, const char* buffer, DWORD size) {
    DWORD offset = 0;
    while (offset < size) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) return false;

        DWORD written = 0;
        BOOL complete = ::WriteFile(pipe, buffer + offset, size - offset, &written,
                                     &overlapped);
        if (!complete) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING) {
                ::CloseHandle(overlapped.hEvent);
                return false;
            }
            HANDLE waits[2] = {stop_event, overlapped.hEvent};
            const DWORD wait_result = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait_result == WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &overlapped);
                ::WaitForSingleObject(overlapped.hEvent, INFINITE);
                ::CloseHandle(overlapped.hEvent);
                return false;
            }
            if (wait_result != WAIT_OBJECT_0 + 1 ||
                !::GetOverlappedResult(pipe, &overlapped, &written, FALSE)) {
                ::CancelIoEx(pipe, &overlapped);
                ::WaitForSingleObject(overlapped.hEvent, INFINITE);
                ::CloseHandle(overlapped.hEvent);
                return false;
            }
        }
        ::CloseHandle(overlapped.hEvent);
        if (written == 0) return false;
        offset += written;
    }
    return true;
}

}  // namespace

NamedPipeServer::NamedPipeServer(Runtime& runtime, DaemonOptions options)
    : runtime_(runtime), options_(std::move(options)) {
    // 手动复位事件同时承担两件事：唤醒 overlapped ConnectNamedPipe，以及
    // 告知 stop() 已经请求退出。事件从构造开始就存在，消除了“stop 早于
    // CreateNamedPipe 调用，唤醒信号丢失”的竞态。
    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

NamedPipeServer::~NamedPipeServer() {
    stop();
    if (stop_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
}

void NamedPipeServer::stop() {
    stopping_.store(true, std::memory_order_release);
    if (stop_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(stop_event_));
    }
    // ReadFile 是同步调用；仅设置 stopping_ 不会让它返回。DisconnectNamedPipe
    // 会让客户端线程收到 ERROR_BROKEN_PIPE，随后由 worker 自己关闭句柄。
    disconnect_clients();
}

std::string NamedPipeServer::handle_line(const std::string& line) {
    // **这里不加锁**：门面里真正碰设备的命令（gate/capture/health）自己会取
    // device_mutex。std::mutex 不可重入，外层再锁一次就是死锁——
    // 每个连接一个线程，一个客户端一条 health 就能把整个守护进程挂住。
    // 串行化由门面自己负责，这里只做协议转换。
    app::BusinessFacade facade(runtime_);
    const app::Response response = facade.dispatch(line);
    json::Value root = json::object();
    root.set("ok", json::Value(response.ok));
    root.set("payload", response.payload);
    if (!response.ok) {
        root.set("error", json::Value(response.error.message));
        root.set("error_kind", json::Value(libs::to_string(response.error.kind)));
        root.set("error_code", json::Value(response.error.code));
    }
    return json::stringify(root);
}

libs::VoidResult NamedPipeServer::run() {
    PipeSecurity security;
    // 构造不出安全描述符就**拒绝启动**，不要带着默认 ACL 跑起来。
    // 默认 ACL 里没有当前用户的 SID（只有 SYSTEM / Administrators），
    // 普通桌面进程（令牌里 Administrators 是"仅用于拒绝"）会被挡在门外，
    // 而提权会话却能连上 —— 现象是"同一个用户，UI 报未连接、命令行却正常"。
    // 静默带着错 ACL 起服务，正是本项目最忌讳的"未知当正常"。
    if (!security.ok()) {
        return libs::Error::make(libs::ErrorKind::internal, "pipe_security_failed",
                                 "无法构造管道安全描述符，拒绝用默认 ACL 启动：" + security.error());
    }

    if (stop_event_ == nullptr) {
        return libs::Error::make(libs::ErrorKind::internal, "pipe_stop_event_failed",
                                 "无法创建命名管道停止事件");
    }

    libs::VoidResult result;

    while (!stopping_.load(std::memory_order_acquire)) {
        reap_clients();
        HANDLE pipe = CreateNamedPipeA(options_.pipe_name.c_str(), kPipeDirection, kPipeMode,
                                       kPipeInstances, kPipeOutBuffer, kPipeInBuffer, kPipeTimeout,
                                       security.attributes());
        if (pipe == INVALID_HANDLE_VALUE) {
            if (stopping_.load(std::memory_order_acquire)) break;
            result = libs::Error::make(libs::ErrorKind::transport, "pipe_create_failed",
                                       "无法创建命名管道 " + options_.pipe_name);
            break;
        }

        // 异步等待客户端或 stop_event_。ERROR_PIPE_CONNECTED 代表客户端在
        // ConnectNamedPipe 前已经连入，仍是一个有效连接。
        OVERLAPPED connect_overlapped{};
        connect_overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (connect_overlapped.hEvent == nullptr) {
            ::CloseHandle(pipe);
            result = libs::Error::make(libs::ErrorKind::transport, "pipe_event_failed",
                                       "无法创建命名管道连接事件");
            break;
        }

        BOOL connected = ::ConnectNamedPipe(pipe, &connect_overlapped);
        if (!connected) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (error == ERROR_IO_PENDING) {
                HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), connect_overlapped.hEvent};
                const DWORD wait_result = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait_result == WAIT_OBJECT_0) {
                    // 取消未完成的 ConnectNamedPipe，再等事件变为有信号，确保
                    // 不在 OVERLAPPED 仍被系统使用时释放 pipe/event。
                    ::CancelIoEx(pipe, &connect_overlapped);
                    ::WaitForSingleObject(connect_overlapped.hEvent, INFINITE);
                    connected = FALSE;
                } else if (wait_result == WAIT_OBJECT_0 + 1) {
                    DWORD transferred = 0;
                    connected = ::GetOverlappedResult(pipe, &connect_overlapped,
                                                      &transferred, FALSE);
                } else {
                    ::CancelIoEx(pipe, &connect_overlapped);
                    ::WaitForSingleObject(connect_overlapped.hEvent, INFINITE);
                    connected = FALSE;
                    if (!stopping_.load(std::memory_order_acquire)) {
                        result = libs::Error::make(libs::ErrorKind::transport,
                                                   "pipe_connect_wait_failed",
                                                   "等待命名管道客户端连接失败");
                    }
                }
            } else {
                connected = FALSE;
            }
        }
        ::CloseHandle(connect_overlapped.hEvent);

        if (!connected) {
            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(pipe);
            if (stopping_.load(std::memory_order_acquire)) break;
            if (!result.ok()) break;
            continue;
        }

        // stop() 可能与连接完成同时发生；不要把唤醒用的连接变成工作线程。
        if (stopping_.load(std::memory_order_acquire)) {
            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(pipe);
            break;
        }

        // **每个连接一个线程**（这才是头文件里写下的契约）。
        // 曾经这里是直接 serve_client_with(pipe) 顺序处理，
        // 后果是：上一个客户端断开到服务端的 ReadFile 返回之间存在窗口，
        // 这段时间里**下一次 CreateNamedPipe 还没发生**，新客户端 CreateFileW
        // 拿到的是 ERROR_FILE_NOT_FOUND(2) / ERROR_PIPE_BUSY(231)——
        // 也就是"上一个请求刚回、紧接着的下一个请求连不上"。
        // 实时光谱 UI 每 100ms 轮询一次 latest，正好把这个窗口踩满，
        // 于是表现为"界面偶发连不上守护进程"。线程化之后 accept 循环
        // 立刻回到 CreateNamedPipe，新连接总能排上队。
        //
        // 设备访问的串行化**不在这里**：门面里碰设备的命令自己取 device_mutex，
        // handle_line() 的注释说明了为什么这里不能再加一把外层锁。
        auto client = std::make_unique<ClientState>(pipe);
        ClientState* client_ptr = client.get();
        client->worker = std::thread([this, client_ptr]() { serve_client_with(client_ptr); });
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(std::move(client));
        }
    }

    // 不论是 stop() 还是创建/连接错误，都必须先打断同步 ReadFile，再 join。
    // 否则一个保持连接但不再发送数据的客户端会让守护进程退出永久卡住。
    disconnect_clients();
    join_clients();
    return result;
}

void NamedPipeServer::reap_clients() {
    std::vector<std::unique_ptr<ClientState>> finished;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.begin();
        while (it != clients_.end()) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // join 不放在 clients_mutex_ 内；worker 完成收尾时也要拿这把锁。
    for (auto& client : finished) {
        if (client->worker.joinable()) client->worker.join();
    }
}

void NamedPipeServer::disconnect_clients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& client : clients_) {
        if (client != nullptr && client->pipe_handle != nullptr) {
            // 先取消 overlapped ReadFile/WriteFile，再断开管道。只断开并不
            // 保证所有版本 Windows 都及时完成一个挂起的 overlapped I/O。
            ::CancelIoEx(static_cast<HANDLE>(client->pipe_handle), nullptr);
            ::DisconnectNamedPipe(static_cast<HANDLE>(client->pipe_handle));
        }
    }
}

void NamedPipeServer::join_clients() {
    std::vector<std::unique_ptr<ClientState>> remaining;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        remaining.swap(clients_);
    }
    // 收尾：把还挂在上面的连接线程 join 掉。守护进程退出时不带线程残留。
    for (auto& client : remaining) {
        if (client->worker.joinable()) client->worker.join();
    }
}

void NamedPipeServer::serve_client_with(ClientState* client) {
    if (client == nullptr) return;
    HANDLE pipe = static_cast<HANDLE>(client->pipe_handle);
    // 一次连接可能有多条请求（JSON Lines）。读到客户端断开为止。
    std::string buffer;
    try {
        for (;;) {
            char chunk[4096];
            DWORD read = 0;
            if (!read_pipe(pipe, static_cast<HANDLE>(stop_event_), chunk, sizeof(chunk), &read) ||
                read == 0) {
                break;
            }
            buffer.append(chunk, read);
            for (;;) {
                const std::size_t nl = buffer.find('\n');
                if (nl == std::string::npos) break;
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const std::string reply = handle_line(line);
                const std::string framed = reply + "\n";
                if (!write_pipe(pipe, static_cast<HANDLE>(stop_event_), framed.data(),
                                static_cast<DWORD>(framed.size()))) {
                    break;
                }
            }
            if (buffer.size() > 8 * 1024 * 1024) break;   // 客户端发了一行巨长请求，丢弃连接
        }
    } catch (...) {
        // 业务层应该通过 Result 返回设备错误；这里仍保证异常不能跳过
        // 句柄收尾，避免一个坏请求泄漏管道和连接线程。
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        // stop() 也在这把锁下调用 DisconnectNamedPipe，因此这里不会与
        // CloseHandle 竞态。Disconnect 对已经断开的管道失败是无害的。
        if (client->pipe_handle != nullptr) {
            ::DisconnectNamedPipe(static_cast<HANDLE>(client->pipe_handle));
            ::CloseHandle(static_cast<HANDLE>(client->pipe_handle));
            client->pipe_handle = nullptr;
        }
        client->done.store(true, std::memory_order_release);
    }
}

}  // namespace business::businessd
