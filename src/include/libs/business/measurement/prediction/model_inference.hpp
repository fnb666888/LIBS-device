#pragma once

#include <memory>
#include <string>
#include "libs/business/shared/json/json.hpp"
#include "libs/error.hpp"

namespace business::measurement::prediction {

// Owns a resident Python model worker. Uses its own mutex, never the device lock.
class ModelInference {
public:
    ModelInference();
    ~ModelInference();
    ModelInference(const ModelInference&) = delete;
    ModelInference& operator=(const ModelInference&) = delete;
    void configure(const std::string& algorithms_dir = {});
    libs::Result<json::Value> status();
    libs::Result<json::Value> analyze(const json::Value& input);
    json::Value latest() const;
    // Interrupt startup/prediction waits before joining the owning service.
    void cancel() noexcept;
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace business::measurement::prediction
