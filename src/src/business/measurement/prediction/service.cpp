#include "libs/business/measurement/prediction/service.hpp"
#include "libs/business/measurement/prediction/algorithm_contract.hpp"
#include "libs/business/shared/time_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace business::measurement::prediction {
namespace {
libs::Error invalid(const char* code, const char* message) {
    return libs::Error::make(libs::ErrorKind::invalid_argument, code, message);
}
json::Value unavailable(const std::string& request, const std::string& message) {
    auto v = json::object();
    v.set("request_id", request);
    v.set("availability", "unavailable");
    v.set("executed", false);
    v.set("detail", message);
    return v;
}
}

Service::Service(FrameQueue& frames, std::string directory, Engine engine)
    : frames_(frames), results_directory_(std::move(directory)), engine_(std::move(engine)) {
    if (!engine_.status) engine_.status = [this] { return model_.status(); };
    if (!engine_.analyze) engine_.analyze = [this](const json::Value& input) { return model_.analyze(input); };
    if (!engine_.cancel) engine_.cancel = [this] { model_.cancel(); };
}
Service::~Service() { stop(); }
void Service::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) return;
    stopping_ = false;
    model_.configure();
    subscription_ = frames_.subscribe();
    worker_ = std::thread([this] { run(); });
}
void Service::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    engine_.cancel();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (subscription_) frames_.unsubscribe(subscription_);
    subscription_ = 0;
    model_.stop();
}

libs::VoidResult Service::configure(bool enabled, const std::string& branch) {
    if (enabled && branch != "oxide" && branch != "sulfide")
        return invalid("prediction_branch_required", "请选择氧化矿或硫化矿模型分支");
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return invalid("prediction_stopped", "预测服务已停止");
    // Drain only our subscription. UI and raw-data writer are unaffected.
    SpectrumFrame discarded;
    while (frames_.try_pop(subscription_, discarded)) {}
    ++generation_;
    enabled_ = enabled;
    branch_ = enabled ? branch : "";
    state_ = enabled ? "loading" : "disabled";
    detail_.clear(); error_code_.clear(); latest_ = json::Value();
    active_request_.clear(); pending_.reset(); last_frame_.reset();
    wake_.notify_all();
    return {};
}

libs::Result<json::Value> Service::submit(const json::Value& input) {
    const auto checked = algorithm::validate_input(input);
    if (!checked) return checked.error();
    const auto& branch = input["parameters"]["branch"].as_string();
    if (branch != "oxide" && branch != "sulfide")
        return invalid("prediction_branch_required", "预测输入须指定 oxide 或 sulfide 分支");
    if (json::stringify(input).size() > 32 * 1024 * 1024)
        return invalid("prediction_input_size", "预测输入超过 32 MB");
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return invalid("prediction_stopped", "预测服务已停止");
    if (pending_ || processing_)
        return libs::Error::make(libs::ErrorKind::device, "prediction_busy", "预测正在处理，请稍后提交");
    pending_ = Task{input, "offline", generation_};
    state_ = "queued";
    detail_.clear(); error_code_.clear(); latest_ = json::Value();
    auto response = json::object();
    response.set("accepted", true);
    response.set("request_id", input["request_id"]);
    wake_.notify_all();
    return response;
}

json::Value Service::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto v = json::object();
    v.set("available", true); v.set("enabled", enabled_); v.set("branch", branch_);
    v.set("state", state_); v.set("detail", detail_); v.set("error_code", error_code_);
    v.set("processing", processing_); v.set("active_request_id", active_request_);
    v.set("completed", static_cast<double>(completed_));
    v.set("ineligible", static_cast<double>(ineligible_));
    v.set("failed", static_cast<double>(failed_));
    v.set("skipped_frames", static_cast<double>(skipped_));
    v.set("model", model_status_); v.set("latest", latest_);
    return v;
}

json::Value Service::input_for(const SpectrumFrame& frame, const std::string& branch,
                               const std::string& request) {
    auto input = json::object(), spectrum = json::object(), group = json::object();
    input.set("schema_version", "libs.algorithm.v1"); input.set("request_id", request);
    auto context = json::object();
    context.set("frame_index", static_cast<double>(frame.frame_index));
    context.set("shot_id", static_cast<double>(frame.shot_id));
    context.set("captured_at_ms", static_cast<double>(frame.wall_clock_ms));
    input.set("context", std::move(context));
    auto parameters = json::object(); parameters.set("branch", branch);
    input.set("parameters", std::move(parameters));
    auto channels = json::array();
    bool complete = frame.ok() && frame.expected_groups == frame.received_groups;
    for (const auto& channel : frame.channels) {
        if (channel.state != ChannelState::present) { complete = false; continue; }
        channels.push_back(to_json(channel));
    }
    group.set("shot_id", static_cast<double>(frame.shot_id));
    group.set("hardware_synchronized", frame.hardware_synchronized);
    group.set("complete", complete); group.set("channels", std::move(channels));
    auto groups = json::array(); groups.push_back(std::move(group));
    spectrum.set("spectrum_source", "LIBS");
    spectrum.set("spectrum_format", "wavelength_nm+counts");
    spectrum.set("scan_groups", std::move(groups));
    input.set("spectrum", std::move(spectrum));
    return input;
}

void Service::run() {
    std::uint64_t loaded_generation = static_cast<std::uint64_t>(-1);
    for (;;) {
        std::optional<Task> task;
        bool enabled; std::string branch; std::uint64_t generation;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) return;
            enabled = enabled_; branch = branch_; generation = generation_;
            if (pending_) { task = std::move(pending_); pending_.reset(); processing_ = true; }
        }
        if (task) { execute(std::move(*task)); continue; }
        if (enabled && loaded_generation != generation) {
            libs::Result<json::Value> loaded;
            try { loaded = engine_.status(); }
            catch (const std::exception& e) { loaded = libs::Error::make(libs::ErrorKind::internal, "model_status_exception", e.what()); }
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            if (generation != generation_) continue;
            loaded_generation = generation;
            if (loaded) {
                model_status_ = loaded.value(); state_ = "waiting";
                detail_.clear(); error_code_.clear();
            } else {
                model_status_ = json::Value(); state_ = "model_unavailable";
                detail_ = loaded.error().message; error_code_ = loaded.error().code;
                ++failed_;
            }
        }
        SpectrumFrame frame;
        if (frames_.try_pop(subscription_, frame)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_ || generation != generation_ || state_ == "model_unavailable") continue;
            if (last_frame_ && frame.frame_index > *last_frame_ + 1)
                skipped_ += static_cast<std::uint64_t>(frame.frame_index - *last_frame_ - 1);
            last_frame_ = frame.frame_index;
            const auto request = "live-" + std::to_string(++next_request_) + "-frame-" + std::to_string(frame.frame_index);
            if (!frame.ok() || !frame.laser_shot) {
                latest_ = unavailable(request, frame.ok() ? "非激光测量帧" : frame.capture_error_message);
                state_ = "ineligible"; ++ineligible_; continue;
            }
            task = Task{input_for(frame, branch_, request), "live", generation_};
            processing_ = true;
        }
        if (task) { execute(std::move(*task)); continue; }
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(40), [this, generation] {
            return stopping_ || pending_.has_value() || generation != generation_;
        });
    }
}

void Service::execute(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || task.generation != generation_) { processing_ = false; return; }
        state_ = "predicting"; active_request_ = task.input["request_id"].as_string();
        // Live status keeps the last completed frame while the next is running.
        // Configuration/offline submission still clears results explicitly.
        detail_.clear(); error_code_.clear();
    }
    libs::Result<json::Value> result;
    try { result = engine_.analyze(task.input); }
    catch (const std::exception& e) { result = libs::Error::make(libs::ErrorKind::internal, "prediction_exception", e.what()); }
    auto record = result ? result.value() : unavailable(task.input["request_id"].as_string(), result.error().message);
    record.set("source", task.source);
    record.set("context", task.input["context"]);
    record.set("branch", task.input["parameters"]["branch"]);
    if (!result) record.set("error_code", result.error().code);
    std::uint64_t record_index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || task.generation != generation_) { processing_ = false; return; }
        record_index = ++next_request_;
    }
    // Save independently of raw spectra. A failed prediction is a record too.
    const std::string filename = "prediction-" + std::to_string(wall_clock_ms()) + "-" + std::to_string(record_index) + ".json";
    std::error_code ec;
    const auto directory = std::filesystem::u8path(results_directory_);
    std::filesystem::create_directories(directory, ec);
    const auto path = directory / filename;
    std::ofstream output;
    if (!ec) output.open(path, std::ios::binary);
    record.set("saved", true); record.set("result_path", path.u8string());
    bool saved = false;
    if (!ec && output.is_open()) {
        output << json::stringify(record, 2); output.close();
        saved = static_cast<bool>(output);
    }
    record.set("saved", saved);
    record.set("result_path", saved ? path.u8string() : "");
    std::lock_guard<std::mutex> lock(mutex_);
    processing_ = false;
    if (stopping_ || task.generation != generation_) return;
    latest_ = std::move(record);
    if (result && latest_["availability"].as_string() == "available") {
        ++completed_; state_ = "completed";
    } else if (result) { ++ineligible_; state_ = "ineligible"; }
    else { ++failed_; state_ = "error"; detail_ = result.error().message; error_code_ = result.error().code; }
}

}  // namespace business::measurement::prediction
