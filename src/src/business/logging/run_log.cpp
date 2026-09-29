#include "libs/business/logging/run_log.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace business::logging {

const char* to_string(RecordKind kind) noexcept {
    switch (kind) {
        case RecordKind::state_change:        return "state_change";
        case RecordKind::parameter_reply:     return "parameter_reply";
        case RecordKind::belt_and_position:   return "belt_and_position";
        case RecordKind::distance_observation: return "distance_observation";
        case RecordKind::environment:         return "environment";
        case RecordKind::exception_recovery:  return "exception_recovery";
        case RecordKind::general:             return "general";
    }
    return "general";
}

json::Value to_json(const RunRecord& record) {
    json::Value v = json::object();
    v.set("kind", json::Value(to_string(record.kind)));
    v.set("wall_clock_ms", json::Value(static_cast<double>(record.wall_clock_ms)));
    v.set("monotonic_ms", json::Value(static_cast<double>(record.monotonic_ms)));
    v.set("message", json::Value(record.message));
    v.set("fields", record.fields);
    return v;
}

RunLog::RunLog(std::string path) : path_(std::move(path)) {}
RunLog::~RunLog() = default;

libs::VoidResult RunLog::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) return libs::VoidResult();
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(path_).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return libs::Error::make(libs::ErrorKind::transport, "log_dir_failed",
                                     "cannot create log directory " + dir.string() + ": " + ec.message());
        }
    }
    out_.open(path_, std::ios::binary | std::ios::app);
    if (!out_) {
        return libs::Error::make(libs::ErrorKind::transport, "log_open_failed",
                                 "cannot open run log for append: " + path_);
    }
    return libs::VoidResult();
}

bool RunLog::enabled() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return out_.is_open();
}

libs::VoidResult RunLog::write(RecordKind kind, std::string message, json::Value fields) {
    RunRecord record;
    record.kind = kind;
    record.wall_clock_ms = wall_clock_ms();
    record.monotonic_ms = monotonic_ms();
    record.message = std::move(message);
    record.fields = fields.is_null() ? json::object() : std::move(fields);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return libs::Error::make(libs::ErrorKind::transport, "log_closed",
                                 "run log is not open; record dropped: " + record.message);
    }
    out_ << json::stringify(to_json(record)) << "\n";
    out_.flush();   // 现场断电是常态，不等缓冲
    ++count_;
    return libs::VoidResult();
}

std::int64_t RunLog::count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

// ── RecoveryContextStore ────────────────────────────────────────────────────

RecoveryContextStore::RecoveryContextStore(std::string directory)
    : directory_(std::move(directory)) {}

json::Value to_json(const RecoveryContext& context) {
    json::Value v = json::object();
    v.set("snapshot_at_ms", json::Value(static_cast<double>(context.snapshot_at_ms)));
    v.set("machine_state", json::Value(context.machine_state));
    json::Array blocked;
    for (const auto& b : context.blocked_by) blocked.emplace_back(b);
    v.set("blocked_by", json::Value(std::move(blocked)));
    v.set("gate_allowed", json::Value(context.gate_allowed));
    v.set("target_belt", json::Value(context.target_belt));
    v.set("detection_stage", json::Value(context.detection_stage));
    v.set("belt_sequence", json::Value(static_cast<double>(context.belt_sequence)));
    v.set("last_parameters", context.last_parameters);
    v.set("last_saved_capture_path", json::Value(context.last_saved_capture_path));
    v.set("last_shot_id", json::Value(static_cast<double>(context.last_shot_id)));
    v.set("reason", json::Value(context.reason));
    v.set("notice", json::Value(
        "恢复上下文只是异常时刻的观测快照，不是命令队列；"
        "不得据此自动重发运动或激光触发命令（任务书 §6.2 红线）"));
    return v;
}

libs::Result<RecoveryContext> from_json(const json::Value& value) {
    if (!value.is_object()) {
        return libs::Error::make(libs::ErrorKind::invalid_argument, "recovery_not_object",
                                 "recovery context must be a JSON object");
    }
    RecoveryContext c;
    c.snapshot_at_ms = static_cast<Milliseconds>(value["snapshot_at_ms"].as_number());
    c.machine_state = value["machine_state"].as_string();
    if (value["blocked_by"].is_array()) {
        for (const auto& b : value["blocked_by"].as_array()) c.blocked_by.push_back(b.as_string());
    }
    c.gate_allowed = value["gate_allowed"].as_bool();
    c.target_belt = value["target_belt"].as_string();
    c.detection_stage = value["detection_stage"].as_string();
    c.belt_sequence = static_cast<std::int64_t>(value["belt_sequence"].as_number());
    c.last_parameters = value["last_parameters"];
    c.last_saved_capture_path = value["last_saved_capture_path"].as_string();
    c.last_shot_id = static_cast<std::int64_t>(value["last_shot_id"].as_number());
    c.reason = value["reason"].as_string();
    return c;
}

libs::Result<std::string> RecoveryContextStore::write(const RecoveryContext& context) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        return libs::Error::make(libs::ErrorKind::transport, "recovery_dir_failed",
                                 "cannot create recovery directory " + directory_ + ": " + ec.message());
    }
    std::ostringstream name;
    name << "recovery_t" << context.snapshot_at_ms << ".json";
    const std::string path = directory_ + "/" + name.str();

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return libs::Error::make(libs::ErrorKind::transport, "recovery_open_failed",
                                 "cannot write recovery snapshot: " + path);
    }
    out << json::stringify(to_json(context), 2);
    out.flush();
    const bool good = static_cast<bool>(out);
    out.close();
    if (!good) {
        return libs::Error::make(libs::ErrorKind::transport, "recovery_write_failed",
                                 "write failed (possibly disk full): " + path);
    }
    return path;
}

libs::Result<std::vector<std::string>> RecoveryContextStore::list() const {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec)) {
        // 没有快照目录是正常情况（还没出过异常），返回空列表而不是错误。
        return std::vector<std::string>();
    }
    std::vector<std::string> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".json") paths.push_back(entry.path().string());
    }
    // 名字里带的是毫秒时间戳，字典序即时间序，倒序即最新在前。
    std::sort(paths.rbegin(), paths.rend());
    return paths;
}

libs::Result<RecoveryContext> RecoveryContextStore::read(const std::string& path) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return libs::Error::make(libs::ErrorKind::invalid_argument, "recovery_missing",
                                 "recovery snapshot not found: " + path);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    json::Value root;
    const json::ParseError err = json::parse(buffer.str(), root);
    if (!err.ok) {
        return libs::Error::make(libs::ErrorKind::invalid_argument, "recovery_bad_json",
                                 "invalid JSON in recovery snapshot " + path + ": " + err.message);
    }
    return from_json(root);
}

}  // namespace business::logging
