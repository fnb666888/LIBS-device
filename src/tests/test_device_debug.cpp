#include "check.hpp"

#include "libs/business/app/runtime.hpp"
#include "libs/business/services/facade.hpp"

using namespace business;
using libs::ErrorKind;

namespace {
struct Fixture {
    app::Runtime runtime;
    app::BusinessFacade facade;
    explicit Fixture(const char* tag) : facade(runtime) {
        app::Runtime::Options options;
        options.open_devices = false;
        options.data_dir = std::string("build/tmp_device_debug_") + tag;
        const auto started = runtime.start(config::default_template().value(), options);
        (void)started;
    }
};
}

TEST_MAIN_BEGIN()

CASE(device_debug_is_public_command); {
    Fixture f("commands");
    const auto response = f.facade.dispatch(
        R"({"command":"ping","args":{}})");
    CHECK(response.ok);
    bool found = false;
    for (const auto& c : response.payload["commands"].as_array())
        if (c.as_string() == "device_debug") found = true;
    CHECK(found);
}

CASE(device_debug_rejects_missing_shape); {
    Fixture f("shape");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"laser"}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
}

CASE(device_debug_rejects_unknown_device_without_touching_hardware); {
    Fixture f("unknown");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"motor","action":"status"}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
    CHECK_EQ(response.error.code, std::string("device_debug_device"));
}

CASE(device_debug_reports_disabled_laser_explicitly); {
    Fixture f("laser");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"laser","action":"status"}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
    CHECK_EQ(response.error.code, std::string("laser_not_open"));
}

CASE(device_debug_validates_laser_action_before_device_access); {
    Fixture f("laser_action");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"laser","action":"write"}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
    CHECK_EQ(response.error.code, std::string("laser_debug_action"));
}

CASE(device_debug_rejects_laser_write); {
    Fixture f("laser_write");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"laser","action":"write","register":36,"value":1}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::invalid_argument);
    CHECK_EQ(response.error.code, std::string("laser_debug_action"));
}

CASE(device_debug_rejects_out_of_range_rangefinder_request); {
    Fixture f("range");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"rangefinder","action":"read","start":512,"count":10}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
    CHECK_EQ(response.error.code, std::string("rangefinder_not_open"));
}

CASE(device_debug_reports_disabled_air_conditioner); {
    Fixture f("ac");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"air_conditioner","action":"set_mode","mode":"cooling"}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
    CHECK_EQ(response.error.code, std::string("air_conditioner_not_open"));
}

CASE(device_debug_reports_disabled_indicator); {
    Fixture f("indicator");
    const auto response = f.facade.dispatch(
        R"({"command":"device_debug","args":{"device":"indicator","action":"query","channel":"red"}})");
    CHECK(!response.ok);
    CHECK_EQ(response.error.kind, ErrorKind::unsupported);
    CHECK_EQ(response.error.code, std::string("indicator_not_open"));
}

TEST_MAIN_RETURN()
