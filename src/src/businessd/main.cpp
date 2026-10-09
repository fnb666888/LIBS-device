#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "libs/business/app/runtime.hpp"
#include "libs/business/shared/config/site_config.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/businessd/named_pipe_server.hpp"

namespace {

constexpr const char* kUsage =
    "businessd - libs 业务层守护进程\n"
    "\n"
    "用法:\n"
    "  businessd [--config <file>] [--data-dir <dir>] [--pipe <name>] [--no-devices]\n"
    "  businessd --print-default-config\n"
    "\n"
    "选项:\n"
    "  --config <file>       站点配置文件（不给则用零默认值，并会显式告警）\n"
    "  --data-dir <dir>      数据、日志、恢复快照的根目录（默认 data）\n"
    "  --pipe <name>         命名管道名（默认 \\\\.\\pipe\\libs-businessd）\n"
    "  --no-devices          不打开任何设备，只跑纯软件能力（离线自检用）\n"
    "  --print-default-config 输出一份全关闭的默认配置模板\n"
    "\n"
    "重要:\n"
    "  设备由本进程独占，客户端请走 lpc.py 经命名管道发 JSON 请求。\n"
    "  所有命令都返回 JSON：成功失败都返回，错误在 error 字段里。\n";

void print_usage() { std::fputs(kUsage, stdout); }

// Ctrl+C / Ctrl+Break / 退出消息体——只置位标志，真正的关闭在 run() 返回后。
std::atomic<bool>* g_stop_flag = nullptr;

BOOL WINAPI console_handler(DWORD signal) {
    if (g_stop_flag != nullptr) g_stop_flag->store(true);
    (void)signal;
    return TRUE;
}

void report_error(const libs::Error& error) {
    std::fprintf(stderr, "参数错误：[%s/%s] %s\n", libs::to_string(error.kind),
                 error.code.c_str(), error.message.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    using namespace business;
    try {
        std::string config_path;
        app::Runtime::Options options;
        businessd::DaemonOptions daemon;
        bool print_default_config = false;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto next = [&](const char* name) -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error(std::string("命令 ") + name + " 缺少参数值");
                }
                return argv[++i];
            };
            if (arg == "--config") {
                config_path = next("--config");
            } else if (arg == "--data-dir") {
                options.data_dir = next("--data-dir");
            } else if (arg == "--pipe") {
                daemon.pipe_name = next("--pipe");
            } else if (arg == "--no-devices") {
                options.open_devices = false;
            } else if (arg == "--print-default-config") {
                print_default_config = true;
            } else if (arg == "-h" || arg == "--help") {
                print_usage();
                return 0;
            } else {
                std::fprintf(stderr, "不认识参数：%s\n", arg.c_str());
                print_usage();
                return 2;
            }
        }

        if (print_default_config) {
            // 用 default_template()：全设备关闭的合法配置，能通过 validate()。
            // 零值 SiteConfig{} 不是合法配置（没有 station_id），不能拿来当模板。
            const libs::Result<config::SiteConfig> template_result = config::default_template();
            if (!template_result.ok()) {
                report_error(template_result.error());
                return 1;
            }
            const libs::Result<std::string> saved = config::save_string(template_result.value());
            if (!saved.ok()) {
                report_error(saved.error());
                return 1;
            }
            std::fputs(saved.value().c_str(), stdout);
            return 0;
        }

        config::SiteConfig site;
        if (!config_path.empty()) {
            auto loaded = config::load_file(config_path);
            if (!loaded.ok()) {
                report_error(loaded.error());
                return 1;
            }
            site = std::move(loaded.value());
        } else {
            // 没给配置也能起，但会有一处被标注的行为缺失——如实告知，不静默兜底。
            std::fprintf(stderr,
                         "注意：未指定 --config，使用零默认配置（所有设备均未配置，"
                         "health 会如实报告未打开）。\n");
        }

        app::Runtime runtime;
        const libs::VoidResult started = runtime.start(site, options);
        if (!started.ok()) {
            report_error(started.error());
            return 1;
        }

        std::atomic<bool> stop_flag{false};
        g_stop_flag = &stop_flag;
        SetConsoleCtrlHandler(console_handler, TRUE);

        businessd::NamedPipeServer server(runtime, daemon);
        // stop() 只置位标志，要把它接到 run() 的等待上——所以这里不能只看 stop_flag。
        std::thread watcher([&server, &stop_flag, &runtime] {
            while (!stop_flag.load() && !runtime.shutdown_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            server.stop();
        });

        std::fprintf(stdout, "businessd 就绪：%s\n", daemon.pipe_name.c_str());
        std::fflush(stdout);
        const libs::VoidResult served = server.run();

        stop_flag.store(true);
        watcher.join();
        SetConsoleCtrlHandler(console_handler, FALSE);
        g_stop_flag = nullptr;
        const libs::VoidResult stopped = runtime.stop();
        if (!served.ok()) {
            report_error(served.error());
            return 1;
        }
        if (!stopped.ok()) {
            report_error(stopped.error());
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "启动时异常：%s\n", e.what());
        return 1;
    }
}