#include "libs/business/policy/detection_config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace business::policy {
namespace {

using libs::Error;
using libs::ErrorKind;
using libs::Result;
using libs::VoidResult;

Error bad_request(const std::string& message) {
    return Error::make(ErrorKind::invalid_argument, "bad_parameter_request", message);
}

// 字段名到单位的默认表。任务书 §4.1 字段未细化，这里只登记驱动已确认语义的字段；
// 未登记的字段仍然接受，但 unit 为空——上层不得据此臆造单位。
const std::map<std::string, std::map<std::string, std::string>>& default_units() {
    static const std::map<std::string, std::map<std::string, std::string>> units = {
        {"laser", {
            {"discharge_current", "A"},
            {"discharge_pulse_width", "us"},
            {"discharge_frequency", "Hz"},
            {"discharge_delay", "us"},
            {"ld_temperature_limit", "degC"},
            {"q_delay", "us"},
            {"voltage_limit", "V"},
            {"q_divider", ""},
            {"q_warmup", "s"},
        }},
        {"spectrometer", {
            {"integration_ms", "ms"},
            {"integration_delay_cycles", "cycles"},
            {"timeout_ms", "ms"},
        }},
        {"trigger", {
            {"laser_delay_cycles", "cycles"},
            {"laser_width_cycles", "cycles"},
        }},
    };
    return units;
}

ReadinessReport make_readiness(const std::map<std::string, ParameterGroup>& groups,
                              const std::vector<std::string>& required_groups,
                              const std::map<std::string, std::vector<std::string>>& required_fields) {
    ReadinessReport r;
    for (const std::string& g : required_groups) {
        const auto needed = required_fields.find(g);
        const std::vector<std::string> empty_fields;
        const std::vector<std::string>& wanted =
            (needed == required_fields.end()) ? empty_fields : needed->second;

        const auto it = groups.find(g);
        if (it == groups.end()) {
            r.missing_groups.push_back(g);
            for (const std::string& f : wanted) r.missing_fields.push_back(g + "." + f);
            continue;
        }
        bool group_ok = true;
        for (const std::string& f : wanted) {
            if (!it->second.has(f)) {
                r.missing_fields.push_back(g + "." + f);
                group_ok = false;
            }
        }
        if (!group_ok) r.missing_groups.push_back(g);
    }
    r.ready = r.missing_groups.empty() && r.missing_fields.empty();
    std::ostringstream os;
    if (r.ready) {
        os << "核心参数齐备";
    } else {
        os << "核心参数未齐备：缺 " << r.missing_groups.size() << " 个分组、"
           << r.missing_fields.size() << " 个字段";
    }
    r.summary = os.str();
    return r;
}

json::Value groups_to_json(const std::map<std::string, ParameterGroup>& groups) {
    json::Value root = json::object();
    for (const auto& kv : groups) {
        json::Value g = json::object();
        json::Value values = json::object();
        for (const auto& f : kv.second.values) values.set(f.first, json::Value(f.second));
        g.set("values", std::move(values));
        json::Value labels = json::object();
        for (const auto& f : kv.second.labels) labels.set(f.first, json::Value(f.second));
        g.set("units", std::move(labels));
        g.set("complete", json::Value(kv.second.complete));
        root.set(kv.first, std::move(g));
    }
    return root;
}

}  // namespace

const char* to_string(ApplyState state) noexcept {
    switch (state) {
        case ApplyState::completed: return "completed";
        case ApplyState::staged:     return "staged";
        case ApplyState::rejected:  return "rejected";
        case ApplyState::failed:    return "failed";
    }
    return "unknown";
}

const char* to_string(ApplyTiming timing) noexcept {
    switch (timing) {
        case ApplyTiming::at_next_idle: return "at_next_idle";
        case ApplyTiming::immediately:  return "immediately";
    }
    return "unknown";
}

// ── FileParameterStore ───────────────────────────────────────────────────────

FileParameterStore::FileParameterStore(std::string path,
                                       std::vector<std::string> required_groups,
                                       std::map<std::string, std::vector<std::string>> required_fields)
    : path_(std::move(path)),
      required_groups_(std::move(required_groups)),
      required_fields_(std::move(required_fields)) {
    for (const auto& group : required_fields_) {
        for (const std::string& field : group.second) {
            const auto& u = default_units();
            const auto it = u.find(group.first);
            if (it != u.end()) {
                const auto fu = it->second.find(field);
                if (fu != it->second.end()) units_[group.first][field] = fu->second;
            }
        }
    }
}

void FileParameterStore::set_unit(const std::string& group, const std::string& field,
                                  const std::string& unit) {
    units_[group][field] = unit;
}

ApplyResult FileParameterStore::apply(const std::string& group,
                                      const std::map<std::string, double>& values,
                                      ApplyTiming timing) {
    return apply_impl(group, values, /*staged=*/timing == ApplyTiming::at_next_idle);
}

ApplyResult FileParameterStore::apply_impl(const std::string& group,
                                           const std::map<std::string, double>& values,
                                           bool staged) {
    ApplyResult r;
    r.group = group;

    if (group.empty()) {
        r.state = ApplyState::rejected;
        r.message = "参数分组名不能为空";
        return r;
    }
    if (values.empty()) {
        r.state = ApplyState::rejected;
        r.message = "没有任何字段被设置";
        return r;
    }

    // 数值校验：NaN/Inf 绝不允许进参数表（纪律 2：未知 != 正常）。
    for (const auto& kv : values) {
        if (kv.first.empty()) {
            r.state = ApplyState::rejected;
            r.message = "字段名不能为空";
            r.failed_fields.push_back("<empty>");
            return r;
        }
        if (std::isnan(kv.second) || std::isinf(kv.second)) {
            r.state = ApplyState::rejected;
            r.message = "字段 " + kv.first + " 的值不是有限数（NaN/Inf 不允许作为参数）";
            r.failed_fields.push_back(kv.first);
            return r;
        }
    }

    if (staged) {
        // at_next_idle：暂存，不改生效值。采集期间不会被参数变更撕裂。
        for (const auto& kv : values) pending_[group][kv.first] = kv.second;
    } else {
        auto& target = groups_[group];
        target.name = group;
        for (const auto& kv : values) {
            target.values[kv.first] = kv.second;
            const auto u = units_.find(group);
            if (u != units_.end()) {
                const auto f = u->second.find(kv.first);
                if (f != u->second.end()) target.labels[kv.first] = f->second;
            }
        }
    }

    // 先落盘再应答（保守策略；任务书 §4.3 的真实一致性规则待细化）。
    if (!staged) {
        const VoidResult p = persist();
        if (!p) {
            r.state = ApplyState::failed;
            r.message = "参数已改但落盘失败，不能应答已完成：" + p.error().message;
            return r;
        }
    }

    // 校验核心字段是否因此变得齐备。齐备与否独立于"完成"语义。
    const auto g = groups_.find(group);
    if (g != groups_.end()) {
        bool ok = true;
        const auto rf = required_fields_.find(group);
        if (rf != required_fields_.end()) {
            for (const std::string& f : rf->second) {
                if (!g->second.has(f)) ok = false;
            }
        }
        g->second.complete = ok;
    }

    // 暂存 != 完成。报 completed 会让调用方以为参数已经生效。
    r.state = staged ? ApplyState::staged : ApplyState::completed;
    r.message = staged ? "已暂存，将在下一次空闲点生效（尚未生效）" : "参数设置已完成";
    return r;
}

ApplyResult FileParameterStore::query(const std::string& group,
                                      std::map<std::string, double>& out) const {
    ApplyResult r;
    r.group = group;
    const auto it = groups_.find(group);
    if (it == groups_.end()) {
        r.state = ApplyState::rejected;
        r.message = "参数分组 " + group + " 尚未设置";
        return r;
    }
    out = it->second.values;
    r.state = ApplyState::completed;
    r.message = it->second.complete ? "查询完成" : "查询完成，但核心字段尚未齐备";
    return r;
}

std::map<std::string, ParameterGroup> FileParameterStore::all() const { return groups_; }

bool FileParameterStore::has_pending() const { return !pending_.empty(); }

bool FileParameterStore::commit_pending() {
    if (pending_.empty()) return false;
    const auto staged = pending_;
    pending_.clear();
    for (const auto& g : staged) {
        const ApplyResult r = apply_impl(g.first, g.second, /*staged=*/false);
        if (!r.completed()) {
            // 提交失败：放回暂存区，不丢改动。
            for (const auto& kv : g.second) pending_[g.first][kv.first] = kv.second;
            return false;
        }
    }
    return true;
}

VoidResult FileParameterStore::persist() {
    if (path_.empty()) {
        return bad_request("参数存储没有配置落盘路径");
    }

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Error::make(ErrorKind::internal, "persist_open_failed",
                               "无法写入临时文件：" + tmp);
        }
        json::Value root = json::object();
        root.set("version", json::Value(1));
        root.set("groups", groups_to_json(groups_));
        out << json::stringify(root, 2);
        if (!out.good()) {
            return Error::make(ErrorKind::internal, "persist_write_failed",
                               "写临时文件时出错：" + tmp);
        }
    }

    // 原子替换：先落盘再应答。任务书 §4.3 待细化，此处为保守策略。
    std::remove(path_.c_str());
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        std::remove(tmp.c_str());
        return Error::make(ErrorKind::internal, "persist_rename_failed",
                           "无法把临时文件替换为参数文件：" + path_);
    }
    return libs::VoidResult();
}

VoidResult FileParameterStore::load() {
    if (path_.empty()) return libs::VoidResult();
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        // 首次运行：文件不存在是正常情况，不是错误。也没有"默认参数"，
        // 因为默认参数就是猜测——核心参数不齐就是不齐（门禁会挡住）。
        groups_.clear();
        return libs::VoidResult();
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    json::Value root;
    const json::ParseError err = json::parse(buf.str(), root);
    if (!err.ok) {
        return Error::make(ErrorKind::invalid_argument, "parameter_file_broken",
                           "参数文件不是合法 JSON：" + err.message);
    }
    const json::Value& g = root["groups"];
    if (!g.is_object()) {
        return Error::make(ErrorKind::invalid_argument, "parameter_file_broken",
                           "参数文件缺少 groups 对象");
    }

    std::map<std::string, ParameterGroup> loaded;
    for (const auto& kv : g.as_object()) {
        ParameterGroup group;
        group.name = kv.first;
        const json::Value& values = kv.second["values"];
        if (values.is_object()) {
            for (const auto& f : values.as_object()) {
                if (!f.second.is_number()) {
                    return Error::make(ErrorKind::invalid_argument, "parameter_file_broken",
                                       "参数字段 " + kv.first + "." + f.first + " 不是数值");
                }
                group.values[f.first] = f.second.as_number();
            }
        }
        const json::Value& labels = kv.second["units"];
        if (labels.is_object()) {
            for (const auto& f : labels.as_object()) {
                if (f.second.is_string()) group.labels[f.first] = f.second.as_string();
            }
        }
        loaded[kv.first] = std::move(group);
    }
    groups_ = std::move(loaded);

    // 加载不等于硬件已加载，更不等于上电自动出光。这里只更新 complete 标记。
    for (auto& kv : groups_) {
        bool ok = true;
        const auto rf = required_fields_.find(kv.first);
        if (rf != required_fields_.end()) {
            for (const std::string& f : rf->second) {
                if (!kv.second.has(f)) ok = false;
            }
        }
        kv.second.complete = ok;
    }
    return libs::VoidResult();
}

ReadinessReport FileParameterStore::readiness() const {
    return make_readiness(groups_, required_groups_, required_fields_);
}

// ── MemoryParameterStore ─────────────────────────────────────────────────────

MemoryParameterStore::MemoryParameterStore(std::vector<std::string> required_groups) {
    set_required(std::move(required_groups), {});
}

void MemoryParameterStore::set_required(std::vector<std::string> groups,
                                        std::map<std::string, std::vector<std::string>> fields) {
    required_groups_ = std::move(groups);
    required_fields_ = std::move(fields);
}

ApplyResult MemoryParameterStore::apply(const std::string& group,
                                        const std::map<std::string, double>& values,
                                        ApplyTiming) {
    ApplyResult r;
    r.group = group;
    if (group.empty() || values.empty()) {
        r.state = ApplyState::rejected;
        r.message = "分组名或字段为空";
        return r;
    }
    for (const auto& kv : values) {
        if (std::isnan(kv.second) || std::isinf(kv.second)) {
            r.state = ApplyState::rejected;
            r.message = "字段 " + kv.first + " 的值不是有限数";
            r.failed_fields.push_back(kv.first);
            return r;
        }
    }

    auto& target = groups_[group];
    target.name = group;
    for (const auto& kv : values) {
        target.values[kv.first] = kv.second;
        const auto& u = default_units();
        const auto it = u.find(group);
        if (it != u.end()) {
            const auto f = it->second.find(kv.first);
            if (f != it->second.end()) target.labels[kv.first] = f->second;
        }
    }
    const auto rf = required_fields_.find(group);
    if (rf != required_fields_.end()) {
        bool ok = true;
        for (const std::string& f : rf->second) {
            if (!target.has(f)) ok = false;
        }
        target.complete = ok;
    }
    r.state = ApplyState::completed;
    r.message = "参数设置已完成（内存存储，未落盘）";
    return r;
}

ApplyResult MemoryParameterStore::query(const std::string& group,
                                        std::map<std::string, double>& out) const {
    ApplyResult r;
    r.group = group;
    const auto it = groups_.find(group);
    if (it == groups_.end()) {
        r.state = ApplyState::rejected;
        r.message = "参数分组 " + group + " 尚未设置";
        return r;
    }
    out = it->second.values;
    r.state = ApplyState::completed;
    r.message = "查询完成";
    return r;
}

ReadinessReport MemoryParameterStore::readiness() const {
    return make_readiness(groups_, required_groups_, required_fields_);
}

}  // namespace business::policy