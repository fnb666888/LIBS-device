#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "libs/business/measurement/frame_bus.hpp"
#include "libs/business/measurement/prediction/model_inference.hpp"

namespace business::measurement::prediction {

// A subscriber of raw measurement frames. No device handles or device locks.
// Inference runs on this service's worker; IPC status/configuration only reads memory.
class Service {
public:
    struct Engine {
        std::function<libs::Result<json::Value>()> status;
        std::function<libs::Result<json::Value>(const json::Value&)> analyze;
        std::function<void()> cancel;
    };
    Service(FrameQueue& frames, std::string results_directory, Engine engine = {});
    ~Service();
    void start();
    void stop();
    libs::VoidResult configure(bool enabled, const std::string& branch);
    libs::Result<json::Value> submit(const json::Value& input);
    json::Value status() const;
    static json::Value input_for(const SpectrumFrame& frame, const std::string& branch,
                                 const std::string& request_id);
private:
    struct Task { json::Value input; std::string source; std::uint64_t generation; };
    void run();
    void execute(Task task);
    FrameQueue& frames_;
    std::string results_directory_;
    ModelInference model_;
    Engine engine_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_ = false, enabled_ = false, processing_ = false;
    std::string branch_, state_ = "disabled", detail_, error_code_;
    std::uint64_t subscription_ = 0, generation_ = 0, next_request_ = 0;
    std::uint64_t completed_ = 0, ineligible_ = 0, failed_ = 0, skipped_ = 0;
    std::optional<std::int64_t> last_frame_;
    std::optional<Task> pending_;
    json::Value model_status_, latest_;
    std::string active_request_;
};

}  // namespace business::measurement::prediction
