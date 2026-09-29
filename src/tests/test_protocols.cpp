// 纯协议层测试：不碰串口、不加载 DLL，全部是纯函数的编解码往返与错误分支。
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <stdexcept>

#include "check.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/indicator.hpp"
#include "libs/protocols/laser.hpp"
#include "libs/protocols/modbus_rtu.hpp"
#include "libs/protocols/rangefinder.hpp"

using libs::Bytes;
using namespace libs;

namespace {

// 已知正确的 Modbus 帧（CRC 与独立实现核对过）。
const Bytes kReadRequest10 = from_hex("01 03 00 00 00 0A C5 CD");
const Bytes kReadReply2 = from_hex("01 03 02 00 2A 39 9B");
const Bytes kException = from_hex("01 83 02 C0 F1");

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// 构造一条 B2 响应帧（22 字节负载）。
Bytes make_b2_frame() {
    Bytes payload(22, 0x00);
    payload[0] = 0x01;   // work_state
    payload[9] = 0x00; payload[10] = 0x0A;  // actual_current = 10
    return laser::make_frame(0xB2, payload);
}

}  // namespace

TEST_MAIN_BEGIN()

// -- hex ---------------------------------------------------------------------
{
    CASE(hex_roundtrip);
    CHECK_EQ(to_hex(from_hex("5a a5 1a")), std::string("5A A5 1A"));
    CHECK_EQ(to_hex(from_hex("5aa51a")), std::string("5A A5 1A"));
    CHECK_THROWS(from_hex("5A A5 1"));   // 奇数位
    CHECK_THROWS(from_hex("5A ZZ"));     // 非法字符
}

// -- modbus_rtu ---------------------------------------------------------------
{
    CASE(crc_known_vector);
    // 01 04 FF FF -> CRC 0xA941 (线上低字节先发: 41 A9)
    const std::uint8_t probe[] = {0x01, 0x04, 0xFF, 0xFF};
    const std::uint16_t crc = modbus::crc16(probe, sizeof(probe));
    CHECK_EQ(int(crc & 0xFF), 0x41);
    CHECK_EQ(int(crc >> 8), 0xA9);
}

{
    CASE(unicast_address_rule);
    CHECK(!modbus::is_unicast_address(0));
    CHECK(modbus::is_unicast_address(1));
    CHECK(modbus::is_unicast_address(247));
    CHECK(!modbus::is_unicast_address(248));
    CHECK_THROWS(modbus::make_request(0, 0x03, 0, 1));  // 广播被拒
}

{
    CASE(make_request_known_frame);
    CHECK_EQ(to_hex(modbus::make_request(1, 0x03, 0x0000, 0x000A)), std::string("01 03 00 00 00 0A C5 CD"));
}

{
    CASE(locate_reply_accepts_normal);
    auto found = modbus::locate_reply(kReadReply2, 1, 0x03, kReadRequest10, 7,
        [](const std::uint8_t* p) { return p[2] == 0x02; });
    CHECK(found.ok());
    CHECK(!found.value().exception);
    CHECK_EQ(found.value().offset, size_t(0));
    CHECK_EQ(to_hex(found.value().bytes), std::string("01 03 02 00 2A 39 9B"));
}

{
    CASE(locate_reply_rejects_echo_only);
    // 适配器回送的请求帧不能被当成响应。
    auto found = modbus::locate_reply(kReadRequest10, 1, 0x03, kReadRequest10, 7,
        [](const std::uint8_t*) { return false; });
    CHECK(!found.ok());
    CHECK_EQ(found.error().code, std::string("no_matching_reply"));
    CHECK(found.error().kind == ErrorKind::protocol);
}

{
    CASE(locate_reply_reports_exception);
    auto found = modbus::locate_reply(kException, 1, 0x03, kReadRequest10, 7,
        [](const std::uint8_t*) { return false; });
    CHECK(found.ok());
    CHECK(found.value().exception);
    CHECK_EQ(int(found.value().exception_code), 0x02);
}

{
    CASE(locate_reply_rejects_ambiguous);
    // 两份 CRC 都对的响应 -> 无法判断哪份回答本次请求。
    const Bytes twice = concat({kReadReply2, kReadReply2});
    auto found = modbus::locate_reply(twice, 1, 0x03, kReadRequest10, 7,
        [](const std::uint8_t* p) { return p[2] == 0x02; });
    CHECK(!found.ok());
    CHECK_EQ(found.error().code, std::string("ambiguous_reply"));
}

{
    CASE(locate_reply_finds_after_noise);
    const Bytes noisy = concat({from_hex("AA BB"), kReadReply2, from_hex("CC")});
    auto found = modbus::locate_reply(noisy, 1, 0x03, kReadRequest10, 7,
        [](const std::uint8_t* p) { return p[2] == 0x02; });
    CHECK(found.ok());
    CHECK_EQ(found.value().offset, size_t(2));
}

{
    CASE(locate_reply_rejects_crc_error);
    Bytes broken = kReadReply2;
    broken[6] = static_cast<std::uint8_t>(broken[6] ^ 0xFF);
    auto found = modbus::locate_reply(broken, 1, 0x03, kReadRequest10, 7,
        [](const std::uint8_t* p) { return p[2] == 0x02; });
    CHECK(!found.ok());
    CHECK_EQ(found.error().code, std::string("no_matching_reply"));
}

// -- laser -------------------------------------------------------------------
{
    CASE(laser_read_command_filter);
    CHECK(laser::is_read_command(0xB1));
    CHECK(laser::is_read_command(0xB2));
    CHECK(laser::is_read_command(0xBC));
    CHECK(!laser::is_read_command(0x11));
}

{
    CASE(laser_make_read_request_shape);
    const Bytes req = laser::make_read_request(0xB2);
    CHECK_EQ(req.size(), size_t(7));
    CHECK_EQ(int(req[0]), 0x5A);
    CHECK_EQ(int(req[1]), 0xA5);
    CHECK_EQ(int(req[2]), 0x04);
    CHECK_EQ(int(req[3]), 0x00);
    CHECK_EQ(int(req[4]), 0xB2);
    CHECK_THROWS(laser::make_read_request(0x11));
}

{
    CASE(laser_make_write_frame_has_no_policy);
    // 重构后的关键点：写指令只做长度检查，任何指令码都放行。
    for (std::uint8_t cmd : {0x11, 0x21, 0x24, 0x31, 0x33, 0x34, 0xFF}) {
        const Bytes frame = laser::make_write_frame(cmd, {0x01});
        CHECK_EQ(frame.size(), size_t(8));
        CHECK_EQ(int(frame[4]), int(cmd));
    }
    CHECK_THROWS(laser::make_write_frame(0x11, Bytes(252, 0x00)));  // 超过 251
}

{
    CASE(laser_decode_roundtrip);
    const Bytes frame = make_b2_frame();
    auto decoded = laser::decode_frame(frame, 0xB2);
    CHECK(decoded.ok());
    CHECK_EQ(decoded.value().kind, std::string("ld_actual"));
    CHECK_EQ(int(decoded.value().command), 0xB2);
    CHECK(decoded.value().fields.size() > 10);
    bool found_work_state = false;
    for (const auto& f : decoded.value().fields)
        if (f.name == "work_state") { found_work_state = true; CHECK(f.scaled); CHECK_EQ(int(f.raw), 1); }
    CHECK(found_work_state);
}

{
    // 放电计数器是协议里唯一的 32 位字段（0xB2 data[13..16]，高位在前）。
    // 它曾经被 static_cast<uint16_t> 截断过——计数器一旦过 65535 就回绕，
    // 表现为"出光确认说没出光"，而且极难查。这里钉死无损。
    CASE(laser_discharge_counter_is_full_32bit);
    Bytes payload(22, 0);
    payload[13] = 0x12; payload[14] = 0x34; payload[15] = 0x56; payload[16] = 0x78;
    auto decoded = laser::decode_frame(laser::make_frame(0xB2, payload), 0xB2);
    CHECK(decoded.ok());
    bool found = false;
    for (const auto& f : decoded.value().fields) {
        if (f.name != "discharge_counter") continue;
        found = true;
        CHECK(f.wide);                       // 标记为 32 位字段
        CHECK_EQ(f.raw32, 0x12345678u);      // 无损
    }
    CHECK(found);
}

{
    CASE(laser_decode_rejects_command_mismatch);
    auto decoded = laser::decode_frame(make_b2_frame(), 0xB1);
    CHECK(!decoded.ok());
    CHECK_EQ(decoded.error().code, std::string("unexpected_command"));
}

{
    CASE(laser_decode_rejects_crc_error);
    Bytes frame = make_b2_frame();
    frame[frame.size() - 1] = static_cast<std::uint8_t>(frame[frame.size() - 1] ^ 0xFF);
    auto decoded = laser::decode_frame(frame, 0xB2);
    CHECK(!decoded.ok());
    CHECK(decoded.error().kind == ErrorKind::protocol);
}

{
    CASE(laser_decode_rejects_truncated);
    const Bytes frame = make_b2_frame();
    auto decoded = laser::decode_frame(Bytes(frame.begin(), frame.end() - 3), 0xB2);
    CHECK(!decoded.ok());
    CHECK_EQ(decoded.error().code, std::string("bad_length"));
}

{
    CASE(laser_find_frame_in_noise);
    const Bytes frame = make_b2_frame();
    const Bytes stream = concat({from_hex("11 22"), frame, from_hex("99")});
    auto scan = laser::find_frame(stream);
    CHECK(scan.status == laser::FrameStatus::complete);
    CHECK_EQ(scan.offset, size_t(2));
    CHECK_EQ(scan.length, frame.size());
}

{
    CASE(laser_find_frame_partial);
    const Bytes frame = make_b2_frame();
    const Bytes partial(frame.begin(), frame.end() - 4);
    auto scan = laser::find_frame(partial);
    CHECK(scan.status == laser::FrameStatus::partial);
}

{
    CASE(laser_find_frame_invalid_on_crc);
    Bytes frame = make_b2_frame();
    frame[frame.size() - 1] = static_cast<std::uint8_t>(frame[frame.size() - 1] ^ 0xFF);
    auto scan = laser::find_frame(frame);
    CHECK(scan.status == laser::FrameStatus::invalid);
}

{
    CASE(laser_find_frame_none);
    auto scan = laser::find_frame(from_hex("DE AD BE EF"));
    CHECK(scan.status == laser::FrameStatus::invalid);
}

// -- rangefinder -------------------------------------------------------------
{
    CASE(rangefinder_register_descriptions);
    auto r = rangefinder::describe_register(0x0204, 90);
    CHECK_EQ(r.name, std::string("confidence"));
    CHECK(r.valid);
    CHECK(!rangefinder::describe_register(0x0204, 200).valid);
    CHECK(!rangefinder::describe_register(0x0200, 0).valid);   // raw==0 哨兵
    CHECK(!rangefinder::describe_register(0x0201, 0xFFFF).valid);
    // 未登记的地址不做有效性判断，valid 保持默认 true
    CHECK(rangefinder::describe_register(0x0300, 1).valid);
}

{
    CASE(rangefinder_request_validation);
    CHECK_THROWS(rangefinder::make_read_request(1, 0x0200, 0));
    CHECK_THROWS(rangefinder::make_read_request(1, 0x0200, 126));
    CHECK_THROWS(rangefinder::make_read_request(0, 0x0200, 1));
    CHECK_THROWS(rangefinder::make_read_request(1, 0xFFFF, 17));  // 越界回绕
}

{
    CASE(rangefinder_decode_read_response);
    Bytes reply{0x01, 0x03, 0x06};
    const std::uint16_t values[] = {10, 0xFFFF, 1234};
    for (const auto v : values) {
        reply.push_back(static_cast<std::uint8_t>(v >> 8));
        reply.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    const std::uint16_t crc = modbus::crc16(reply.data(), reply.size());
    reply.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    reply.push_back(static_cast<std::uint8_t>(crc >> 8));

    auto decoded = rangefinder::decode_read_response(reply, 1, 0x0200, 3);
    CHECK(decoded.ok());
    CHECK_EQ(decoded.value().registers.size(), size_t(3));
    CHECK_EQ(int(decoded.value().registers[0].raw), 10);
    CHECK(decoded.value().registers[0].valid);
    CHECK(!decoded.value().registers[1].valid);   // 0xFFFF 哨兵
    CHECK_EQ(int(decoded.value().registers[2].raw), 1234);
}

{
    CASE(rangefinder_surfaces_modbus_exception);
    auto decoded = rangefinder::decode_read_response(kException, 1, 0x0200, 3);
    CHECK(!decoded.ok());
    CHECK(decoded.error().kind == ErrorKind::device);
    CHECK_EQ(decoded.error().code, std::string("modbus_exception"));
}

// -- indicator ---------------------------------------------------------------
{
    CASE(indicator_parse_roundtrip);
    auto ch = indicator::parse_channel("green");
    CHECK(ch.ok());
    CHECK(ch.value() == indicator::Channel::green);
    CHECK(!indicator::parse_channel("purple").ok());
    auto st = indicator::parse_state("blink-2hz");
    CHECK(st.ok());
    CHECK(st.value() == indicator::State::blink_2hz);
    CHECK(!indicator::parse_state("strobe").ok());
}

{
    CASE(indicator_model_lookup);
    indicator::ModelTraits traits;
    CHECK(indicator::lookup_model("CL50S+K50-485", traits));
    CHECK(traits.k50);
    CHECK(!indicator::lookup_model("MADE-UP-9000", traits));
}

{
    CASE(indicator_k50_yellow_green_swapped);
    // K50: 绿=2, 黄=3；普通灯: 黄=2, 绿=3。
    const indicator::WriteRequest k50_green{"CL50S+K50-485", indicator::Channel::green, indicator::State::on};
    const indicator::WriteRequest cl50_green{"TL50-485", indicator::Channel::green, indicator::State::on};
    const indicator::WriteRequest k50_yellow{"CL50S+K50-485", indicator::Channel::yellow, indicator::State::on};
    const indicator::WriteRequest cl50_yellow{"TL50-485", indicator::Channel::yellow, indicator::State::on};
    auto a = indicator::make_write_request(1, k50_green);
    auto b = indicator::make_write_request(1, cl50_green);
    auto c = indicator::make_write_request(1, k50_yellow);
    auto d = indicator::make_write_request(1, cl50_yellow);
    CHECK(a.ok() && b.ok() && c.ok() && d.ok());
    CHECK_EQ(int(a.value()[3]), 2);   // K50 绿 -> 线圈 2
    CHECK_EQ(int(d.value()[3]), 2);   // 普通黄 -> 线圈 2
    CHECK_EQ(int(c.value()[3]), 3);   // K50 黄 -> 线圈 3
    CHECK_EQ(int(b.value()[3]), 3);   // 普通绿 -> 线圈 3
}

{
    CASE(indicator_blink_coils);
    const indicator::WriteRequest on{"TL50-485", indicator::Channel::red, indicator::State::on};
    const indicator::WriteRequest slow{"TL50-485", indicator::Channel::red, indicator::State::blink_1hz};
    const indicator::WriteRequest fast{"TL50-485", indicator::Channel::red, indicator::State::blink_2hz};
    const indicator::WriteRequest off{"TL50-485", indicator::Channel::red, indicator::State::off};
    auto a = indicator::make_write_request(1, on);
    auto b = indicator::make_write_request(1, slow);
    auto c = indicator::make_write_request(1, fast);
    auto d = indicator::make_write_request(1, off);
    CHECK(a.ok() && b.ok() && c.ok() && d.ok());
    CHECK_EQ(int(a.value()[3]), 0x01);   // 红常亮
    CHECK_EQ(int(b.value()[3]), 0x06);   // 红 1 Hz
    CHECK_EQ(int(c.value()[3]), 0x16);   // 红 2 Hz
    CHECK_EQ(int(d.value()[3]), 0x01);   // 单通道熄灭复用同一线圈 + 值 0
    CHECK_EQ(int(d.value()[4]), 0x00);
    CHECK_EQ(int(d.value()[5]), 0x00);
}

{
    CASE(indicator_query_request);
    auto req = indicator::make_query_request(1, indicator::Channel::red);
    CHECK_EQ(req.size(), size_t(8));
    CHECK_EQ(int(req[1]), 0x01);   // 功能码 0x01 读离散输入
    CHECK_EQ(int(indicator::query_register(indicator::Channel::buzzer)), 6);
}

{
    CASE(indicator_rejects_unsupported);
    // 未登记型号 -> 不生成任何字节
    const indicator::WriteRequest bad{"NO-SUCH-MODEL", indicator::Channel::red, indicator::State::on};
    auto r = indicator::make_write_request(1, bad);
    CHECK(!r.ok());
    CHECK(r.error().kind == ErrorKind::unsupported);

    // RL30-24V 没有蜂鸣器命令
    const indicator::WriteRequest buzzer{"RL30-24V", indicator::Channel::buzzer, indicator::State::on};
    auto r2 = indicator::make_write_request(1, buzzer);
    CHECK(!r2.ok());
    CHECK_EQ(r2.error().code, std::string("model_has_no_buzzer"));
}

TEST_MAIN_RETURN()


