#pragma once

// 参数管理与持久化（能力 3）。
//
// 判定依据：范围报告 §3.3 = 3.3.1 参数管理骨架 [A] + 3.3.2 断电保留 [A]
//         + 3.3.3 生效时点仲裁 [B]。
//
// 本能力把任务书 §4.1-4.2 的硬要求变成可离线测的不变量。
//
// **本能力的核心价值是"参数设置已完成"的两态语义**（范围报告 §3.3.1）：
// 收到设置请求 != 设置完成。落盘失败、校验失败两条路径**全部**必须走
// "未完成"。任务书 §4.2 明文要求失败不能返回成功应答。
//
// 纪律：
//   1. 各参数分组的**具体字段**由任务书 §4.1 后续细化，当前用"字段存在性"
//      表达"核心参数是否齐备"，不臆造字段名之外的语义。
//   2. 断电写入一致性规则任务书 §4.3 待细化：按"先落盘再应答"的保守策略
//      实现，并在注释里标注该策略尚未确认。
//   3. **读取到已保存参数 != 硬件已加载，更 != 上电自动发光或运动。**
//
// ── 曾经在这里、现已删除的东西 ─────────────────────────────────────────
//
//   * `ParameterGroup::labels` / `FileParameterStore::units_`：
//     字段名 -> 工程单位的映射表。它唯一的去处是 param_get 的回显，
//     而界面上的单位本来就是前端自己写的。于是同一个单位有两个来源。
//   * `ApplyTiming`（原本是 `immediately | at_next_idle`）：
//     at_next_idle **全库没有任何提交点** —— 暂存区永远不会被 commit，
//     于是操作员在界面上调完参数、界面显示"已暂存"，而出光时读的仍是旧值。
//     沉默的"改了不生效"比报错更坏。连同 staged 机制一起删除，
//     现在参数一律 apply 即写即生效。

#include <map>
#include <string>
#include <vector>

#include "libs/error.hpp"

namespace business::config {

// 任务书 §4.1 的参数分组。字段未细化，因此这里只承载"分组 + 值 + 是否齐备"。
struct ParameterGroup {
    std::map<std::string, double> values;    // 字段名 -> 值（单位由字段名自解释）
    bool complete = false;                   // 核心字段是否齐备

    bool has(const std::string& field) const { return values.count(field) != 0; }
};

// 一次"设置/覆盖/查询"的结果。
//
// 关键字段：completed
//   true  = 已确认写入并生效
//   false = 收到请求了，但没完成。调用方**必须**检查这个字段。
enum class ApplyState { completed, rejected, failed };

const char* to_string(ApplyState state) noexcept;

struct ApplyResult {
    ApplyState state = ApplyState::rejected;
    std::string group;
    std::string message;                 // 中文原因，可直接写日志
    std::vector<std::string> failed_fields;

    bool completed() const noexcept { return state == ApplyState::completed; }
};

// 核心参数是否齐备的判定结果。缺字段时逐项列出，便于 UI 提示。
struct ReadinessReport {
    bool ready = false;
    std::vector<std::string> missing_groups;
    std::vector<std::string> missing_fields;   // "group.field"
    std::string summary;
};

// 参数存储。默认实现是 JSON 落盘（断电保留，能力 3.3.2）。
class ParameterStore {
public:
    virtual ~ParameterStore() = default;

    // 覆盖/设置一个分组的若干字段。**立即生效并先落盘**。
    virtual ApplyResult apply(const std::string& group,
                              const std::map<std::string, double>& values) = 0;

    // 查询一个分组的当前值。分组不存在返回 rejected。
    virtual ApplyResult query(const std::string& group,
                              std::map<std::string, double>& out) const = 0;

    // 全部分组。用于回显与落盘。
    virtual std::map<std::string, ParameterGroup> all() const = 0;

    // 启动时加载。文件不存在是正常情况（首次运行），不算错误。
    virtual libs::VoidResult load() = 0;

    virtual ReadinessReport readiness() const = 0;
};

// 文件存储：落盘到指定路径，原子替换（先写临时文件再 rename）。
// 任务书 §4.3 的一致性规则待细化，本实现按保守策略并在注释标注。
class FileParameterStore : public ParameterStore {
public:
    // required_groups: 判定"核心参数齐备"所必需的分组名。
    // required_fields[group]: 该分组内必须存在的字段。
    // 未确认的字段由调用方按任务书 §4.1 后续细化补齐。
    FileParameterStore(std::string path,
                       std::vector<std::string> required_groups,
                       std::map<std::string, std::vector<std::string>> required_fields);

    ApplyResult apply(const std::string& group,
                      const std::map<std::string, double>& values) override;
    ApplyResult query(const std::string& group, std::map<std::string, double>& out) const override;
    std::map<std::string, ParameterGroup> all() const override;
    libs::VoidResult load() override;
    ReadinessReport readiness() const override;

private:
    // 落盘。成功后才允许应答 completed（先落盘再应答的保守策略）。
    //
    // **刻意不是接口的一部分**：全库只有 apply() 在写完之后调它，
    // 没有任何调用方需要在 apply 之外强制落盘。留在 public virtual 上
    // 等于对外承诺一个"绕过参数校验也能落盘"的入口。
    libs::VoidResult persist();

    std::string path_;
    std::vector<std::string> required_groups_;
    std::map<std::string, std::vector<std::string>> required_fields_;
    std::map<std::string, ParameterGroup> groups_;
};

}  // namespace business::config