#include "libs/business/measurement/frame_bus.hpp"

#include <utility>

namespace business::measurement {

FrameQueue::FrameQueue() : options_(Options{}) {}

FrameQueue::FrameQueue(Options options) : options_(options) {}

std::uint64_t FrameQueue::subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t id = next_subscriber_id_++;
    subscribers_[id] = Subscriber{};
    return id;
}

void FrameQueue::unsubscribe(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(id);
}

bool FrameQueue::try_pop(std::uint64_t id, SpectrumFrame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(id);
    if (it == subscribers_.end() || it->second.queue.empty()) return false;
    out = std::move(it->second.queue.front());
    it->second.queue.pop_front();
    return true;
}

std::size_t FrameQueue::pending(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(id);
    return it == subscribers_.end() ? 0 : it->second.queue.size();
}

std::uint64_t FrameQueue::dropped_frames() const noexcept {
    return dropped_frames_.load();
}

void FrameQueue::publish(const SpectrumFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++next_frame_index_;
    publish_locked(frame);
}

void FrameQueue::publish_locked(const SpectrumFrame& frame) {
    ++delivered_frames_;
    for (auto& entry : subscribers_) {
        auto& queue = entry.second.queue;
        // 丢最旧的一帧（背压见文件头）：宁可 UI 少看几帧，
        // 也不能让一个慢客户端把采集节奏拖住。
        while (queue.size() >= options_.subscriber_queue_frames && !queue.empty()) {
            queue.pop_front();
            dropped_frames_++;
        }
        queue.push_back(frame);
    }
}

void FrameQueue::clear_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : subscribers_) entry.second.queue.clear();
}

json::Value FrameQueue::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value v = json::object();
    // **本类不采集**，所以没有 running 概念。是否在产帧由 MeasurementJob 报告，
    // 那是 `measurement status` 的事。这里只回答"队列这一端怎么样"。
    v.set("subscribers", json::Value(static_cast<double>(subscribers_.size())));
    v.set("frame_index", json::Value(static_cast<double>(next_frame_index_)));
    v.set("delivered_frames", json::Value(static_cast<double>(delivered_frames_)));
    v.set("dropped_frames", json::Value(static_cast<double>(dropped_frames_.load())));
    v.set("spectrometer_open", json::Value());   // null：本类不知道设备的事
    v.set("note", json::Value("帧队列只负责存帧与取帧，不采集、不碰设备；"
                              "帧由测量流程产出。见 measurement_start / measurement_status"));
    return v;
}

}  // namespace business::measurement