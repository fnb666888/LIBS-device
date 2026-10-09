#include "libs/business/measurement/prediction/algorithm_contract.hpp"

#include <cmath>
#include <set>
#include <string>

namespace business::measurement::prediction::algorithm {
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
    output.set("availability", json::Value("unavailable"));
    output.set("executed", json::Value(false));
    json::Value status = json::object();
    status.set("state", json::Value("not_executed"));
    status.set("reason", json::Value("no_prediction_yet"));
    status.set("message", json::Value("输出模板无预测值；调用 analyze 才执行模型，输入校验成功不代表预测成功"));
    output.set("status", std::move(status));

    json::Value composition = json::object();
    composition.set("availability", json::Value("unavailable"));
    composition.set("unit", json::Value("mass_fraction_percent"));
    composition.set("unit_confirmed", json::Value(true));
    composition.set("phosphorus_definition", "elemental_P; COMPO_P2O5 is not predicted");
    composition.set("target_values", json::object());
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
    output.set("results", json::array());
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
    result.set("availability", json::Value("runtime_check_required"));
    result.set("model_id", "iron_ore_current_mixed_v2");
    result.set("model_status_action", "status");
    result.set("required_wavelength_nm", json::Array{json::Value(200), json::Value(565)});
    result.set("minimum_accepted_wavelength_nm", json::Array{json::Value(200), json::Value(564.7)});
    result.set("prediction_note", "当前 v2 为 240 维特征；真机末端可使用固定 564.7 nm 网格，保留适配与域外标记；模型状态为实际加载版本的依据");
    json::Value actions = json::array();
    for (const char* action : {"contract", "status", "latest", "configure", "validate", "analyze"})
        actions.push_back(json::Value(action));
    result.set("actions", std::move(actions));
    result.set("analyze_execution", "asynchronous; accepted/request_id then poll latest/status");

    json::Value input = json::object();
    input.set("schema_version", json::Value(kInputSchema));
    input.set("request_id", json::Value(""));
    input.set("context", json::object());
    json::Value spectrum = json::object();
    spectrum.set("spectrum_source", json::Value("LIBS"));
    spectrum.set("spectrum_format", json::Value("wavelength_nm+counts"));
    spectrum.set("scan_groups", json::array());
    input.set("spectrum", std::move(spectrum));
    json::Value parameters = json::object(); parameters.set("branch", "oxide");
    input.set("parameters", std::move(parameters));
    input.set("model", json::object());
    result.set("input_template", std::move(input));
    result.set("input_template_valid", json::Value(false));
    result.set("template_note", json::Value("空模板仅描述结构；须填 request_id 与真实采集谱才能校验"));
    result.set("output_template", output_template(""));

    json::Value fields = json::array();
    add_field(fields, "input.schema_version", "string", true, "固定 libs.algorithm.v1");
    add_field(fields, "input.request_id", "string", true, "调用方提供的非空关联 ID，原样回传");
    add_field(fields, "input.context", "object", false, "站点、带号、报关单号、采集时间等业务上下文；不推断缺失值");
    add_field(fields, "input.spectrum", "object", true, "原始光谱采集 的完整原始 LIBS 载荷；旧 raw_capture 可作别名");
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
    add_field(fields, "input.parameters.branch", "string", true, "analyze 必须显式指定 oxide 或 sulfide；不根据文件名推断矿种");
    add_field(fields, "input.model.id", "string", false, "须匹配已加载模型的 model_id；当前 iron_ore_current_mixed_v2；固定预处理，不接受调用方覆盖");
    add_field(fields, "output.composition.target_values", "object", true, "模型原始十项质量分数%，含元素 P；COMPO_P2O5 保持 null");
    add_field(fields, "output.composition.values", "object", true, "海科同名十项 COMPO_*，缺结果一律 null；COMPO_P2O5 不由元素 P 自动换算");
    add_field(fields, "output.classification", "object", true, "标签、置信度为 null，类别数组为空");
    add_field(fields, "output.similarity", "object", true, "分数、参照 ID 为 null，匹配数组为空");
    add_field(fields, "output.quality", "object", true, "保留低信噪比、域外、铁池约束及同步未确认等状态；结构校验通过不是质量合格");
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

}  // namespace business::measurement::prediction::algorithm
