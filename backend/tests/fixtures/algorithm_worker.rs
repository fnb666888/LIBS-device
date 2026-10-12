use libs_backend::algorithm::PROTOCOL;
use serde_json::{Value, json};
use std::io::{BufRead, Write};

fn main() {
    let args: Vec<_> = std::env::args().collect();
    let version = args.get(1).map_or("v1", String::as_str);
    let behavior = args.get(2).map_or("normal", String::as_str);
    let model = json!({"availability":"available","implemented":true,"model_id":"test_fixture","version":version,
        "input_schema":"libs.algorithm.v1","output_schema":"libs.algorithm.result.v1","modes":["calibration_free","calibrated"]});
    let mut output = std::io::stdout().lock();
    for line in std::io::stdin().lock().lines() {
        let request: Value = serde_json::from_str(&line.unwrap()).unwrap();
        let mut reply = json!({"protocol":PROTOCOL,"id":request["id"],"ok":true});
        if request["action"] == "describe" {
            reply["payload"] = model.clone();
            writeln!(output, "{reply}").unwrap();
            output.flush().unwrap();
            if behavior == "block_input" {
                std::thread::sleep(std::time::Duration::from_secs(60));
            }
            continue;
        }
        if behavior == "crash" {
            std::process::exit(7);
        }
        if behavior == "timeout" {
            std::thread::sleep(std::time::Duration::from_secs(3));
        }
        let input = &request["input"];
        let counts = input["spectrum"]["scan_groups"][0]["channels"][0]["counts"]
            .as_array()
            .unwrap();
        let factor = if input["parameters"]["mode"] == "calibrated" {
            input["parameters"]["calibration"]["factor"]
                .as_f64()
                .unwrap()
        } else {
            1.
        };
        let value = counts.iter().map(|c| c.as_f64().unwrap()).sum::<f64>() * factor;
        reply["payload"] = json!({"schema_version":"libs.algorithm.result.v1","request_id":input["request_id"],
            "availability":"available","executed":true,"mode":input["parameters"]["mode"],"status":{"state":"completed","reason":"test_fixture"},
            "composition":{"availability":"available","unit":"mass_fraction_percent","unit_confirmed":true,"values":{"COMPO_TFE":value},"target_values":{"TFe":value}},
            "quality":{"status":"test_fixture","issues":[]},"model":model,"parameters_received":input["parameters"]});
        if behavior == "bad_id" {
            reply["id"] = 12345.into();
        }
        if behavior == "bad_result" {
            reply["payload"]["request_id"] = "wrong".into();
        }
        writeln!(output, "{reply}").unwrap();
        output.flush().unwrap();
    }
}
