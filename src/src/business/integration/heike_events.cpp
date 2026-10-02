#include "libs/business/integration/heike_events.hpp"

#include <cctype>

namespace business::integration::heike {
namespace {

using libs::Error;
using libs::ErrorKind;
using libs::Result;

Error invalid(const std::string& message) {
    return Error::make(ErrorKind::invalid_argument, "heike_bad_report", message);
}

bool nonempty(const std::string& value) { return !value.empty(); }

bool looks_like_uuid(const std::string& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-')
        return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

Result<json::Value> normalized_content(const json::Value& value) {
    if (value.is_object() || value.is_array()) return value;
    // 附件3把 eventContent 定义为 JSON 字符串，而模型示例又直接放 object；
    // 输入层兼容字符串，但仍严格解析，禁止把普通文本当成报文。
    if (value.is_string()) {
        json::Value parsed;
        const json::ParseError error = json::parse(value.as_string(), parsed);
        if (!error.ok || (!parsed.is_object() && !parsed.is_array()))
            return invalid("eventContent string must contain a JSON object or array");
        return parsed;
    }
    return invalid("eventContent must be a JSON object, array, or JSON string");
}

libs::VoidResult require_identity(const DeviceIdentity& identity) {
    if (!nonempty(identity.equipment_id)) return invalid("equipmentID is required");
    if (!nonempty(identity.equipment_type)) return invalid("equipmentType is required");
    if (!nonempty(identity.timeseries)) return invalid("timeseries is required");
    return libs::VoidResult();
}

json::Value identity_fields(const DeviceIdentity& identity) {
    json::Value v = json::object();
    v.set("equipmentID", json::Value(identity.equipment_id));
    v.set("equipmentType", json::Value(identity.equipment_type));
    if (!identity.customs.empty()) v.set("customs", json::Value(identity.customs));
    v.set("timeseries", json::Value(identity.timeseries));
    return v;
}

}  // namespace

libs::Result<json::Value> make_envelope(const EnvelopeInput& input) {
    if (!nonempty(input.biz_id)) return invalid("bizId is required");
    if (!nonempty(input.customs_code)) return invalid("customsCode is required");
    if (!nonempty(input.event_code)) return invalid("eventCode is required");
    if (!nonempty(input.event_source)) return invalid("eventSource is required");
    if (!looks_like_uuid(input.message_id))
        return invalid("messageId must be a UUID");
    if (!nonempty(input.content_type)) return invalid("contentType must not be empty");

    const Result<json::Value> content = normalized_content(input.event_content);
    if (!content) return content.error();
    const std::string content_text = json::stringify(content.value());

    json::Value v = json::object();
    v.set("bizId", json::Value(input.biz_id));
    v.set("contentSize", json::Value(std::to_string(content_text.size())));
    v.set("contentType", json::Value(input.content_type));
    v.set("customsCode", json::Value(input.customs_code));
    v.set("eventCode", json::Value(input.event_code));
    if (input.event_content_as_string)
        v.set("eventContent", json::Value(content_text));
    else
        v.set("eventContent", content.value());
    v.set("eventSource", json::Value(input.event_source));
    v.set("messageId", json::Value(input.message_id));
    if (!input.notes.empty()) v.set("notes", json::Value(input.notes));
    if (!input.publisher.empty()) v.set("publisher", json::Value(input.publisher));
    if (!input.receiver_channel_name.empty())
        v.set("receiverChannelName", json::Value(input.receiver_channel_name));
    if (!input.version.empty()) v.set("version", json::Value(input.version));
    return v;
}

libs::Result<json::Value> make_heartbeat(const DeviceIdentity& identity) {
    const libs::VoidResult valid = require_identity(identity);
    if (!valid) return valid.error();
    return identity_fields(identity);
}

libs::Result<json::Value> make_runtime_event(const DeviceIdentity& identity,
                                             const std::string& code,
                                             const std::string& value) {
    const libs::VoidResult valid = require_identity(identity);
    if (!valid) return valid.error();
    if ((code != "0" && code != "1") || (value != "0" && value != "1"))
        return invalid("EVENT data.code/data.value must be string 0 or 1");

    json::Value v = identity_fields(identity);
    json::Value data = json::object();
    data.set("code", json::Value(code));
    data.set("value", json::Value(value));
    v.set("data", std::move(data));
    return v;
}

libs::Result<json::Value> make_openinit(const DeviceIdentity& identity,
                                        const std::vector<DailyStat>& stats) {
    const libs::VoidResult valid = require_identity(identity);
    if (!valid) return valid.error();
    if (stats.empty()) return invalid("startupInitCount must not be empty");

    json::Value v = identity_fields(identity);
    json::Value list = json::array();
    for (const DailyStat& item : stats) {
        if (item.date.empty()) return invalid("startupInitCount.date is required");
        if (item.count < 0 || item.online_period_s < 0)
            return invalid("startupInitCount count/onlinePeriod must be non-negative");
        json::Value one = json::object();
        one.set("date", json::Value(item.date));
        one.set("count", json::Value(static_cast<double>(item.count)));
        one.set("onlinePeriod", json::Value(static_cast<double>(item.online_period_s)));
        list.push_back(std::move(one));
    }
    v.set("startupInitCount", std::move(list));
    return v;
}

libs::Result<json::Value> make_alarm(const DeviceIdentity& identity,
                                     const std::vector<Alarm>& alarms) {
    const libs::VoidResult valid = require_identity(identity);
    if (!valid) return valid.error();
    if (alarms.empty()) return invalid("ALARM data must not be empty");

    json::Value v = identity_fields(identity);
    json::Value list = json::array();
    for (const Alarm& alarm : alarms) {
        if (alarm.name.empty() || alarm.type.empty() || alarm.firsttime.empty() ||
            alarm.time.empty() || alarm.value.empty())
            return invalid("ALARM item requires name/type/firsttime/time/value");
        json::Value one = json::object();
        one.set("name", json::Value(alarm.name));
        one.set("type", json::Value(alarm.type));
        if (!alarm.color.empty()) one.set("color", json::Value(alarm.color));
        one.set("firsttime", json::Value(alarm.firsttime));
        one.set("time", json::Value(alarm.time));
        one.set("value", json::Value(alarm.value));
        if (!alarm.remake.empty()) one.set("remake", json::Value(alarm.remake));
        list.push_back(std::move(one));
    }
    v.set("data", std::move(list));
    return v;
}

libs::Result<json::Value> make_libs_raw_event(const LibsCaptureContext& context,
                                              const json::Value& raw_capture,
                                              bool event_content_as_string) {
    if (context.biz_id.empty() || context.customs_code.empty() || context.belt_code.empty() ||
        context.edge_code.empty() || context.collect_time.empty() || context.event_code.empty() ||
        context.message_id.empty())
        return invalid("LIBS raw event requires biz_id/customs_code/belt_code/edge_code/"
                       "collect_time/event_code/message_id");
    if (!raw_capture.is_object() && !raw_capture.is_array())
        return invalid("LIBS raw capture must be a JSON object or array");

    json::Value content = json::object();
    content.set("SPECTRUM_SOURCE", json::Value("LIBS"));
    content.set("SPECTRUM_FORMAT", json::Value("wavelength_nm+counts"));
    content.set("BELT_CODE", json::Value(context.belt_code));
    content.set("CUSTOMS_DECLA_NO", json::Value(context.biz_id));
    content.set("COLLECT_TIME", json::Value(context.collect_time));
    content.set("EDGE_CODE", json::Value(context.edge_code));
    content.set("SEQ_NO", json::Value(static_cast<double>(context.seq_no)));
    content.set("RAW_SPECTRUM", raw_capture);
    content.set("LIBS_UPLOAD_NOTE", json::Value(
        "原始激光诱导击穿光谱；不能直接当作 PISYS001 近红外十项成分或 MODEL003 相似度结果"));

    EnvelopeInput input;
    input.biz_id = context.biz_id;
    input.customs_code = context.customs_code;
    input.event_code = context.event_code;
    input.event_source = context.event_source;
    input.message_id = context.message_id;
    input.event_content = std::move(content);
    input.event_content_as_string = event_content_as_string;
    return make_envelope(input);
}

libs::Result<json::Value> make_model_event(const EnvelopeInput& input) {
    if (input.event_code != "MODEL001" && input.event_code != "MODEL002" &&
        input.event_code != "MODEL003" && input.event_code != "MODEL004")
        return invalid("model event_code must be MODEL001..MODEL004");
    if (!input.event_content.is_object())
        return invalid("model eventContent must be an object supplied by the model");
    return make_envelope(input);
}

std::string model_event_name(const std::string& event_code) {
    if (event_code == "MODEL001") return "BrandIdentify";
    if (event_code == "MODEL002") return "ElementIdentify";
    if (event_code == "MODEL003") return "SpectrumIdentify";
    if (event_code == "MODEL004") return "AppearanceIdentify";
    return {};
}

}  // namespace business::integration::heike
