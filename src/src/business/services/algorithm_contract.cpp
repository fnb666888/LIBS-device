#include "libs/business/services/algorithm_contract.hpp"

#include <cmath>
#include <set>
#include <string>

namespace business::services::algorithm {
namespace {

constexpr const char* kInputSchema = "libs.algorithm.v1";
constexpr const char* kOutputSchema = "libs.algorithm.result.v1";
constexpr const char* kComponents[] = {
    "COMPO_AL2O3", "COMPO_CAO", "COMPO_FEO", "COMPO_MGO", "COMPO_MNO",
    "COMPO_P2O5", "COMPO_S", "COMPO_SIO2", "COMPO_TFE", "COMPO_TIO2"};

libs::Error invalid(const std::string& path, const std::string& message) {
    return libs::Error::make(libs::ErrorKind::invalid_argument,
                            "algorithm_input_invalid", path + ": " + message);
}

bool nonempty_string(const json::Value& value) {
    if (!value.is_string()) return false;
    return value.as_string().find_first_not_of(" \t\r\n") != std::string::npos;
}

bool finite_integer(const json::Value& value, double minimum) {
    if (!value.is_number()) return false;
    const double number = value.as_number();
    return std::isfinite(number) && number >= minimum &&
           number <= 9007199254740991.0 && std::floor(number) == number;
}

const json::Value& spectrum_of(const json::Value& input) {
    // raw_capture 是本地旧载荷的兼容入口，仍须有明确 LIBS 来源和格式。
    return input.has("spectrum") ? input["spectrum"] : input["raw_capture"];
}

json::Value output_template(const std::string& request_id) {
    json::Value output = json::object();
    output.set("schema_version", json::Value(kOutputSchema));
    output.set("request_id", json::Value(request_id));
    output.set("availability", json::Value("algorithm_missing"));
    output.set("executed", json::Value(false));
    json::Value status = json::object();
    status.set("state", json::Value("not_executed"));
    status.set("reason", json::Value("algorithm_missing"));
    status.set("message", json::Value("算法尚未接入；输入校验成功不代表产生分析结果"));
    output.set("status", std::move(status));

    json::Value composition = json::object();
    composition.set("availability", json::Value("algorithm_missing"));
    composition.set("unit", json::Value("percent"));
    composition.set("unit_confirmed", json::Value(false));
    composition.set("unit_note", json::Value("百分比及质量分数口径待模型与标定方案确认"));
    json::Value values = json::object();
    for (const char* key : kComponents) values.set(key, json::Value());
    composition.set("values", std::move(values));
    output.set("composition", std::move(composition));

    json::Value classification = json::object();
    classification.set("label", json::Value());
    classification.set("confidence", json::Value());
    classification.set("classes", json::array());
    output.set("classification", std::move(classification));
    json::Value similarity = json::object();
    similarity.set("score", json::Value());
    similarity.set("reference_id", json::Value());
    similarity.set("matches", json::array());
    output.set("similarity", std::move(similarity));
    json::Value quality = json::object();
    quality.set("status", json::Value("not_evaluated"));
    quality.set("issues", json::array());
    output.set("quality", std::move(quality));
    output.set("context", json::object());
    output.set("model", json::Value());
    return output;
}

void add_field(json::Value& fields, const char* path, const char* type,
               bool required, const char* description) {
    json::Value field = json::object();
    field.set("path", json::Value(path));
    field.set("type", json::Value(type));
    field.set("required", json::Value(required));
    field.set("description", json::Value(description));
    fields.push_back(std::move(field));
}

}  // namespace

json::Value contract() {
    json::Value result = json::object();
    result.set("schema_version", json::Value(kInputSchema));
    result.set("output_schema_version", json::Value(kOutputSchema));
    result.set("availability", json::Value("algorithm_missing"));
    json::Value actions = json::array();
    for (const char* action : {"contract", "validate", "analyze"})
        actions.push_back(json::Value(action));
    result.set("actions", std::move(actions));

    json::Value input = json::object();
    input.set("schema_version", json::Value(kInputSchema));
    input.set("request_id", json::Value(""));
    input.set("context", json::object());
    json::Value spectrum = json::object();
    spectrum.set("spectrum_source", json::Value("LIBS"));
    spectrum.set("spectrum_format", json::Value("wavelength_nm+counts"));
    spectrum.set("scan_groups", json::array());
    input.set("spectrum", std::move(spectrum));
    input.set("parameters", json::object());
    input.set("model", json::object());
    result.set("input_template", std::move(input));
    result.set("input_template_valid", json::Value(false));
    result.set("template_note", json::Value("空模板仅描述结构；须填 request_id 与真实采集谱才能校验"));
    result.set("output_template", output_template(""));

    json::Value fields = json::array();
    add_field(fields, "input.schema_version", "string", true, "固定 libs.algorithm.v1");
    add_field(fields, "input.request_id", "string", true, "调用方提供的非空关联 ID，原样回传");
    add_field(fields, "input.context", "object", false, "站点、带号、报关单号、采集时间等业务上下文；不推断缺失值");
    add_field(fields, "input.spectrum", "object", true, "CaptureCoordinator::to_payload 的完整原始 LIBS 载荷；旧 raw_capture 可作别名");
    add_field(fields, "input.spectrum.spectrum_source", "string", true, "固定 LIBS；不能传近红外原始谱");
    add_field(fields, "input.spectrum.spectrum_format", "string", true, "固定 wavelength_nm+counts");
    add_field(fields, "input.spectrum.scan_groups", "array", true, "至少一组，每组至少一个真实采到的通道；缺失通道不补零");
    add_field(fields, "input.spectrum.scan_groups[].shot_id", "integer", false, "非负且在 JSON 精确整数范围内；采集载荷已有时原样携带");
    add_field(fields, "input.spectrum.scan_groups[].complete", "boolean", false, "原样保留采集完整性；false 不会被改成 true");
    add_field(fields, "input.spectrum.scan_groups[].hardware_synchronized", "boolean", false, "原样携带同步标记，不由通道数量推断");
    add_field(fields, "input.spectrum.scan_groups[].arm_order", "array<string>", false, "布防顺序，元素须为非空序列号");
    add_field(fields, "input.spectrum.scan_groups[].channels", "array", true, "同一组内 serial 必须唯一；可接受部分通道并报告缺失元数据");
    add_field(fields, "input.spectrum.scan_groups[].channels[].serial", "string", true, "非空设备序列号");
    add_field(fields, "input.spectrum.scan_groups[].channels[].pixels", "integer", false, "正整数且等于两个数组长度");
    add_field(fields, "input.spectrum.scan_groups[].channels[].device_time_ticks", "integer", false, "非负设备时间计数，禁止与墙钟时间混用");
    add_field(fields, "input.spectrum.scan_groups[].channels[].wavelength_nm", "array<number>", true, "非空，正且有限的 nm 数值；不重采样或排序");
    add_field(fields, "input.spectrum.scan_groups[].channels[].counts", "array<number>", true, "非空且与波长等长，所有强度有限；不转成含量");
    add_field(fields, "input.parameters", "object", false, "算法窗口、预处理等参数的预留对象，算法未接入时不应用");
    add_field(fields, "input.model", "object", false, "模型 ID/版本等的预留对象，算法未接入时不加载");
    add_field(fields, "output.composition.values", "object", true, "海科同名十项 COMPO_*，缺结果一律 null；COMPO_P2O5 不由元素 P 自动换算");
    add_field(fields, "output.classification", "object", true, "标签、置信度为 null，类别数组为空");
    add_field(fields, "output.similarity", "object", true, "分数、参照 ID 为 null，匹配数组为空");
    add_field(fields, "output.quality", "object", true, "算法质量未评估；结构校验通过不是质量合格");
    result.set("fields", std::move(fields));
    return result;
}

libs::Result<json::Value> validate_input(const json::Value& input) {
    if (!input.is_object()) return invalid("input", "must be an object");
    if (!input["schema_version"].is_string() || input["schema_version"].as_string() != kInputSchema)
        return invalid("input.schema_version", "must be libs.algorithm.v1");
    if (!nonempty_string(input["request_id"])) return invalid("input.request_id", "must be a non-empty string");
    json::Value missing = json::array();
    for (const char* key : {"context", "parameters", "model"}) {
        if (input.has(key)) {
            if (!input[key].is_object()) return invalid(std::string("input.") + key, "must be an object when supplied");
        } else missing.push_back(json::Value(std::string("input.") + key));
    }
    const json::Value& spectrum = spectrum_of(input);
    const std::string spectrum_path = input.has("spectrum") ? "input.spectrum" : "input.raw_capture";
    if (!spectrum.is_object()) return invalid("input.spectrum", "must supply an object containing real captured spectra");
    if (!spectrum["spectrum_source"].is_string() || spectrum["spectrum_source"].as_string() != "LIBS")
        return invalid(spectrum_path + ".spectrum_source", "must be LIBS");
    if (!spectrum["spectrum_format"].is_string() || spectrum["spectrum_format"].as_string() != "wavelength_nm+counts")
        return invalid(spectrum_path + ".spectrum_format", "must be wavelength_nm+counts");
    const json::Value& groups = spectrum["scan_groups"];
    if (!groups.is_array() || groups.as_array().empty())
        return invalid(spectrum_path + ".scan_groups", "must be a non-empty array of real captures");

    std::size_t channel_count = 0;
    std::size_t sample_count = 0;
    std::set<std::string> all_serials;
    json::Value incomplete_groups = json::array();
    json::Value missing_metadata = json::array();
    for (std::size_t gi = 0; gi < groups.as_array().size(); ++gi) {
        const json::Value& group = groups.as_array()[gi];
        const std::string gp = spectrum_path + ".scan_groups[" + std::to_string(gi) + "]";
        if (!group.is_object()) return invalid(gp, "must be an object");
        if (group.has("shot_id") && !finite_integer(group["shot_id"], 0))
            return invalid(gp + ".shot_id", "must be a non-negative exact integer");
        for (const char* key : {"shot_id", "complete", "hardware_synchronized", "arm_order"})
            if (!group.has(key)) missing_metadata.push_back(json::Value(gp + "." + key));
        for (const char* key : {"complete", "hardware_synchronized"})
            if (group.has(key) && !group[key].is_bool()) return invalid(gp + "." + key, "must be boolean");
        if (group.has("complete") && !group["complete"].as_bool())
            incomplete_groups.push_back(json::Value(static_cast<double>(gi)));
        if (group.has("arm_order")) {
            if (!group["arm_order"].is_array()) return invalid(gp + ".arm_order", "must be an array");
            for (const auto& serial : group["arm_order"].as_array())
                if (!nonempty_string(serial)) return invalid(gp + ".arm_order", "each entry must be a non-empty serial string");
        }
        const json::Value& channels = group["channels"];
        if (!channels.is_array() || channels.as_array().empty())
            return invalid(gp + ".channels", "must contain at least one captured channel");
        std::set<std::string> group_serials;
        for (std::size_t ci = 0; ci < channels.as_array().size(); ++ci) {
            const json::Value& channel = channels.as_array()[ci];
            const std::string cp = gp + ".channels[" + std::to_string(ci) + "]";
            if (!channel.is_object()) return invalid(cp, "must be an object");
            if (!nonempty_string(channel["serial"])) return invalid(cp + ".serial", "must be a non-empty string");
            const std::string serial = channel["serial"].as_string();
            if (!group_serials.insert(serial).second) return invalid(cp + ".serial", "duplicate serial within this scan group");
            all_serials.insert(serial);
            const json::Value& wavelength = channel["wavelength_nm"];
            const json::Value& counts = channel["counts"];
            if (!wavelength.is_array() || wavelength.as_array().empty()) return invalid(cp + ".wavelength_nm", "must be a non-empty number array");
            if (!counts.is_array() || counts.as_array().empty()) return invalid(cp + ".counts", "must be a non-empty number array");
            if (counts.as_array().size() != wavelength.as_array().size()) return invalid(cp, "wavelength_nm and counts lengths must match");
            if (channel.has("pixels")) {
                if (!finite_integer(channel["pixels"], 1) || channel["pixels"].as_number() != static_cast<double>(counts.as_array().size()))
                    return invalid(cp + ".pixels", "must be a positive exact integer equal to array lengths");
            } else missing_metadata.push_back(json::Value(cp + ".pixels"));
            if (channel.has("device_time_ticks")) {
                if (!finite_integer(channel["device_time_ticks"], 0)) return invalid(cp + ".device_time_ticks", "must be a non-negative exact integer");
            } else missing_metadata.push_back(json::Value(cp + ".device_time_ticks"));
            for (std::size_t pi = 0; pi < counts.as_array().size(); ++pi) {
                const auto& nm = wavelength.as_array()[pi];
                const auto& count = counts.as_array()[pi];
                if (!nm.is_number() || !std::isfinite(nm.as_number()) || nm.as_number() <= 0)
                    return invalid(cp + ".wavelength_nm[" + std::to_string(pi) + "]", "must be a positive finite number");
                if (!count.is_number() || !std::isfinite(count.as_number()))
                    return invalid(cp + ".counts[" + std::to_string(pi) + "]", "must be a finite number");
            }
            ++channel_count;
            sample_count += counts.as_array().size();
        }
    }
    json::Value summary = json::object();
    summary.set("valid", json::Value(true));
    summary.set("schema_version", json::Value(kInputSchema));
    summary.set("request_id", input["request_id"]);
    summary.set("spectrum_source", json::Value("LIBS"));
    summary.set("scan_group_count", json::Value(static_cast<double>(groups.as_array().size())));
    summary.set("channel_capture_count", json::Value(static_cast<double>(channel_count)));
    summary.set("sample_count", json::Value(static_cast<double>(sample_count)));
    json::Value serials = json::array();
    for (const auto& serial : all_serials) serials.push_back(json::Value(serial));
    summary.set("channel_serials", std::move(serials));
    summary.set("incomplete_group_indices", std::move(incomplete_groups));
    summary.set("missing_optional_fields", std::move(missing));
    summary.set("missing_metadata", std::move(missing_metadata));
    summary.set("input_source_key", json::Value(input.has("spectrum") ? "spectrum" : "raw_capture"));
    summary.set("note", json::Value("仅校验结构和数值有效性；不判断光谱质量、物理同步或成分准确性"));
    return summary;
}

libs::Result<json::Value> validate_algorithm_input(const json::Value& input) {
    return validate_input(input);
}

libs::Result<json::Value> analyze_placeholder(const json::Value& input) {
    const auto checked = validate_input(input);
    if (!checked) return checked.error();
    json::Value output = output_template(input["request_id"].as_string());
    output.set("validation", checked.value());
    if (input.has("context")) output.set("context", input["context"]);
    output.set("model_request", input.has("model") ? input["model"] : json::object());
    output.set("parameters_applied", json::Value(false));
    return output;
}

}  // namespace business::services::algorithm
