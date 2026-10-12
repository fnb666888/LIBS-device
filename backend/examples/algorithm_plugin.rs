use libs_backend::algorithm::PROTOCOL;
use serde_json::{Value, json};
use std::io::{BufRead, Write};

fn description() -> Value {
    json!({"availability":"available","implemented":true,"model_id":"algorithm_template","version":"0.1.0",
        "input_schema":"libs.algorithm.v1","output_schema":"libs.algorithm.result.v1",
        "modes":["calibration_free","calibrated"],"parameter_schema":{"type":"object"}})
}
fn analyze(input: &Value) -> Value {
    // Replace this body with the complete algorithm, including its preprocessing and quality checks.
    json!({"schema_version":"libs.algorithm.result.v1","request_id":input["request_id"],
        "availability":"unavailable","executed":false,"mode":input["parameters"]["mode"],"status":{"state":"not_executed","reason":"algorithm_not_implemented"},
        "composition":{"availability":"unavailable","unit":"mass_fraction_percent","unit_confirmed":false,"values":{},"target_values":{}},
        "quality":{"status":"unavailable","issues":["algorithm_not_implemented"]},"model":description()})
}
fn main() {
    let mut output = std::io::stdout().lock();
    for line in std::io::stdin().lock().lines() {
        let request: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let mut reply = json!({"protocol":PROTOCOL,"id":request["id"],"ok":true});
        match request["action"].as_str() {
            Some("describe") => reply["payload"] = description(),
            Some("analyze") => reply["payload"] = analyze(&request["input"]),
            _ => {
                reply["ok"] = false.into();
                reply["error"] =
                    json!({"code":"unsupported_action","message":"Expected describe or analyze"});
            }
        }
        writeln!(output, "{reply}").unwrap();
        output.flush().unwrap();
    }
}
