// 设备驱动层测试：注入假 transport::Link，不碰真实串口、不加载 DLL。
// 验证的是"驱动是否把一次事务正确地组帧发出、并把回包解码成结构化结果"。
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "libs/devices/indicator.hpp"
#include "libs/devices/laser.hpp"
#include "libs/devices/rangefinder.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/laser.hpp"
#include "libs/protocols/modbus_rtu.hpp"

using libs::Bytes;
using namespace libs;

namespace {

// 可编程的假传输：按脚本逐次回放预置字节。
class FakeLink final : public transport::Link {
public:
    struct Exchange {
        Bytes request;
        unsigned timeout_ms = 0;
    };

    std::vector<Exchange> sent;                 // 记录发出去的每一帧
    std::deque<Result<transport::Reply>> replies; // 预置回包；空则默认回显请求
    transport::Reply last_reply;
    bool fail_next = false;
    std::string port = "FAKE0";

    Result<transport::Reply> exchange(const Bytes& request,
                                      unsigned timeout_ms = 650,
                                      unsigned idle_ms = 80) override {
        (void)idle_ms;
        sent.push_back(Exchange{request, timeout_ms});
        if (fail_next) {
            fail_next = false;
            return Error::make(ErrorKind::transport, "cable_unplugged", "fake transport failure");
        }
        const bool is_read =
            (request.size() >= 5 && request[0] == 0x5A && request[1] == 0xA5 &&
             (request[4] == laser::command::kLdSettings ||
              request[4] == laser::command::kLdActual ||
              request[4] == laser::command::kTec)) ||
            (request.size() >= 2 && (request[1] == 0x01 || request[1] == 0x03));
        if (is_read && !replies.empty()) {
            auto r = replies.front();
            replies.pop_front();
            if (!r) return r.error();
            last_reply = r.value();
            return r;
        }
        // 默认：设备把请求原样回显（真实 RS485 转换器最常见的行为）
        transport::Reply reply;
        reply.bytes = request;
        reply.elapsed_ms = 1;
        last_reply = reply;
        return reply;
    }

    std::string endpoint() const override { return port; }

    void queue(const Bytes& bytes, unsigned errors = 0, bool timed_out = false) {
        transport::Reply r;
        r.bytes = bytes;
        r.elapsed_ms = 2;
        r.receive_errors = errors;
        r.timed_out = timed_out;
        replies.push_back(Result<transport::Reply>::success(r));
    }
    void queue_error(Error e) { replies.push_back(Result<transport::Reply>::failure(std::move(e))); }
};

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// 造一条 modbus FC03 正常响应。
Bytes modbus_read_reply(std::uint8_t address, std::uint16_t start, std::uint16_t count,
                        const std::vector<std::uint16_t>& values) {
    Bytes f{address, 0x03, static_cast<std::uint8_t>(count * 2)};
    for (const auto v : values) {
        f.push_back(static_cast<std::uint8_t>(v >> 8));
        f.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    (void)start;   // FC03 响应不回显起始寄存器
    const auto crc = modbus::crc16(f.data(), f.size());
    f.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    f.push_back(static_cast<std::uint8_t>(crc >> 8));
    return f;
}

Bytes laser_b2_reply() {
    Bytes payload(22, 0x00);
    payload[0] = 0x01;   // work_state = 工作
    return laser::make_frame(0xB2, payload);
}

}  // namespace

TEST_MAIN_BEGIN()

// -- Laser -------------------------------------------------------------------
{
    CASE(laser_read_sends_b2_and_decodes);
    FakeLink link;
    link.queue(concat({laser::make_read_request(0xB2), laser_b2_reply()}));
    devices::Laser laser(link);
    devices::LaserOperationTrace trace;
    auto tx = laser.readActual(&trace);
    CHECK(tx.ok());
    CHECK_EQ(link.sent.size(), size_t(1));
    CHECK_EQ(trace.sent_frames.size(), size_t(1));
    if (!trace.sent_frames.empty())
        CHECK_EQ(trace.sent_frames[0], to_hex(laser::make_read_request(0xB2)));
    CHECK(tx.value().valid);
    CHECK_EQ(static_cast<int>(tx.value().work_state), 1);
}

{
    CASE(laser_read_ignores_echo_and_noise);
    // 真实现场：请求回显 + 若干噪声字节 + 一帧有效响应
    Bytes stream = concat({from_hex("AA"), laser::make_read_request(0xB2),
                           from_hex("00 FF"), laser_b2_reply(), from_hex("5C")});
    FakeLink link;
    link.queue(stream);
    devices::Laser laser(link);
    devices::LaserOperationTrace trace;
    auto tx = laser.readActual(&trace);
    CHECK(tx.ok());
    CHECK(tx.value().valid);
}

{
    CASE(laser_read_rejects_two_complete_frames);
    // 一次事务里出现两帧完整 B2：无法判断哪帧回答本次请求
    Bytes stream = concat({laser_b2_reply(), laser_b2_reply()});
    FakeLink link;
    link.queue(stream);
    devices::Laser laser(link);
    devices::LaserOperationTrace trace;
    auto tx = laser.readActual(&trace);
    CHECK(!tx.ok());
    CHECK_EQ(tx.error().code, std::string("ambiguous_reply"));
}

{
    CASE(laser_surfaces_uart_errors);
    FakeLink link;
    link.queue(laser_b2_reply(), /*errors=*/3);
    devices::Laser laser(link);
    devices::LaserOperationTrace trace;
    auto tx = laser.readActual(&trace);
    CHECK(!tx.ok());
    CHECK(tx.error().kind == ErrorKind::transport);
    CHECK_EQ(tx.error().code, std::string("uart_errors"));
}

{
    CASE(laser_propagates_transport_failure);
    FakeLink link;
    link.fail_next = true;
    devices::Laser laser(link);
    devices::LaserOperationTrace trace;
    auto tx = laser.readActual(&trace);
    CHECK(!tx.ok());
    CHECK(tx.error().kind == ErrorKind::transport);
    CHECK_EQ(tx.error().code, std::string("cable_unplugged"));
}


// -- Rangefinder -------------------------------------------------------------
{
    CASE(rangefinder_read_roundtrip);
    FakeLink link;
    link.queue(modbus_read_reply(1, 0x0200, 3, {10, 0xFFFF, 1234}));
    devices::Rangefinder rf(link);
    auto read = rf.readRawRegisters(0x0200, 3);
    CHECK(read.ok());
    CHECK_EQ(to_hex(link.sent[0].request), to_hex(rangefinder::make_read_request(1, 0x0200, 3)));
    CHECK_EQ(read.value().registers.size(), size_t(3));
    CHECK_EQ(int(read.value().registers[0].raw), 10);
    CHECK(read.value().registers[0].valid);
    CHECK(!read.value().registers[1].valid);    // 0xFFFF 哨兵
    CHECK_EQ(int(read.value().registers[2].raw), 1234);
    CHECK_EQ(read.value().address, 1);
}

{
    CASE(rangefinder_skips_request_echo);
    // 真实适配器会把请求回显出来；驱动必须只认响应帧。
    Bytes stream = concat({rangefinder::make_read_request(1, 0x0200, 2),
                           modbus_read_reply(1, 0x0200, 2, {5, 6})});
    FakeLink link;
    link.queue(stream);
    devices::Rangefinder rf(link);
    auto read = rf.readRawRegisters(0x0200, 2);
    CHECK(read.ok());
    CHECK_EQ(read.value().registers.size(), size_t(2));
    CHECK_EQ(int(read.value().registers[0].raw), 5);
}

{
    CASE(rangefinder_rejects_bad_range_without_io);
    FakeLink link;
    devices::Rangefinder rf(link);
    auto read = rf.readRawRegisters(0x0200, 0);   // count=0 非法
    CHECK(!read.ok());
    CHECK(read.error().kind == ErrorKind::invalid_argument);
    CHECK_EQ(link.sent.size(), size_t(0));
}

{
    CASE(rangefinder_surfaces_modbus_exception);
    Bytes exception{0x01, 0x83, 0x02};
    const auto crc = modbus::crc16(exception.data(), exception.size());
    exception.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    exception.push_back(static_cast<std::uint8_t>(crc >> 8));
    FakeLink link;
    link.queue(exception);
    devices::Rangefinder rf(link);
    auto read = rf.readRawRegisters(0x0200, 2);
    CHECK(!read.ok());
    CHECK(read.error().kind == ErrorKind::device);
    CHECK_EQ(read.error().code, std::string("modbus_exception"));
}

// -- Indicator ---------------------------------------------------------------
{
    CASE(indicator_apply_verifies_state);
    // 科隆威私有 8 字节回包：地址 功能码 00 寄存器号 00 状态 CRC
    const std::uint16_t reg = indicator::query_register(indicator::Channel::red);
    Bytes state{0x01, 0x01, 0x00, static_cast<std::uint8_t>(reg), 0x00, 0x00};
    const auto crc = modbus::crc16(state.data(), state.size());
    state.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    state.push_back(static_cast<std::uint8_t>(crc >> 8));
    FakeLink link;
    link.queue(concat({indicator::make_query_request(1, indicator::Channel::red), state}));
    devices::Indicator lamp(link, 1, "TL50-485", 650, 0);
    auto q = lamp.apply(indicator::Channel::red, indicator::State::off, true);
    CHECK(q.ok());
    CHECK(q.value().state == indicator::State::off);
}

{
    CASE(indicator_write_reports_echo);
    FakeLink link;
    devices::Indicator lamp(link, 1, "TL50-485", 650, 0);
    auto w = lamp.apply(indicator::Channel::green, indicator::State::on, false);
    CHECK(w.ok());
    CHECK(w.value().write_echo_seen);
    CHECK_EQ(w.value().model, std::string("TL50-485"));
    CHECK(w.value().channel == indicator::Channel::green);
    CHECK(w.value().state == indicator::State::on);
}

{
    CASE(indicator_write_rejects_unknown_model_without_io);
    FakeLink link;
    devices::Indicator lamp(link, 1, "MYSTERY-LAMP", 650, 0);
    auto w = lamp.apply(indicator::Channel::red, indicator::State::on, false);
    CHECK(!w.ok());
    CHECK(w.error().kind == ErrorKind::unsupported);
    CHECK_EQ(link.sent.size(), size_t(0));
}

{
    CASE(k50_complete_state_resets_then_switches_color_and_verifies_supported_channels);
    using Ch = indicator::Channel;
    using St = indicator::State;
    FakeLink link;
    devices::Indicator lamp(link, 1, "CL50S+K50-485", 650, 0);
    const auto queue_states = [&](Ch active) {
        for (const auto ch : {Ch::red, Ch::yellow, Ch::green, Ch::buzzer}) {
            Bytes reply{1, 1, 0, static_cast<std::uint8_t>(indicator::query_register(ch)),
                        0, static_cast<std::uint8_t>(ch == active ? 0xFF : 0)};
            const auto crc = modbus::crc16(reply.data(), reply.size());
            reply.push_back(static_cast<std::uint8_t>(crc));
            reply.push_back(static_cast<std::uint8_t>(crc >> 8));
            link.queue(reply);
        }
    };
    const Bytes reset{1, 5, 0, 0, 0, 0, 0xCD, 0xCA};
    std::map<Ch, St> intent{{Ch::red, St::off}, {Ch::yellow, St::on},
        {Ch::green, St::off}, {Ch::blue, St::off}, {Ch::white, St::off}, {Ch::buzzer, St::off}};
    queue_states(Ch::yellow);
    auto yellow = lamp.applyAll(intent, true);
    CHECK(yellow.ok());
    CHECK_EQ(yellow.value().queries.size(), size_t(4));
    CHECK_EQ(yellow.value().writes.size(), size_t(1));
    CHECK(yellow.value().reset_echo_seen);
    CHECK(link.sent[0].request == reset);
    CHECK(link.sent[1].request == (Bytes{1, 5, 0, 3, 0xFF, 0, 0x7C, 0x3A}));
    intent[Ch::yellow] = St::off;
    intent[Ch::green] = St::on;
    queue_states(Ch::green);
    auto green = lamp.applyAll(intent, true);
    CHECK(green.ok());
    CHECK(link.sent[6].request == reset);
    CHECK(link.sent[7].request == (Bytes{1, 5, 0, 2, 0xFF, 0, 0x2D, 0xFA}));
    CHECK_EQ(link.sent.size(), size_t(12));
    intent[Ch::green] = St::off;
    queue_states(Ch::blue); // nonexistent blue is never queried: all supported channels off
    auto off = lamp.applyAll(intent, true);
    CHECK(off.ok());
    CHECK(off.value().writes.empty());
    CHECK(link.sent[12].request == reset);
    CHECK_EQ(link.sent.size(), size_t(17));
}

{
    CASE(indicator_complete_state_validates_before_any_io);
    using Ch = indicator::Channel;
    using St = indicator::State;
    FakeLink link;
    devices::Indicator lamp(link, 1, "CL50S+K50-485", 650, 0);
    auto unsupported = lamp.applyAll({{Ch::red, St::on}, {Ch::blue, St::on}}, true);
    CHECK(!unsupported.ok());
    CHECK_EQ(unsupported.error().code, std::string("model_has_no_color"));
    CHECK(link.sent.empty());
    link.fail_next = true;
    auto broken = lamp.applyAll({{Ch::green, St::on}}, true);
    CHECK(!broken.ok());
    CHECK_EQ(broken.error().code, std::string("cable_unplugged"));
    CHECK_EQ(link.sent.size(), size_t(1)); // no color write after a failed reset
}

{
    CASE(indicator_complete_state_rejects_stale_color_readback);
    FakeLink link;
    Bytes stale{1, 1, 0, 1, 0, 0xFF}; // red still on after a green intent
    const auto crc = modbus::crc16(stale.data(), stale.size());
    stale.push_back(static_cast<std::uint8_t>(crc));
    stale.push_back(static_cast<std::uint8_t>(crc >> 8));
    link.queue(stale);
    devices::Indicator lamp(link, 1, "CL50S+K50-485", 650, 0);
    auto result = lamp.applyAll({{indicator::Channel::green, indicator::State::on}}, true);
    CHECK(!result.ok());
    CHECK_EQ(result.error().code, std::string("indicator_state_mismatch"));
}

TEST_MAIN_RETURN()
