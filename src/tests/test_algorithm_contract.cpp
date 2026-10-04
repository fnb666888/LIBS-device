#include "check.hpp"

#include "libs/business/services/algorithm_contract.hpp"

using namespace business;

namespace {

json::Value channel(const char* serial, int pixels = 3) {
    json::Value c = json::object();
    c.set("serial", json::Value(serial));
    c.set("pixels", json::Value(pixels));
    c.set("device_time_ticks", json::Value(42));
    json::Value wavelengths = json::array();
    json::Value counts = json::array();
    for (int i = 0; i < pixels; ++i) {
        wavelengths.push_back(json::Value(400.0 + i));
        counts.push_back(json::Value(10.0 + i));
    }
    c.set("wavelength_nm", std::move(wavelengths));
    c.set("counts", std::move(counts));
    return c;
}

json::Value valid_input() {
    json::Value input = json::object();
    input.set("schema_version", json::Value("libs.algorithm.v1"));
    input.set("request_id", json::Value("req-1"));
    input.set("context", json::object());
    json::Value spectrum = json::object();
    spectrum.set("spectrum_source", json::Value("LIBS"));
    spectrum.set("spectrum_format", json::Value("wavelength_nm+counts"));
    json::Value groups = json::array();
    json::Value group = json::object();
    group.set("shot_id", json::Value(1));
    group.set("complete", json::Value(true));
    group.set("hardware_synchronized", json::Value(false));
    json::Value arm = json::array();
    arm.push_back(json::Value("CH1"));
    group.set("arm_order", std::move(arm));
    json::Value channels = json::array();
    channels.push_back(channel("CH1"));
    group.set("channels", std::move(channels));
    groups.push_back(std::move(group));
    spectrum.set("scan_groups", std::move(groups));
    input.set("spectrum", std::move(spectrum));
    input.set("parameters", json::object());
    input.set("model", json::object());
    return input;
}

}  // namespace

TEST_MAIN_BEGIN()

CASE(contract_exposes_stable_schema_and_templates); {
    const json::Value c = services::algorithm::contract();
    CHECK_EQ(c["schema_version"].as_string(), std::string("libs.algorithm.v1"));
    CHECK(c["input_template"].is_object());
    CHECK(c["output_template"].is_object());
    CHECK(!c["input_template_valid"].as_bool());
    CHECK_EQ(c["output_template"]["availability"].as_string(), std::string("algorithm_missing"));
    CHECK(c["output_template"]["composition"]["values"]["COMPO_P2O5"].is_null());
}

CASE(valid_input_returns_capture_summary); {
    const auto result = services::algorithm::validate_input(valid_input());
    CHECK(result.ok());
    if (result) {
        CHECK(result.value()["valid"].as_bool());
        CHECK_EQ(result.value()["channel_capture_count"].as_number(), 1.0);
        CHECK_EQ(result.value()["sample_count"].as_number(), 3.0);
    }
}

CASE(empty_template_is_rejected); {
    const auto result = services::algorithm::validate_input(services::algorithm::contract()["input_template"]);
    CHECK(!result.ok());
    if (!result.ok()) CHECK_EQ(result.error().kind, libs::ErrorKind::invalid_argument);
}

CASE(source_format_and_real_spectrum_are_required); {
    json::Value input = valid_input();
    input.as_object().at("spectrum").set("spectrum_source", json::Value("NIR"));
    auto result = services::algorithm::validate_input(input);
    CHECK(!result.ok());
    input = valid_input();
    input.as_object().at("spectrum").set("spectrum_format", json::Value("counts_only"));
    result = services::algorithm::validate_input(input);
    CHECK(!result.ok());
}

CASE(lengths_and_duplicate_serials_are_rejected); {
    json::Value input = valid_input();
    auto& spectrum = input.as_object().at("spectrum");
    auto& groups = spectrum.as_object().at("scan_groups");
    auto& group = groups.as_array().at(0);
    auto& channels = group.as_object().at("channels");
    channels.as_array().at(0).set("counts", json::array());
    auto result = services::algorithm::validate_input(input);
    CHECK(!result.ok());
    input = valid_input();
    auto& channels2 = input.as_object().at("spectrum").as_object().at("scan_groups")
        .as_array().at(0).as_object().at("channels");
    channels2.push_back(channel("CH1"));
    result = services::algorithm::validate_input(input);
    CHECK(!result.ok());
}

CASE(analyze_never_invents_values_or_model_outputs); {
    const auto result = services::algorithm::analyze_placeholder(valid_input());
    CHECK(result.ok());
    if (result) {
        CHECK_EQ(result.value()["availability"].as_string(), std::string("algorithm_missing"));
        CHECK(!result.value()["executed"].as_bool());
        CHECK(result.value()["composition"]["values"]["COMPO_TFE"].is_null());
        CHECK(result.value()["classification"]["label"].is_null());
        CHECK(result.value()["similarity"]["score"].is_null());
    }
}

CASE(raw_capture_alias_is_accepted_but_still_requires_libs_shape); {
    json::Value input = valid_input();
    input.set("raw_capture", input["spectrum"]);
    input.as_object().erase("spectrum");
    const auto result = services::algorithm::validate_input(input);
    CHECK(result.ok());
    if (result) CHECK_EQ(result.value()["input_source_key"].as_string(), std::string("raw_capture"));
}

TEST_MAIN_RETURN()
