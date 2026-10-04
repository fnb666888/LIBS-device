// 业务层 IPC 回归测试：协议面 + 一条真实的 Windows 命名管道线路。
//
// 大多数用例直接调用 handle_line()，这样不依赖设备，也能稳定覆盖错误形状。
// 文件末尾另有一个真实管道用例：它启动 NamedPipeServer，像 UI 一样反复
// CreateFile/WriteFile/ReadFile，并在停止后检查句柄是否回收。这个用例专门守住
// "accept 不能有窗口"、"短连接不能泄漏句柄/线程" 和 "多客户端能并发排队"。
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "libs/business/app/runtime.hpp"
#include "libs/business/json/json.hpp"
#include "libs/businessd/named_pipe_server.hpp"

using namespace business;

namespace {

// 一个最小的真实管道客户端。它不复用 UI 的 PipeClient，避免两边共用同一个
// bug；测试只依赖 Win32 API 和 JSON Lines 线路本身。
class WireClient {
public:
    explicit WireClient(const std::string& pipe_name) : pipe_name_(pipe_name) {}

    bool request(const std::string& line, std::string* reply, DWORD timeout_ms = 3000) const {
        HANDLE pipe = connect(timeout_ms);
        if (pipe == INVALID_HANDLE_VALUE) return false;

        const std::string framed = line + "\n";
        DWORD written = 0;
        const bool sent = WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()),
                                    &written, nullptr) && written == framed.size();
        if (!sent) {
            CloseHandle(pipe);
            return false;
        }

        std::string buffer;
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        char chunk[1024];
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                CloseHandle(pipe);
                return false;
            }
            if (available == 0) {
                if (GetTickCount64() >= deadline) {
                    CloseHandle(pipe);
                    return false;
                }
                Sleep(1);
                continue;
            }
            DWORD read = 0;
            if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
                CloseHandle(pipe);
                return false;
            }
            buffer.append(chunk, read);
            const std::size_t nl = buffer.find('\n');
            if (nl != std::string::npos) {
                if (reply != nullptr) *reply = buffer.substr(0, nl);
                CloseHandle(pipe);
                return true;
            }
        }
    }

    // Open-and-close without sending data. This wakes ConnectNamedPipe after stop().
    bool wake(DWORD timeout_ms = 3000) const {
        HANDLE pipe = connect(timeout_ms);
        if (pipe == INVALID_HANDLE_VALUE) return false;
        CloseHandle(pipe);
        return true;
    }

private:
    HANDLE connect(DWORD timeout_ms) const {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            HANDLE pipe = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return pipe;

            const DWORD error = GetLastError();
            if (GetTickCount64() >= deadline) return INVALID_HANDLE_VALUE;
            if (error == ERROR_PIPE_BUSY) {
                const DWORD remaining = static_cast<DWORD>(deadline - GetTickCount64());
                (void)WaitNamedPipeA(pipe_name_.c_str(), remaining > 100 ? 100 : remaining);
            } else {
                // ERROR_FILE_NOT_FOUND is expected while the accept loop is creating
                // its next instance. Do not turn that tiny window into a test failure.
                Sleep(5);
            }
        }
    }

    std::string pipe_name_;
};

class WireFixture {
public:
    explicit WireFixture(const std::string& tag)
        : server(runtime, businessd::DaemonOptions{
                              "\\\\.\\pipe\\libs-test-wire-" + tag + "-" +
                              std::to_string(GetCurrentProcessId())}),
          client(server.options().pipe_name) {
        app::Runtime::Options options;
        options.open_devices = false;
        options.data_dir = std::string("build/tmp_ipc_wire_") + tag;
        (void)runtime.start(config::default_template().value(), options);
        running = true;
        thread = std::thread([this] {
            result = server.run();
            running = false;
        });
    }

    ~WireFixture() { stop(); }

    WireFixture(const WireFixture&) = delete;
    WireFixture& operator=(const WireFixture&) = delete;

    bool ready() const { return client.wake(); }

    void stop() {
        if (!thread.joinable()) return;
        server.stop();
        // stop() 本身会 SetEvent/CancelIoEx，额外的短连接只保留为兼容性
        // 烟雾动作：无论 accept 是否已经进入下一实例，都必须能返回 join。
        (void)client.wake();
        thread.join();
        running = false;
        (void)runtime.stop();
    }

    app::Runtime runtime;
    businessd::NamedPipeServer server;
    WireClient client;
    std::thread thread;
    std::atomic<bool> running{false};
    libs::VoidResult result;
};

DWORD process_handle_count() {
    DWORD count = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

struct Fixture {
    app::Runtime runtime;
    businessd::NamedPipeServer server;
    std::string data_dir;

    explicit Fixture(const std::string& tag)
        : server(runtime, businessd::DaemonOptions{"\\\\.\\pipe\\libs-test-" + tag}) {
        data_dir = std::string("build/tmp_ipc_") + tag;
        app::Runtime::Options options;
        options.open_devices = false;
        options.data_dir = data_dir;
        (void)runtime.start(config::default_template().value(), options);
    }
};

json::Value parse_reply(const std::string& text, bool* ok) {
    json::Value v;
    *ok = json::parse(text, v).ok;
    return v;
}

}  // namespace

TEST_MAIN_BEGIN()

CASE(reply_is_a_single_line_of_json); {
    // JSON Lines：一行一个响应，绝不能带换行，否则客户端会读串。
    Fixture f("line");
    const std::string reply = f.server.handle_line(R"({"command":"ping","args":{}})");
    CHECK(reply.find('\n') == std::string::npos);
    bool ok = false;
    const json::Value v = parse_reply(reply, &ok);
    CHECK(ok);
}

CASE(ping_over_the_pipe_succeeds); {
    Fixture f("ping");
    bool ok = false;
    const json::Value v = parse_reply(f.server.handle_line(R"({"command":"ping","args":{}})"), &ok);
    CHECK(ok);
    CHECK(v["ok"].as_bool());
    CHECK(v["payload"]["pong"].as_bool());
    // 成功响应不应带错误字段。
    CHECK(!v.has("error_kind"));
}

CASE(bad_json_over_the_pipe_reports_invalid_argument); {
    Fixture f("bad");
    bool ok = false;
    const json::Value v = parse_reply(f.server.handle_line("{oops"), &ok);
    CHECK(ok);            // 协议层仍然回一行合法 JSON
    CHECK(!v["ok"].as_bool(true));
    CHECK_EQ(v["error_kind"].as_string(), std::string("invalid_argument"));
    CHECK(!v["error"].as_string().empty());
    CHECK(v.has("error_code"));
}

CASE(unknown_command_over_the_pipe_reports_unsupported); {
    Fixture f("unknown");
    bool ok = false;
    const json::Value v = parse_reply(
        f.server.handle_line(R"({"command":"selfdestruct","args":{}})"), &ok);
    CHECK(ok);
    CHECK(!v["ok"].as_bool(true));
    CHECK_EQ(v["error_kind"].as_string(), std::string("unsupported"));
}

CASE(empty_line_does_not_crash_the_server); {
    Fixture f("empty");
    bool ok = false;
    const json::Value v = parse_reply(f.server.handle_line(""), &ok);
    CHECK(ok);
    CHECK(!v["ok"].as_bool(true));
    // 服务端还得能继续回答。
    bool ok2 = false;
    const json::Value v2 = parse_reply(f.server.handle_line(R"({"command":"ping","args":{}})"), &ok2);
    CHECK(ok2);
    CHECK(v2["ok"].as_bool());
}

CASE(health_over_the_pipe_does_not_open_devices); {
    Fixture f("health");
    bool ok = false;
    const json::Value v = parse_reply(f.server.handle_line(R"({"command":"health","args":{}})"), &ok);
    CHECK(ok);
    CHECK(v["ok"].as_bool());
    // 全禁用模板：逐个如实报告没有配置、没有打开。
    // all_required_open 在"没要求接任何设备"时为真是对的。
    const char* devices[] = {"laser", "spectrometer"};
    for (const char* name : devices) {
        CHECK(!v["payload"][name]["configured"].as_bool(true));
        CHECK(!v["payload"][name]["opened"].as_bool(true));
    }
    CHECK(!v["payload"]["summary"].as_string().empty());
}

CASE(commands_that_touch_devices_do_not_deadlock); {
    // 回归测试：handle_line 曾经自己也取 device_mutex，而 gate/capture/health
    // 在门面里还要再取一次。std::mutex 不可重入 —— 守护进程当时会整个挂死。
    // 这三个命令就是当年挂死的那三条路径，逐个跑一遍确认能返回。
    Fixture f("lock");
    const char* commands[] = {
        R"({"command":"health","args":{}})",
        R"({"command":"gate","args":{"action":"refresh"}})",
        R"({"command":"status","args":{}})",
    };
    for (const char* request : commands) {
        bool ok = false;
        const json::Value v = parse_reply(f.server.handle_line(request), &ok);
        CHECK(ok);
        CHECK(v.has("ok"));
    }
}

CASE(real_named_pipe_round_trip_and_accept_wakeup); {
    WireFixture f("round_trip");
    CHECK(f.ready());

    std::string reply;
    CHECK(f.client.request(R"({"command":"ping","args":{}})", &reply));
    bool ok = false;
    const json::Value value = parse_reply(reply, &ok);
    CHECK(ok);
    CHECK(value["ok"].as_bool());
    CHECK(value["payload"]["pong"].as_bool());

    // stop() 必须能唤醒阻塞中的 ConnectNamedPipe；否则这里会永远 join 不回来。
    f.stop();
    CHECK(!f.running.load());
    CHECK(f.result.ok());
}

CASE(real_named_pipe_handles_concurrent_short_connections_without_leaking); {
    WireFixture f("concurrent");
    CHECK(f.ready());
    const DWORD before = process_handle_count();

    constexpr int kWorkers = 8;
    constexpr int kRequestsPerWorker = 8;
    std::atomic<int> successes{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&] {
            for (int i = 0; i < kRequestsPerWorker; ++i) {
                std::string reply;
                if (!f.client.request(R"({"command":"ping","args":{}})", &reply)) continue;
                bool ok = false;
                const json::Value value = parse_reply(reply, &ok);
                if (ok && value["ok"].as_bool(false) && value["payload"]["pong"].as_bool(false)) {
                    ++successes;
                }
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    CHECK_EQ(successes.load(), kWorkers * kRequestsPerWorker);

    f.stop();
    const DWORD after = process_handle_count();
    // std::thread/CRT/pipe implementation may retain a tiny fixed amount of
    // bookkeeping, but one HANDLE per short connection is never acceptable.
    CHECK(after <= before + 4);
}

TEST_MAIN_RETURN()
