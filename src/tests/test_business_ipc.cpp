// 业务层离线测试（四）：命名管道的线路协议。
//
// handle_line() 是公开的，所以整套测试**不真的建管道**：
// 只验证"一行请求 -> 一行 JSON 响应"这个协议面，含错误形状。
// 真正的 Win32 管道/ACL 由人工与冒烟测试覆盖。
#include <string>

#include "check.hpp"
#include "libs/business/app/runtime.hpp"
#include "libs/business/json/json.hpp"
#include "libs/businessd/named_pipe_server.hpp"

using namespace business;

namespace {

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

TEST_MAIN_RETURN()