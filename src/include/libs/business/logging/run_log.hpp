#pragma once

// 运行记录与恢复上下文（能力 7）。
//
// 判定依据：范围报告 §3.7
//   3.7.1 运行记录 / 事件日志 [A]
//   3.7.2 异常恢复上下文快照 [B]
//
// 两条纪律：
//   1. **运行记录与光谱数据、交付记录关联但分离。** 日志不是数据的替代品：
//      一次采集有 spectra（数据）与 delivery（交付）与 log（过程）三份，互不覆盖。
//   2. **恢复上下文 != 重发命令**（任务书 §6.2 红线）。本文件只做快照的写入与查询，
//      **不做任何"自动重发运动/触发"的函数**。也不把异常前保存的位置当作当前实物位置。
//      需要恢复动作时，调用方自己根据快照决定，本类不代劳。

#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/business/time_util.hpp"
#include "libs/error.hpp"

namespace business::logging {

// 运行记录种类。日志按类分流，便于后续按需落不同文件。
enum class RecordKind {
    state_change,       // 业务状态迁移（待机/就绪/采集中/异常/恢复）
    parameter_reply,    // 参数应答（尤其"设置已完成"两态）
    belt_and_position,  // 带号与位置阶段
    distance_observation,  // 测距观测（关联但不复制数据）
    environment,        // 空调状态
    exception_recovery, // 异常与恢复过程
    general,
};
const char* to_string(RecordKind kind) noexcept;

struct RunRecord {
    RecordKind kind = RecordKind::general;
    Milliseconds wall_clock_ms = 0;
    Milliseconds monotonic_ms = 0;   // 用于算间隔，不与墙钟混
    std::string message;              // 中文摘要，可直接写日志
    json::Value fields = json::object();  // 结构化附加字段
};

json::Value to_json(const RunRecord& record);

// 运行记录 / 事件日志。线程安全（守护进程会有多个客户端线程）。
//
// 落盘策略：追加写 JSON Lines（一行一条），**不缓冲到内存等程序退出**——
// 现场断电是常态，未落盘的日志等于没有。
class RunLog {
public:
    explicit RunLog(std::string path);
    ~RunLog();

    RunLog(const RunLog&) = delete;
    RunLog& operator=(const RunLog&) = delete;

    // 打开日志文件。失败不抛异常——日志系统不该把业务进程拖垮，
    // 但要在 enabled()==false 时让调用方知道"没在记日志"。
    libs::VoidResult open();

    bool enabled() const noexcept;

    // 记一条。落盘失败时返回错误（调用方可决定要不要报警），
    // 但**不阻止业务流程**。
    libs::VoidResult write(RecordKind kind, std::string message, json::Value fields = json::object());

    std::int64_t count() const noexcept;   // 已记条数

private:
    std::string path_;
    std::ofstream out_;
    mutable std::mutex mutex_;
    std::int64_t count_ = 0;
};

// ── 3.7.2 异常恢复上下文快照 [B]───────────────────────────────────────────
//
// 只做"写入 + 查询"。**没有自动重发。** 这是任务书 §6.2 的硬红线，
// 也是本类最容易被误用的地方：不要加一个 `auto_resume()`。

// 一次快照里"此刻是什么"的事实。全部是**观测值**，不是期望值。
struct RecoveryContext {
    Milliseconds snapshot_at_ms = 0;

    // 异常发生时的机器状态与门禁结论（来自能力 4）。
    std::string machine_state;
    std::vector<std::string> blocked_by;   // 门禁拦截原因
    bool gate_allowed = false;

    // 异常前"业务上处于"什么。这些是**业务上下文**，
    // **不是**"把设备恢复到这些状态"的命令队列。
    std::string target_belt;
    std::string detection_stage;
    std::int64_t belt_sequence = -1;

    // 异常前最后一笔参数（软件已保存值）。注意这**不等于**设备实际生效值。
    json::Value last_parameters = json::object();

    // 异常前已保存的数据位置。用于人工复盘与"从哪续上"，
    // **不用于自动重发**。
    std::string last_saved_capture_path;
    std::int64_t last_shot_id = -1;

    // 为什么建的快照、异常摘要。
    std::string reason;
};

// 快照仓库。写入 + 查询 + 列出。没有"恢复执行"接口，这是刻意的。
class RecoveryContextStore {
public:
    explicit RecoveryContextStore(std::string directory);

    // 落一份快照。返回它的绝对路径。
    libs::Result<std::string> write(const RecoveryContext& context);

    // 列出已有快照路径，按时间倒序（最新的在前）。
    libs::Result<std::vector<std::string>> list() const;

    // 读一份快照。文件不存在返回 invalid_argument（不是"空快照"）。
    libs::Result<RecoveryContext> read(const std::string& path) const;

    const std::string& directory() const noexcept { return directory_; }

private:
    std::string directory_;
};

json::Value to_json(const RecoveryContext& context);
libs::Result<RecoveryContext> from_json(const json::Value& value);

}  // namespace business::logging

