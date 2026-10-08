#pragma once

// 对外唯一门面（`business/services/`）。
//
// 范围报告 §5.2：守护进程独占设备 + 薄客户端经命名管道 JSON 通信。
// 因此**本门面从第一天起就是跨进程可调用的**：
//   * 入参出参都是可序列化的值类型（json::Value 或纯值结构），不持有裸句柄；
//   * 不把 std::string 之外的东西泄露给 IPC 层；
//   * 每个方法都对应一条 CLI 子命令，`lpc-cli` 只是它的一层薄壳。
//
// **本门面不做的事**（避免把判定权散到各处）：
//   * 不自己判门禁（调 Gate）；
//   * 不自己算有矿/成分（调服务模块，C 类返回 unavailable）；
//   * 不自己重发恢复命令（只给快照）。

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libs/business/app/runtime.hpp"
#include "libs/business/json/json.hpp"
#include "libs/business/services/data_delivery.hpp"
#include "libs/business/services/laser_fire.hpp"
#include "libs/error.hpp"

namespace business::app {

// 一次 JSON 请求的响应。
// 约定：**永远返回 JSON**，成功失败都返回；错误放在 error 字段里。
// 这样 CLI 不用区分"传输失败"和"业务失败"两种退出路径。
struct Response {
    bool ok = false;
    json::Value payload = json::object();
    libs::Error error{};

    static Response success(json::Value payload);
    static Response failure(libs::Error error);
};

// 门面。持有一个 Runtime（设备 + 各服务），把请求路由到对应能力。
//
// 无状态：单次调用不留下跨调用状态（除了 Runtime 里的设备与参数），
// 这样多客户端并发不会互相踩（守护进程会给每个请求一把锁，见 runtime）。
class BusinessFacade {
public:
    explicit BusinessFacade(Runtime& runtime) : runtime_(runtime) {}

    // 处理一条 JSON 请求。**不抛异常**：任何内部错误都转成 Response::failure。
    Response dispatch(const std::string& request_json);

    // 路由表。供 `lpc-cli` 自省（列出可用子命令）。
    static std::vector<std::string> commands();

private:
    Response handle(const std::string& command, const json::Value& args);
    Response cmd_ping(const json::Value& args);
    Response cmd_config_show(const json::Value& args);
    Response cmd_status(const json::Value& args);
    Response cmd_gate(const json::Value& args);
    Response cmd_param_set(const json::Value& args);
    Response cmd_param_get(const json::Value& args);
    Response cmd_detection_start(const json::Value& args);
    Response cmd_capture(const json::Value& args);
    Response cmd_spectrum_stream(const json::Value& args);
    Response cmd_laser_fire(const json::Value& args);
    Response cmd_laser_run(const json::Value& args);
    Response cmd_laser_stop(const json::Value& args);
    Response cmd_motor(const json::Value& args);
    Response cmd_composition(const json::Value& args);
    Response cmd_recovery_list(const json::Value& args);
    Response cmd_recovery_write(const json::Value& args);
    Response cmd_health(const json::Value& args);
    Response cmd_heike_report_preview(const json::Value& args);
    Response cmd_device_debug(const json::Value& args);
    Response cmd_algorithm(const json::Value& args);
    Response cmd_debug_context(const json::Value& args);
    Response cmd_debug_contract(const json::Value& args);
    Response cmd_shutdown(const json::Value& args);

    // 把一次采集结果交付到本地出口（能力 6.3.1）。
    //
    // **两个产生光谱的命令共用这一处**：capture 与 laser_fire。分开各写一遍
    // 必然出现"一处接了、另一处漏了"—— 那正是"数据只活在管道响应里"的成因。
    //
    // 返回空 = 已落盘，status_out 填好供上送；
    // 返回非空 = 未落盘（status_out 仍填好）：这一发的数据丢了，调用方必须报失败。
    libs::VoidResult deliver_capture(const std::vector<libs::devices::ScanGroup>& groups,
                                     data_delivery::DeliveryStatus& status_out);

    // 解析本次出光参数：站点配置 + 请求覆盖。
    // 覆盖只允许改会话参数（电流/脉宽/频率/延时/Q 延时/Q 预热）与光谱仪的
    // 积分时间/等触发超时，且走**与站点配置同一套**量程校验。
    // 成功返回 true；失败返回 false 并把原因写进 error。
    bool parse_fire_parameters(const json::Value& args, services::FireParameters& out,
                               libs::Error& error);

    Runtime& runtime_;
};

}  // namespace business::app
