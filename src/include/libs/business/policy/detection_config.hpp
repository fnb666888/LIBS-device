#pragma once

// 参数管理与持久化（能力 3）。
//
// 判定依据：范围报告 §3.3 = 3.3.1 参数管理骨架 [A] + 3.3.2 断电保留 [A]
//         + 3.3.3 生效时点仲裁 [B]。
//
// 本能力把任务书 §4.1-4.2 的硬要求变成可离线测的不变量。
//
// **本能力的核心价值是"参数设置已完成"的两态语义**（范围报告 §3.3.1）：
// 收到设置请求 != 设置完成。设备应用失败、校验失败、持久化失败三条路径
// **全部**必须走"未完成"。任务书 §4.2 明文要求失败不能返回成功应答。
//
// 纪律：
//   1. 各参数分组的**具体字段**由任务书 §4.1 后续细化，当前用"字段存在性"
//      表达"核心参数是否齐备"，不臆造字段名之外的语义。
//   2. 断电写入一致性规则任务书 §4.3 待细化：按"先落盘再应答"的保守策略
//      实现，并在注释里标注该策略尚未确认。
//   3. **读取到已保存参数 != 硬件已加载，更 != 上电自动发光或运动。**

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "libs/business/json/json.hpp"
#include "libs/error.hpp"

namespace business::policy {

// 任务书 §4.1 的参数分组。字段未细化，因此这里只承载"分组 + 值 + 是否已设置"。
struct ParameterGroup {
    std::string name;                        // 如 "spectrometer" / "laser" / "trigger"
    std::map<std::string, double> values;    // 字段名 -> 值（单位随字段名，见字段说明）
    std::map<std::string, std::string> labels;  // 字段名 -> 工程单位，仅用于回显
    bool complete = false;                   // 核心字段是否齐备

    bool has(const std::string& field) const { return values.count(field) != 0; }
};

// 一次"设置/覆盖/查询"的结果。
//
// 关键字段：completed
//   true  = 已确认写入并生效
//   false = 收到请求了，但没完成。调用方**必须**检查这个字段。
// staged 是单独一档，不能并进 completed：
// at_next_idle 策略下参数只是**暂存**，尚未生效。此时报 completed=true
// 就是"说已完成但其实没完成"，正是本文件要防的那种撒谎。
enum class ApplyState { completed, staged, rejected, failed };

const char* to_string(ApplyState state) noexcept;

struct ApplyResult {
    ApplyState state = ApplyState::rejected;
    std::string group;
    std::string message;                 // 中文原因，可直接写日志
    std::vector<std::string> failed_fields;

    bool completed() const noexcept { return state == ApplyState::completed; }
};

// 运行中改参的生效时点策略（范围报告 §3.3.3，B 类）。
//
// 任务书 §3 步骤 7、§4.2：生效时点及是否需要采集暂停**尚未确定**。
// 两种策略并存，默认保守。后续确认后删掉一个即可。
enum class ApplyTiming { at_next_idle, immediately };
const char* to_string(ApplyTiming timing) noexcept;

// 核心参数是否齐备的判定结果。缺字段时逐项列出，便于 UI 提示。
struct ReadinessReport {
    bool ready = false;
    std::vector<std::string> missing_groups;
    std::vector<std::string> missing_fields;   // "group.field"
    std::string summary;
};

// 参数存储。默认实现是内存 + JSON 落盘（断电保留，能力 3.3.2）。
class ParameterStore {
public:
    virtual ~ParameterStore() = default;

    // 覆盖/设置一个分组的若干字段。空字段名列表表示"整组替换"。
    virtual ApplyResult apply(const std::string& group,
                              const std::map<std::string, double>& values,
                              ApplyTiming timing) = 0;

    // 查询一个分组的当前值。分组不存在返回 rejected。
    virtual ApplyResult query(const std::string& group,
                              std::map<std::string, double>& out) const = 0;

    // 全部分组。用于回显与落盘。
    virtual std::map<std::string, ParameterGroup> all() const = 0;

    // 落盘。成功后才允许应答 completed（先落盘再应答的保守策略）。
    virtual libs::VoidResult persist() = 0;

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

    ApplyResult apply(const std::string& group, const std::map<std::string, double>& values,
                      ApplyTiming timing) override;
    ApplyResult query(const std::string& group, std::map<std::string, double>& out) const override;
    std::map<std::string, ParameterGroup> all() const override;
    libs::VoidResult persist() override;
    libs::VoidResult load() override;
    ReadinessReport readiness() const override;

    // 待生效的改动（at_next_idle 策略下 accumulate 在这里）。
    bool has_pending() const;
    // 在下一个空闲点提交待生效改动。返回是否有改动被提交。
    bool commit_pending();

    // 各字段的工程单位。用于回显，避免上层猜单位。
    void set_unit(const std::string& group, const std::string& field, const std::string& unit);

private:
    ApplyResult apply_impl(const std::string& group,
                           const std::map<std::string, double>& values,
                           bool staged);

    std::string path_;
    std::vector<std::string> required_groups_;
    std::map<std::string, std::vector<std::string>> required_fields_;
    std::map<std::string, ParameterGroup> groups_;
    std::map<std::string, std::map<std::string, double>> pending_;   // at_next_idle 暂存
    std::map<std::string, std::map<std::string, std::string>> units_;
};

// 内存存储：只用于单元测试与"本次运行不落盘"的场景。
class MemoryParameterStore : public ParameterStore {
public:
    explicit MemoryParameterStore(std::vector<std::string> required_groups = {});
    ApplyResult apply(const std::string& group, const std::map<std::string, double>& values,
                      ApplyTiming timing) override;
    ApplyResult query(const std::string& group, std::map<std::string, double>& out) const override;
    std::map<std::string, ParameterGroup> all() const override { return groups_; }
    libs::VoidResult persist() override { return libs::VoidResult(); }   // 内存：不落盘
    libs::VoidResult load() override { return libs::VoidResult(); }
    ReadinessReport readiness() const override;
    void set_required(std::vector<std::string> groups,
                      std::map<std::string, std::vector<std::string>> fields);

private:
    std::vector<std::string> required_groups_;
    std::map<std::string, std::vector<std::string>> required_fields_;
    std::map<std::string, ParameterGroup> groups_;
};

}  // namespace business::policy