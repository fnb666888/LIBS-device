#include "libs/protocols/indicator.hpp"

#include <algorithm>
#include <stdexcept>

#include "libs/protocols/modbus_rtu.hpp"

namespace libs::indicator {
namespace {
using Bytes = libs::Bytes;

struct ChannelCoil {
    std::uint16_t on = 0;
    std::uint16_t blink_1hz = 0;   // 0 表示该型号没有登记这一档
    std::uint16_t blink_2hz = 0;
};

ChannelCoil coil_for(const ModelTraits& t, Channel channel) {
    if (channel == Channel::red)     return {1, 6, 0x16};
    if (channel == Channel::yellow)  return t.k50 ? ChannelCoil{3, 8, 0x18} : ChannelCoil{2, 7, 0x17};
    if (channel == Channel::green)   return t.k50 ? ChannelCoil{2, 7, 0x17} : ChannelCoil{3, 8, 0x18};
    if (channel == Channel::buzzer)  return {4, 9, 0x19};
    if (channel == Channel::blue)    return {0x11, 0x21, 0x23};
    return {0x12, 0x22, 0x24};  // white
}

}  // namespace

Result<Channel> parse_channel(const std::string& name) {
    if (name == "red") return Channel::red;
    if (name == "yellow") return Channel::yellow;
    if (name == "green") return Channel::green;
    if (name == "blue") return Channel::blue;
    if (name == "white") return Channel::white;
    if (name == "buzzer") return Channel::buzzer;
    return Error::make(ErrorKind::invalid_argument, "unknown_channel",
                       "Unknown lamp channel: " + name);
}

Result<State> parse_state(const std::string& name) {
    if (name == "off") return State::off;
    if (name == "on") return State::on;
    if (name == "blink-1hz") return State::blink_1hz;
    if (name == "blink-2hz") return State::blink_2hz;
    return Error::make(ErrorKind::invalid_argument, "unknown_state",
                       "Unknown lamp state: " + name);
}

const char* to_string(Channel channel) noexcept {
    switch (channel) {
        case Channel::red: return "red";
        case Channel::yellow: return "yellow";
        case Channel::green: return "green";
        case Channel::blue: return "blue";
        case Channel::white: return "white";
        case Channel::buzzer: return "buzzer";
    }
    return "red";
}

const char* to_string(State state) noexcept {
    switch (state) {
        case State::off: return "off";
        case State::on: return "on";
        case State::blink_1hz: return "blink-1hz";
        case State::blink_2hz: return "blink-2hz";
    }
    return "off";
}

bool lookup_model(const std::string& model, ModelTraits& traits) {
    static const ModelTraits kKnown[] = {
        {"CL50S+K50-485", true,  false, false, false, false},
        {"RL30-24V",      false, true,  false, true,  true},
        {"CL50S-USB-5V",  false, false, false, true,  true},
        {"TL50-485",      false, false, false, true,  true},
        {"CL60-485",      false, false, true,  true,  true},
        {"485-10-30V",    false, false, true,  true,  true},
        {"485-24V-WK",    false, false, true,  true,  true},
    };
    const auto it = std::find_if(std::begin(kKnown), std::end(kKnown),
                                 [&](const ModelTraits& t) { return t.model == model; });
    if (it == std::end(kKnown)) return false;
    traits = *it;
    return true;
}

std::uint16_t query_register(Channel channel) {
    switch (channel) {
        case Channel::red: return 1;
        case Channel::green: return 2;
        case Channel::blue: return 3;
        case Channel::yellow: return 4;
        case Channel::white: return 5;
        case Channel::buzzer: return 6;
    }
    throw std::invalid_argument("unknown lamp channel");
}

Bytes make_query_request(std::uint8_t address, Channel channel) {
    if (!modbus::is_unicast_address(address))
        throw std::invalid_argument("Modbus address must be 1..247");
    return modbus::make_request(address, 0x01, query_register(channel), 1);
}

Result<QueryResponse> decode_query_response(const Bytes& capture, std::uint8_t address, Channel channel) {
    const Bytes request = make_query_request(address, channel);
    const auto reg = query_register(channel);
    auto located = modbus::locate_reply(capture, address, 0x01, request, 8,
                                        [reg](const std::uint8_t* p) {
                                            // 科隆威是厂商私有格式，不是标准 FC01：
                                            // 地址 功能码 00 寄存器号 00 状态字节 CRC，共 8 字节。
                                            // 状态字节取值 00/FF 表示关，11/22 对应蓝/白常亮档。
                                            // 这里只按文档已知集合收窄候选帧，不判断"该不该亮"。
                                            const bool known_state =
                                                p[5] == 0x00 || p[5] == 0xFF || p[5] == 0x11 || p[5] == 0x22;
                                            return p[2] == 0x00 && p[3] == static_cast<std::uint8_t>(reg) &&
                                                   p[4] == 0x00 && known_state;
                                        });
    if (!located) return located.error();
    const auto& frame = located.value();
    if (frame.exception)
        return Error::with_raw(ErrorKind::device, "modbus_exception",
                               "the lamp returned a Modbus exception (code " +
                                   std::to_string(frame.exception_code) + ")",
                               frame.bytes);

    QueryResponse response;
    response.channel = channel;
    response.register_address = query_register(channel);
    response.raw_state = frame.bytes[5];
    switch (response.raw_state) {
        case 0x00: response.state = State::off; break;
        case 0xFF: response.state = State::on; break;
        case 0x11: response.state = State::blink_1hz; break;
        default:   response.state = State::blink_2hz; break;   // 0x22
    }
    return response;
}

Result<Bytes> make_write_request(std::uint8_t address, const WriteRequest& request) {
    if (!modbus::is_unicast_address(address))
        return Error::make(ErrorKind::invalid_argument, "bad_address",
                           "Modbus address must be 1..247");
    ModelTraits traits;
    if (!lookup_model(request.model, traits))
        return Error::make(ErrorKind::unsupported, "unknown_model",
                           "Unknown lamp model '" + request.model +
                               "'; a documented model is required to build a write frame");
    if (request.channel == Channel::buzzer && traits.rgb_only)
        return Error::make(ErrorKind::unsupported, "model_has_no_buzzer",
                           "Model " + traits.model + " has no documented buzzer command");
    if ((request.channel == Channel::blue || request.channel == Channel::white) && !traits.has_blue_white)
        return Error::make(ErrorKind::unsupported, "model_has_no_color",
                           "Model " + traits.model + " has no documented blue/white channel");

    const auto coil = coil_for(traits, request.channel);
    if (request.state == State::off) {
        if (!traits.individual_off)
            return Error::make(ErrorKind::unsupported, "no_individual_off",
                               "Model " + traits.model +
                                   " has no documented individual-off command; use the all-off coil 0");
        return modbus::make_request(address, 0x05, coil.on, 0x0000);
    }
    std::uint16_t target = 0;
    switch (request.state) {
        case State::on:        target = coil.on; break;
        case State::blink_1hz:
            if (!traits.slow_blink_is_1hz || coil.blink_1hz == 0)
                return Error::make(ErrorKind::unsupported, "slow_blink_not_1hz",
                                   "Model " + traits.model +
                                       " does not document slow blink as exactly 1 Hz");
            target = coil.blink_1hz; break;
        case State::blink_2hz:
            if (coil.blink_2hz == 0)
                return Error::make(ErrorKind::unsupported, "unsupported_state",
                                   "Model " + traits.model + " has no documented 2 Hz state");
            target = coil.blink_2hz; break;
        case State::off: break;   // handled above
    }
    return modbus::make_request(address, 0x05, target, 0xFF00);
}

}  // namespace libs::indicator


