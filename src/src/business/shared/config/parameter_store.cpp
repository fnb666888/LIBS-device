#include "libs/business/shared/config/parameter_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "libs/business/shared/json/json.hpp"

namespace business::config {
namespace {

using libs::Error;
using libs::ErrorKind;
using libs::VoidResult;

Error bad_request(const std::string& message) {
    return Error::make(ErrorKind::invalid_argument, "bad_parameter_request", message);
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
        g.set("complete", json::Value(kv.second.complete));
        root.set(kv.first, std::move(g));
    }
    return root;
}

}  // namespace

const char* to_string(ApplyState state) noexcept {
    switch (state) {
        case ApplyState::completed: return "completed";
        case ApplyState::rejected:  return "rejected";
        case ApplyState::failed:    return "failed";
    }
    return "unknown";
}

// ── FileParameterStore ───────────────────────────────────────────────────────

FileParameterStore::FileParameterStore(std::string path,
                                       std::vector<std::string> required_groups,
                                       std::map<std::string, std::vector<std::string>> required_fields)
    : path_(std::move(path)),
      required_groups_(std::move(required_groups)),
      required_fields_(std::move(required_fields)) {}

ApplyResult FileParameterStore::apply(const std::string& group,
                                      const std::map<std::string, double>& values) {
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

    // 立即生效：直接写进 groups_，没有暂存区。
    auto& target = groups_[group];
    for (const auto& kv : values) {
        target.values[kv.first] = kv.second;
    }

    // 齐备标志**必须在落盘之前**算好。
    //
    // 顺序曾经是"先 persist()、再算 complete"，于是磁盘上存的永远是
    // 上一次的齐备状态：重启后 load() 读回来是 complete=false，
    // 而 readiness() 是实时算的、报 true —— 同一个库里两个视图互相矛盾，
    // 现场表现是"参数明明齐了，界面上却还标着未齐备"。这类不一致最难排。
    {
        bool ok = true;
        const auto rf = required_fields_.find(group);
        if (rf != required_fields_.end()) {
            for (const std::string& f : rf->second) {
                if (!target.has(f)) ok = false;
            }
        }
        target.complete = ok;
    }

    // 先落盘再应答（保守策略；任务书 §4.3 的真实一致性规则待细化）。
    const VoidResult p = persist();
    if (!p) {
        r.state = ApplyState::failed;
        r.message = "参数已改但落盘失败，不能应答已完成：" + p.error().message;
        return r;
    }

    r.state = ApplyState::completed;
    r.message = "参数设置已完成";
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

}  // namespace business::config