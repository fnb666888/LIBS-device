#pragma once

// 对外唯一门面（`business/app/`）—— **进程边界，不属于服务层**。
//
// 范围报告 §5.2：守护进程独占设备 + 薄客户端经命名管道 JSON 通信。
// 因此**本门面从第一天起就是跨进程可调用的**：
//   * 入参出参都是可序列化的值类型（json::Value 或纯值结构），不持有裸句柄；
//   * 不把 std::string 之外的东西泄露给 IPC 层；
//   * 每个方法都对应一条 CLI 子命令，`lpc-cli` 只是它的一层薄壳。
//
// ── 本门面不做的事（避免把判定权散到各处）─────────────────────────────
//
//   * **不先问门禁再自己决定要不要开始检测**。
//     这是被明令禁止的写法（方案 §2 约束 1）：一旦这么写，判定就有了两个执行点，
//     将来把判定搬进常驻进程时必须先重写一遍才能收回来。
//     正确形态是 `measurement_start` 一次性成功或失败：
//     成功就当成"已开光"，失败原因看返回码。`gate` / `status` 只读**缓存**
//     （`monitor()->latest()`），供界面显示，**不参与决策**。
//   * 不自己算成分（算法服务已裁剪，属未来扩展）；
//   * 不自己重发恢复命令（只给快照）。

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libs/business/app/runtime.hpp"
#include "libs/business/shared/json/json.hpp"
#include "libs/business/shared/data_delivery.hpp"
#include "libs/business/measurement/frame.hpp"
#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/job.hpp"
#include "libs/business/readiness/monitor.hpp"
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

    // 失败但**附带业务报告**。
    //
    // 为什么需要这一条：预检不放行时，操作员真正要看的不是"失败"两个字，
    // 而是"哪几条不满足、灯亮没亮"。如果只有 failure()，报告就只能塞进
    // message 字符串里，界面只能整段显示文本，无法分字段渲染。
    //
    // **报告放 payload.report 子对象**，不铺在顶层。顶层 error 键
    // 已被 failure() 占用，而报告里的诊断字段（如 detail / lamp_error）
    // 一旦撞名，谁后 set 谁赢就成了一句碰运气的话 —— 把两者隔到不同
    // 层级，冲突就不可能发生。
    static Response failure_with_payload(libs::Error error, json::Value report);
};

// 门禁结论 -> JSON（status / gate 共用同一份形状）。
//
// **为什么门面自己拼而不是让 readiness 提供**：readiness 只回答"能不能出光"，
// 而 status 面板要的是"现场现在什么状态"。两者字段有重叠但不相同，
// 在门面拼一层就够，不要把展示字段倒灌进纯函数层。
json::Value gate_snapshot_json(const readiness::ReadinessMonitor& monitor);

// 门面。持有一个 Runtime（设备 + 各业务），把请求路由到对应能力。
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
    // 业务一（设备预检）的 IPC 出口。
    //
    // 预检服务本身在 Runtime::start() 里就装配好了，但**在这条命令出现之前
    // 它没有任何跨进程调用者**：也就是说这台机器能判"能不能出光"，
    // 却没有任何一个客户端能主动让它判一次。界面上那颗"设备预检"按钮
    // 只能去问 gate 缓存，而 gate 缓存要等别的命令先跑过判定才有内容。
    //
    // 这条命令补上的是**唯一缺失的那一环**：显式触发一次完整预检
    // （布防 -> 判定 -> 刷灯），并把完整报告如实回给调用方。
    Response cmd_precheck(const json::Value& args);
    Response cmd_param_set(const json::Value& args);
    Response cmd_param_get(const json::Value& args);
    Response cmd_capture(const json::Value& args);
    Response cmd_spectrum_stream(const json::Value& args);
    // 连续出光测量作业：出光 + 逐发采集 + 落盘 + 实时显示。
    // 这是现场真正要跑的那一个流程，也是**全库唯一能开光闸的入口**。
    //
    // 门禁判定在 job->start() 内部完成；门面**不得**在调它之前自己去问一遍。
    Response cmd_measurement_start(const json::Value& args);
    Response cmd_measurement_stop(const json::Value& args);
    Response cmd_measurement_status(const json::Value& args);
    Response cmd_recovery_list(const json::Value& args);
    Response cmd_recovery_write(const json::Value& args);
    Response cmd_health(const json::Value& args);

    // 把一次采集结果交付到本地出口（能力 6.3.1）。
    //
    // 返回空 = 已落盘，status_out 填好供上送；
    // 返回非空 = 未落盘（status_out 仍填好）：这一发的数据丢了，调用方必须报失败。
    libs::VoidResult deliver_capture(const std::vector<libs::devices::ScanGroup>& groups,
                                     data_delivery::DeliveryStatus& status_out);

    // 解析本次出光参数：参数库 + 请求覆盖。
    // 覆盖只允许改会话参数（电流/脉宽/频率/延时/Q 延时/Q 预热）与光谱仪的
    // 积分时间/等触发超时，且走**与参数库同一套**量程校验。
    // 成功返回 true；失败返回 false 并把原因写进 error。
    bool parse_fire_parameters(const json::Value& args, measurement::FireParameters& out,
                               libs::Error& error);

    Runtime& runtime_;
};

}  // namespace business::app