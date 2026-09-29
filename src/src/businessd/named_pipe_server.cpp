#include "libs/businessd/named_pipe_server.hpp"

#include <windows.h>

#include <aclapi.h>

#include <sddl.h>

#include <string>
#include <vector>

#include "libs/business/services/facade.hpp"

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

constexpr DWORD kPipeDirection = PIPE_ACCESS_DUPLEX;
constexpr DWORD kPipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
constexpr DWORD kPipeInstances = PIPE_UNLIMITED_INSTANCES;
constexpr DWORD kPipeOutBuffer = 64 * 1024;
constexpr DWORD kPipeInBuffer = 64 * 1024;
constexpr DWORD kPipeTimeout = 5000;   // 客户端不读写就断开，不留半开连接

}  // namespace

NamedPipeServer::NamedPipeServer(Runtime& runtime, DaemonOptions options)
    : runtime_(runtime), options_(std::move(options)) {}

NamedPipeServer::~NamedPipeServer() { stop(); }

void NamedPipeServer::stop() { stopping_.store(true); }

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

    while (!stopping_.load()) {
        HANDLE pipe = CreateNamedPipeA(options_.pipe_name.c_str(), kPipeDirection, kPipeMode,
                                        kPipeInstances, kPipeOutBuffer, kPipeInBuffer, kPipeTimeout,
                                        security.attributes());
        if (pipe == INVALID_HANDLE_VALUE) {
            join_clients();
            return libs::Error::make(libs::ErrorKind::transport, "pipe_create_failed",
                                     "无法创建命名管道 " + options_.pipe_name);
        }
        // 等待客户端连接。可被 stop() 用一个哨兵实例唤醒。
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) {
            CloseHandle(pipe);
            if (stopping_.load()) break;
            continue;
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
        clients_.emplace_back([this, pipe]() { serve_client_with(pipe); });
    }
    join_clients();
    return libs::VoidResult();
}

void NamedPipeServer::join_clients() {
    // 收尾：把还挂在上面的连接线程 join 掉。守护进程退出时不带线程残留。
    for (std::thread& t : clients_) {
        if (t.joinable()) t.join();
    }
    clients_.clear();
}

void NamedPipeServer::serve_client_with(void* pipe_handle) {
    HANDLE pipe = static_cast<HANDLE>(pipe_handle);
    // 一次连接可能有多条请求（JSON Lines）。读到客户端断开为止。
    std::string buffer;
    for (;;) {
        char chunk[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
        buffer.append(chunk, read);
        for (;;) {
            const std::size_t nl = buffer.find('\n');
            if (nl == std::string::npos) break;
            std::string line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const std::string reply = handle_line(line);
            DWORD written = 0;
            const std::string framed = reply + "\n";
            WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
        }
        if (buffer.size() > 8 * 1024 * 1024) break;   // 客户端发了一行巨长请求，丢弃连接
    }
}

}  // namespace business::businessd
