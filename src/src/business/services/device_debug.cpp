#include "libs/business/services/facade.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "libs/devices/air_conditioner.hpp"
#include "libs/devices/indicator.hpp"
#include "libs/devices/laser.hpp"
#include "libs/devices/rangefinder.hpp"
#include "libs/protocols/air_conditioner.hpp"
#include "libs/protocols/indicator.hpp"
#include "libs/protocols/rangefinder.hpp"
#include "libs/hex.hpp"

namespace business::app {
namespace {

using business::json::Value;

Response debug_error(libs::ErrorKind kind, const std::string& code, const std::string& message) {
    return Response::failure(libs::Error::make(kind, code, message));
}

bool has_only(const Value& args, std::initializer_list<const char*> names) {
    for (const auto& kv : args.as_object()) {
        bool allowed = false;
        for (const char* name : names) if (kv.first == name) { allowed = true; break; }
        if (!allowed) return false;
    }
    return true;
}

bool required_string(const Value& args, const char* key, std::string& out) {
    if (!args.has(key) || !args[key].is_string() || args[key].as_string().empty()) return false;
    out = args[key].as_string();
    return true;
}

Value transaction(const libs::Bytes& request, const libs::Bytes& response,
                  unsigned elapsed_ms, unsigned receive_errors = 0,
                  bool timed_out = false) {
    Value v = business::json::object();
    v.set("request_hex", Value(libs::to_hex(request)));
    v.set("response_hex", Value(libs::to_hex(response)));
    v.set("elapsed_ms", Value(static_cast<double>(elapsed_ms)));
    v.set("receive_errors", Value(static_cast<double>(receive_errors)));
    v.set("timed_out", Value(timed_out));
    return v;
}

Value laser_field(const libs::laser::Field& f) {
    Value v = business::json::object();
    v.set("name", Value(f.name));
    v.set("unit", Value(f.unit));
    v.set("raw", Value(static_cast<double>(f.raw)));
    v.set("raw32", Value(static_cast<double>(f.wide ? f.raw32 : f.raw)));
    v.set("wide", Value(f.wide));
    v.set("scale", Value(f.scale));
    v.set("scaled", Value(f.scaled));
    if (f.scaled) {
        const double raw = static_cast<double>(f.wide ? f.raw32 : f.raw);
        v.set("value", Value(raw * f.scale));
    } else {
        v.set("value", Value());
    }
    return v;
}

Value laser_frame(const libs::laser::Frame& frame) {
    Value v = business::json::object();
    v.set("kind", Value(frame.kind));
    v.set("command", Value(static_cast<double>(static_cast<std::uint8_t>(frame.command))));
    v.set("payload_hex", Value(libs::to_hex(frame.payload)));
    v.set("notes", Value(frame.notes));
    Value fields = business::json::array();
    for (const auto& f : frame.fields) fields.push_back(laser_field(f));
    v.set("fields", std::move(fields));
    return v;
}

Value range_reading(const libs::rangefinder::Reading& reading) {
    Value v = business::json::object();
    v.set("address", Value(static_cast<double>(reading.address)));
    v.set("start", Value(static_cast<double>(reading.start)));
    v.set("count", Value(static_cast<double>(reading.count)));
    Value registers = business::json::array();
    for (const auto& r : reading.registers) {
        Value item = business::json::object();
        item.set("address", Value(static_cast<double>(r.address)));
        item.set("name", Value(r.name));
        item.set("raw", Value(static_cast<double>(r.raw)));
        item.set("unit", Value(r.unit));
        item.set("valid", Value(r.valid));
        registers.push_back(std::move(item));
    }
    v.set("registers", std::move(registers));
    return v;
}

Value ac_status(const libs::air_conditioner::StatusReading& reading) {
    Value v = business::json::object();
    v.set("address", Value(static_cast<double>(reading.address)));
    v.set("start", Value(static_cast<double>(reading.start)));
    v.set("count", Value(static_cast<double>(reading.count)));
    Value bits = business::json::array();
    for (const auto& bit : reading.bits) {
        Value b = business::json::object();
        b.set("address", Value(static_cast<double>(bit.address)));
        b.set("name", Value(bit.name));
        b.set("active", Value(bit.active));
        b.set("is_alarm", Value(bit.is_alarm));
        bits.push_back(std::move(b));
    }
    v.set("bits", std::move(bits));
    Value raw = business::json::array();
    for (std::uint8_t b : reading.raw_bytes) raw.push_back(Value(static_cast<double>(b)));
    v.set("raw_bytes", std::move(raw));
    return v;
}

Value ac_params(const libs::air_conditioner::ParameterReading& reading) {
    Value v = business::json::object();
    v.set("address", Value(static_cast<double>(reading.address)));
    v.set("start", Value(static_cast<double>(reading.start)));
    v.set("count", Value(static_cast<double>(reading.count)));
    Value regs = business::json::array();
    for (const auto& r : reading.registers) {
        Value item = business::json::object();
        item.set("address", Value(static_cast<double>(r.address)));
        item.set("name", Value(r.name));
        item.set("raw", Value(static_cast<double>(r.raw)));
        item.set("unit", Value(r.unit));
        item.set("scale", Value(r.scale));
        item.set("value", Value(r.value));
        item.set("documented", Value(r.documented));
        item.set("is_signed", Value(r.is_signed));
        item.set("writable", Value(r.writable));
        regs.push_back(std::move(item));
    }
    v.set("registers", std::move(regs));
    return v;
}

const libs::air_conditioner::ParamRegister* find_param(
        const libs::air_conditioner::ParameterReading& reading, std::uint16_t address) {
    for (const auto& r : reading.registers) if (r.address == address) return &r;
    return nullptr;
}

bool channel_from(const Value& args, const char* key, libs::indicator::Channel& channel) {
    std::string text;
    if (!required_string(args, key, text)) return false;
    const auto parsed = libs::indicator::parse_channel(text);
    if (!parsed) return false;
    channel = parsed.value();
    return true;
}

bool mode_from(const Value& args, libs::air_conditioner::Mode& mode) {
    std::string text;
    if (!required_string(args, "mode", text)) return false;
    if (text == "standby") mode = libs::air_conditioner::Mode::standby;
    else if (text == "cooling") mode = libs::air_conditioner::Mode::cooling;
    else if (text == "heating") mode = libs::air_conditioner::Mode::heating;
    else if (text == "dehumidify") mode = libs::air_conditioner::Mode::dehumidify;
    else return false;
    return true;
}

bool setpoint_from(const Value& args, std::uint16_t& reg) {
    std::string name;
    if (!required_string(args, "parameter", name)) return false;
    if (name == "cooling_on") reg = libs::air_conditioner::reg::kCoolingOnSetpoint;
    else if (name == "cooling_off") reg = libs::air_conditioner::reg::kCoolingOffSetpoint;
    else if (name == "heating_on") reg = libs::air_conditioner::reg::kHeatingOnSetpoint;
    else if (name == "heating_off") reg = libs::air_conditioner::reg::kHeatingOffSetpoint;
    else if (name == "indoor_high_alarm") reg = libs::air_conditioner::reg::kIndoorHighAlarmSetpoint;
    else if (name == "indoor_low_alarm") reg = libs::air_conditioner::reg::kIndoorLowAlarmSetpoint;
    else return false;
    return true;
}

} // namespace

Response BusinessFacade::cmd_device_debug(const json::Value& args) {
    if (!args.is_object() || !args.has("device") || !args.has("action") ||
        !args["device"].is_string() || !args["action"].is_string())
        return debug_error(libs::ErrorKind::invalid_argument, "device_debug_shape",
                           "device_debug 需要 device 与 action 字符串");
    const std::string device = args["device"].as_string();
    const std::string action = args["action"].as_string();
    std::lock_guard<std::mutex> lock(runtime_.device_mutex());
    Value out = business::json::object();
    out.set("device", Value(device));
    out.set("action", Value(action));

    if (device == "laser") {
        if (action != "status" || !has_only(args, {"device", "action"}))
            return debug_error(libs::ErrorKind::invalid_argument, "laser_debug_action",
                               "laser 仅支持 action=status；禁止裸寄存器写入和出光绕过门禁");
        if (runtime_.devices().laser() == nullptr)
            return debug_error(libs::ErrorKind::unsupported, "laser_not_open", "激光器未打开");
        const auto actual = runtime_.devices().laser()->read(libs::laser::command::kLdActual);
        if (!actual) return Response::failure(actual.error());
        const auto settings = runtime_.devices().laser()->read(libs::laser::command::kLdSettings);
        if (!settings) return Response::failure(settings.error());
        out.set("read_only", Value(true));
        out.set("actual", laser_frame(actual.value().frame));
        out.set("actual_transaction", transaction(actual.value().request, actual.value().response,
                                                   actual.value().elapsed_ms, actual.value().receive_errors,
                                                   actual.value().timed_out));
        out.set("settings", laser_frame(settings.value().frame));
        out.set("settings_transaction", transaction(settings.value().request, settings.value().response,
                                                     settings.value().elapsed_ms, settings.value().receive_errors,
                                                     settings.value().timed_out));
        return Response::success(std::move(out));
    }

    if (device == "rangefinder") {
        if (action != "read")
            return debug_error(libs::ErrorKind::invalid_argument, "rangefinder_debug_action",
                               "rangefinder 仅支持 action=read");
        if (!has_only(args, {"device", "action", "start", "count"}))
            return debug_error(libs::ErrorKind::invalid_argument, "rangefinder_debug_field",
                               "rangefinder.read 只接受可选 start/count");
        if (runtime_.devices().rangefinder() == nullptr)
            return debug_error(libs::ErrorKind::unsupported, "rangefinder_not_open", "测距仪未打开");
        std::uint16_t start = libs::rangefinder::reg::kDistanceZeroOnOutOfRange;
        std::uint16_t count = 9;
        if (args.has("start")) {
            if (!args["start"].is_number() || args["start"].as_number() != std::floor(args["start"].as_number()))
                return debug_error(libs::ErrorKind::invalid_argument, "rangefinder_start_bad", "start 必须是整数");
            const double value = args["start"].as_number();
            if (value < 0x0200 || value > 0x0208) return debug_error(libs::ErrorKind::invalid_argument,
                "rangefinder_start_bad", "start 必须在 0x0200..0x0208");
            start = static_cast<std::uint16_t>(value);
        }
        if (args.has("count")) {
            if (!args["count"].is_number() || args["count"].as_number() != std::floor(args["count"].as_number()))
                return debug_error(libs::ErrorKind::invalid_argument, "rangefinder_count_bad", "count 必须是整数");
            const double value = args["count"].as_number();
            if (value < 1 || value > 9) return debug_error(libs::ErrorKind::invalid_argument,
                "rangefinder_count_bad", "count 必须在 1..9");
            count = static_cast<std::uint16_t>(value);
        }
        if (static_cast<unsigned>(start) + count > 0x0209)
            return debug_error(libs::ErrorKind::invalid_argument, "rangefinder_range_bad",
                               "start+count 超出 0x0200..0x0208");
        const auto read = runtime_.devices().rangefinder()->read(runtime_.config().rangefinder_address,
                                                                  start, count);
        if (!read) return Response::failure(read.error());
        out.set("read_only", Value(true));
        out.set("reading", range_reading(read.value().reading));
        out.set("transaction", transaction(read.value().request, read.value().response,
                                            read.value().elapsed_ms, read.value().receive_errors,
                                            read.value().timed_out));
        return Response::success(std::move(out));
    }

    if (device == "air_conditioner") {
        if (runtime_.devices().air_conditioner() == nullptr)
            return debug_error(libs::ErrorKind::unsupported, "air_conditioner_not_open", "空调未打开");
        auto* ac = runtime_.devices().air_conditioner();
        if (action == "status") {
            if (!has_only(args, {"device", "action"})) return debug_error(libs::ErrorKind::invalid_argument,
                "air_conditioner_debug_field", "air_conditioner.status 不接受其他字段");
            const auto status = ac->read_status(runtime_.config().air_conditioner_address);
            if (!status) return Response::failure(status.error());
            const auto params = ac->read_params(runtime_.config().air_conditioner_address);
            if (!params) return Response::failure(params.error());
            out.set("read_only", Value(true));
            out.set("status", ac_status(status.value().reading));
            out.set("status_transaction", transaction(status.value().request, status.value().response,
                                                        status.value().elapsed_ms, status.value().receive_errors,
                                                        status.value().timed_out));
            out.set("parameters", ac_params(params.value().reading));
            out.set("parameters_transaction", transaction(params.value().request, params.value().response,
                                                           params.value().elapsed_ms, params.value().receive_errors,
                                                           params.value().timed_out));
            return Response::success(std::move(out));
        }
        if (action == "set_power") {
            if (!has_only(args, {"device", "action", "on"}) || !args.has("on") || !args["on"].is_bool())
                return debug_error(libs::ErrorKind::invalid_argument, "air_conditioner_power_bad",
                                   "set_power 需要 on:boolean");
            const bool on = args["on"].as_bool();
            const auto write = ac->set_power(runtime_.config().air_conditioner_address, on);
            if (!write) return Response::failure(write.error());
            const auto verify = ac->read_status(runtime_.config().air_conditioner_address);
            if (!verify) return Response::failure(verify.error());
            const auto* bit = [&]() -> const libs::air_conditioner::StatusBit* {
                for (const auto& b : verify.value().reading.bits)
                    if (b.address == libs::air_conditioner::reg::kPowerState) return &b;
                return nullptr;
            }();
            if (bit == nullptr || bit->active != on)
                return debug_error(libs::ErrorKind::device, "air_conditioner_power_not_confirmed",
                                   "空调开关写入成功但 10042 回读未确认");
            out.set("command_ack", Value(true)); out.set("confirmed", Value(true));
            out.set("on", Value(on));
            out.set("transaction", transaction(write.value().request, write.value().raw, write.value().elapsed_ms));
            out.set("verify", ac_status(verify.value().reading));
            return Response::success(std::move(out));
        }
        if (action == "set_mode") {
            if (!has_only(args, {"device", "action", "mode"})) return debug_error(libs::ErrorKind::invalid_argument,
                "air_conditioner_mode_bad", "set_mode 需要 mode=standby/cooling/heating/dehumidify");
            libs::air_conditioner::Mode mode{};
            if (!mode_from(args, mode)) return debug_error(libs::ErrorKind::invalid_argument,
                "air_conditioner_mode_bad", "mode 必须是 standby/cooling/heating/dehumidify");
            const auto write = ac->set_mode(runtime_.config().air_conditioner_address, mode);
            if (!write) return Response::failure(write.error());
            const auto verify = ac->read_params(runtime_.config().air_conditioner_address);
            if (!verify) return Response::failure(verify.error());
            const auto* reg = find_param(verify.value().reading, libs::air_conditioner::reg::kMode);
            if (reg == nullptr || reg->raw != static_cast<std::uint16_t>(mode))
                return debug_error(libs::ErrorKind::device, "air_conditioner_mode_not_confirmed",
                                   "空调模式写入成功但 30026 回读未确认");
            out.set("command_ack", Value(true)); out.set("confirmed", Value(true));
            out.set("mode", Value(libs::air_conditioner::to_string(mode)));
            out.set("transaction", transaction(write.value().request, write.value().raw, write.value().elapsed_ms));
            out.set("verify", ac_params(verify.value().reading));
            out.set("note", Value("仅下发一次模式命令；未启动100秒保持循环"));
            return Response::success(std::move(out));
        }
        if (action == "set_setpoint") {
            if (!has_only(args, {"device", "action", "parameter", "value"}) ||
                !args.has("value") || !args["value"].is_number() ||
                !std::isfinite(args["value"].as_number()))
                return debug_error(libs::ErrorKind::invalid_argument, "air_conditioner_setpoint_bad",
                                   "set_setpoint 需要白名单 parameter 与有限数值 value");
            std::uint16_t reg = 0;
            if (!setpoint_from(args, reg)) return debug_error(libs::ErrorKind::invalid_argument,
                "air_conditioner_setpoint_bad", "parameter 必须是 cooling_on/cooling_off/heating_on/heating_off/indoor_high_alarm/indoor_low_alarm");
            const auto write = ac->set_setpoint(runtime_.config().air_conditioner_address, reg,
                                                args["value"].as_number());
            if (!write) return Response::failure(write.error());
            const auto verify = ac->read_params(runtime_.config().air_conditioner_address);
            if (!verify) return Response::failure(verify.error());
            const auto* p = find_param(verify.value().reading, reg);
            if (p == nullptr) return debug_error(libs::ErrorKind::device,
                "air_conditioner_setpoint_not_confirmed", "设定点写入成功但参数回读缺失");
            out.set("command_ack", Value(true)); out.set("confirmed", Value(true));
            out.set("parameter", Value(args["parameter"].as_string()));
            out.set("requested_value", Value(args["value"].as_number()));
            out.set("verify_value", Value(p->value)); out.set("verify_raw", Value(static_cast<double>(p->raw)));
            out.set("transaction", transaction(write.value().request, write.value().raw, write.value().elapsed_ms));
            return Response::success(std::move(out));
        }
        return debug_error(libs::ErrorKind::invalid_argument, "air_conditioner_debug_action",
                           "air_conditioner action 只能是 status/set_power/set_mode/set_setpoint");
    }

    if (device == "indicator") {
        if (runtime_.devices().indicator() == nullptr)
            return debug_error(libs::ErrorKind::unsupported, "indicator_not_open", "状态灯未打开");
        auto* indicator = runtime_.devices().indicator();
        libs::indicator::Channel channel{};
        if (!channel_from(args, "channel", channel)) return debug_error(libs::ErrorKind::invalid_argument,
            "indicator_channel_bad", "channel 必须是 red/yellow/green/blue/white/buzzer");
        if (action == "query") {
            if (!has_only(args, {"device", "action", "channel"})) return debug_error(libs::ErrorKind::invalid_argument,
                "indicator_query_bad", "query 不接受其他字段");
            const auto q = indicator->query(runtime_.config().indicator_address, channel);
            if (!q) return Response::failure(q.error());
            out.set("read_only", Value(true));
            out.set("channel", Value(libs::indicator::to_string(channel)));
            out.set("state", Value(libs::indicator::to_string(q.value().response.state)));
            out.set("raw_state", Value(static_cast<double>(q.value().response.raw_state)));
            out.set("transaction", transaction(q.value().request, q.value().raw_response,
                                                q.value().elapsed_ms, q.value().receive_errors,
                                                q.value().timed_out));
            return Response::success(std::move(out));
        }
        if (action == "write") {
            if (!has_only(args, {"device", "action", "channel", "state"})) return debug_error(libs::ErrorKind::invalid_argument,
                "indicator_write_bad", "write 需要 channel/state，型号取站点配置");
            std::string state_text;
            if (!required_string(args, "state", state_text)) return debug_error(libs::ErrorKind::invalid_argument,
                "indicator_state_bad", "state 必须是 off/on/blink_1hz/blink_2hz");
            const auto state = libs::indicator::parse_state(state_text);
            if (!state) return debug_error(libs::ErrorKind::invalid_argument,
                "indicator_state_bad", "state 必须是 off/on/blink_1hz/blink_2hz");
            if (runtime_.config().indicator_model.empty()) return debug_error(libs::ErrorKind::invalid_argument,
                "indicator_model_missing", "站点配置缺少 indicator_model，拒绝猜测型号");
            libs::indicator::WriteRequest request{runtime_.config().indicator_model, channel, state.value()};
            const auto write = indicator->write(runtime_.config().indicator_address, request);
            if (!write) return Response::failure(write.error());
            const auto verify = indicator->query(runtime_.config().indicator_address, channel);
            if (!verify) return Response::failure(verify.error());
            if (verify.value().response.state != state.value()) return debug_error(libs::ErrorKind::device,
                "indicator_write_not_confirmed", "状态灯写入回显存在，但查询未确认目标状态");
            out.set("command_ack", Value(true)); out.set("confirmed", Value(true));
            out.set("channel", Value(libs::indicator::to_string(channel)));
            out.set("state", Value(libs::indicator::to_string(state.value())));
            out.set("transaction", transaction(write.value().write_request, write.value().write_response,
                                                write.value().elapsed_ms, write.value().receive_errors));
            out.set("verify_raw_state", Value(static_cast<double>(verify.value().response.raw_state)));
            return Response::success(std::move(out));
        }
        return debug_error(libs::ErrorKind::invalid_argument, "indicator_debug_action",
                           "indicator action 只能是 query/write");
    }

    return debug_error(libs::ErrorKind::invalid_argument, "device_debug_device",
                       "device 只能是 laser/rangefinder/air_conditioner/indicator");
}

} // namespace business::app
