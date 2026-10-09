#include "check.hpp"
#include "libs/business/measurement/prediction/model_inference.hpp"

TEST_MAIN_BEGIN()

CASE(missing_runtime_config_is_an_explicit_error); {
    business::measurement::prediction::ModelInference model;
    model.configure("build/missing_model_runtime_config");
    const auto result = model.status();
    CHECK(!result.ok());
    if (!result) CHECK_EQ(result.error().kind, libs::ErrorKind::unsupported);
    model.stop(); model.stop();
}

CASE(invalid_input_is_rejected_before_starting_python); {
    business::measurement::prediction::ModelInference model;
    model.configure("build/missing_model_runtime_config");
    const auto result = model.analyze(business::json::object());
    CHECK(!result.ok());
    if (!result) CHECK_EQ(result.error().kind, libs::ErrorKind::invalid_argument);
}

CASE(no_prediction_is_unknown_not_a_zero_result); {
    business::measurement::prediction::ModelInference model;
    const auto result = model.latest();
    CHECK_EQ(result["availability"].as_string(), std::string("unavailable"));
    CHECK_EQ(result["reason"].as_string(), std::string("no_prediction_yet"));
    CHECK(!result["executed"].as_bool());
}

TEST_MAIN_RETURN()
