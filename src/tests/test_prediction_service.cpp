#include "check.hpp"
#include "libs/business/measurement/prediction/service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace business;
using namespace business::measurement;
namespace {
bool until(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
SpectrumFrame frame(int index = 0) {
    SpectrumFrame f;
    f.frame_index = index; f.shot_id = 12; f.wall_clock_ms = 1234;
    f.laser_shot = true; f.hardware_synchronized = true;
    f.expected_groups = f.received_groups = 1;
    ChannelFrame c;
    c.serial = "CH1"; c.state = ChannelState::present; c.pixels = 3;
    c.wavelength_nm = {200, 400, 600}; c.counts = {1, 22, 3};
    f.channels.push_back(c);
    return f;
}
struct Engine {
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool blocked = false, release = false;
    prediction::Service::Engine adapter() {
        return {
            []() -> libs::Result<json::Value> { auto v = json::object(); v.set("availability", "available"); return v; },
            [this](const json::Value& input) -> libs::Result<json::Value> {
                ++calls;
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] { return !blocked || release; });
                auto v = json::object(); v.set("request_id", input["request_id"]);
                v.set("availability", "available"); v.set("executed", true);
                auto composition = json::object(), values = json::object();
                values.set("TFe", 58.0); composition.set("target_values", values);
                v.set("composition", composition); return v;
            },
            [this] { unblock(); }
        };
    }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); release = true; cv.notify_all(); }
};
}

TEST_MAIN_BEGIN()
CASE(frame_adapter_preserves_raw_data_and_marks_absent_channels); {
    auto f = frame(27);
    ChannelFrame missing; missing.serial = "CH2"; missing.reason = "timeout";
    f.channels.push_back(missing);
    const auto input = prediction::Service::input_for(f, "oxide", "frame-27");
    CHECK_EQ(input["context"]["frame_index"].as_number(), 27.0);
    CHECK_EQ(input["context"]["shot_id"].as_number(), 12.0);
    const auto& group = input["spectrum"]["scan_groups"][0];
    CHECK(!group["complete"].as_bool());
    CHECK(group["hardware_synchronized"].as_bool());
    CHECK_EQ(group["channels"].as_array().size(), std::size_t(1));
    CHECK_EQ(group["channels"][0]["counts"][1].as_number(), 22.0);
    CHECK_EQ(f.channels.size(), std::size_t(2));
}
CASE(live_prediction_is_a_separate_subscriber_and_saves_correlated_results); {
    FrameQueue bus; const auto ui = bus.subscribe(); Engine engine;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_live"), engine.adapter());
    service.start(); CHECK(service.configure(true, "oxide").ok());
    bus.publish(frame(4));
    CHECK(until([&] { return service.status()["completed"].as_number() == 1; }));
    auto status = service.status(); const auto& result = status["latest"];
    CHECK_EQ(result["context"]["frame_index"].as_number(), 4.0);
    CHECK_EQ(result["branch"].as_string(), std::string("oxide"));
    CHECK_EQ(result["source"].as_string(), std::string("live"));
    CHECK(result["saved"].as_bool());
    CHECK(std::filesystem::exists(std::filesystem::u8path(result["result_path"].as_string())));
    SpectrumFrame shown; CHECK(bus.try_pop(ui, shown)); CHECK_EQ(shown.channels[0].counts[1], 22.0);
    service.stop(); service.stop(); bus.unsubscribe(ui);
}
CASE(config_change_invalidates_inflight_result_without_blocking_status); {
    FrameQueue bus; Engine engine; engine.blocked = true;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_generation"), engine.adapter());
    service.start(); CHECK(service.configure(true, "oxide").ok()); bus.publish(frame());
    CHECK(until([&] { return engine.calls.load() == 1; }));
    const auto before = std::chrono::steady_clock::now();
    CHECK(service.status()["processing"].as_bool());
    CHECK(service.configure(false, "").ok());
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(250));
    engine.unblock(); CHECK(until([&] { return !service.status()["processing"].as_bool(); }));
    CHECK(service.status()["latest"].is_null());
    CHECK_EQ(service.status()["completed"].as_number(), 0.0);
    CHECK_EQ(service.status()["state"].as_string(), std::string("disabled"));
    service.stop();
}
CASE(last_completed_live_result_remains_visible_during_next_inference); {
    FrameQueue bus; Engine engine;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_visible"), engine.adapter());
    service.start(); CHECK(service.configure(true, "oxide").ok()); bus.publish(frame(40));
    CHECK(until([&] { return service.status()["completed"].as_number() == 1; }));
    {
        std::lock_guard<std::mutex> lock(engine.mutex);
        engine.blocked = true;
    }
    bus.publish(frame(41));
    CHECK(until([&] { return engine.calls.load() == 2; }));
    const auto status = service.status();
    CHECK_EQ(status["state"].as_string(), std::string("predicting"));
    CHECK(status["processing"].as_bool());
    CHECK_EQ(status["latest"]["context"]["frame_index"].as_number(), 40.0);
    CHECK_EQ(status["latest"]["availability"].as_string(), std::string("available"));
    CHECK(status["active_request_id"].as_string() != status["latest"]["request_id"].as_string());
    engine.unblock();
    CHECK(until([&] { return service.status()["completed"].as_number() == 2; }));
    CHECK_EQ(service.status()["latest"]["context"]["frame_index"].as_number(), 41.0);
    CHECK(service.configure(false, "").ok());
    CHECK(service.status()["latest"].is_null());
    service.stop();
}
CASE(queue_overflow_does_not_block_capture_and_is_reported); {
    FrameQueue bus(FrameQueue::Options{2}); Engine engine; engine.blocked = true;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_overflow"), engine.adapter());
    service.start(); CHECK(service.configure(true, "oxide").ok()); bus.publish(frame(0));
    CHECK(until([&] { return engine.calls.load() == 1; }));
    for (int i = 1; i <= 9; ++i) bus.publish(frame(i));
    engine.unblock(); CHECK(until([&] { return service.status()["completed"].as_number() == 3; }));
    CHECK_EQ(service.status()["skipped_frames"].as_number(), 7.0);
    service.stop();
}
CASE(offline_requests_are_async_bounded_and_do_not_require_laser); {
    FrameQueue bus; Engine engine; engine.blocked = true;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_offline"), engine.adapter());
    service.start();
    auto input = prediction::Service::input_for(frame(), "sulfide", "offline-1");
    CHECK(service.submit(input).ok());
    CHECK(until([&] { return engine.calls.load() == 1; }));
    const auto busy = service.submit(input); CHECK(!busy.ok());
    CHECK_EQ(busy.error().code, std::string("prediction_busy"));
    engine.unblock(); CHECK(until([&] { return service.status()["completed"].as_number() == 1; }));
    CHECK_EQ(service.status()["latest"]["source"].as_string(), std::string("offline"));
    CHECK(!service.status()["enabled"].as_bool());
    auto bad = prediction::Service::input_for(frame(), "auto", "invalid-branch");
    CHECK(!service.submit(bad).ok());
    service.stop();
}
CASE(stop_cancels_worker_before_joining); {
    FrameQueue bus; Engine engine; engine.blocked = true;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_stop"), engine.adapter());
    service.start(); CHECK(service.configure(true, "oxide").ok()); bus.publish(frame());
    CHECK(until([&] { return engine.calls.load() == 1; }));
    const auto before = std::chrono::steady_clock::now(); service.stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    CHECK_EQ(bus.status()["subscribers"].as_number(), 0.0);
}
CASE(failed_or_non_laser_frames_do_not_reach_the_model); {
    FrameQueue bus; Engine engine;
    prediction::Service service(bus, ::testing::scratch("build/tmp_prediction_failed"), engine.adapter());
    service.start(); CHECK(service.configure(true, "oxide").ok());
    auto failed = frame(0); failed.capture_error_code = "timeout"; failed.capture_error_message = "capture timed out";
    bus.publish(failed); auto background = frame(1); background.laser_shot = false; bus.publish(background);
    CHECK(until([&] { return service.status()["ineligible"].as_number() == 2; }));
    CHECK_EQ(engine.calls.load(), 0);
    CHECK_EQ(service.status()["latest"]["availability"].as_string(), std::string("unavailable"));
    service.stop();
}
TEST_MAIN_RETURN()
