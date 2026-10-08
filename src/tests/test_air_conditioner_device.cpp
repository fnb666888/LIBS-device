// 空调设备驱动测试：注入假 transport::Link，不碰真实串口。
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "libs/devices/air_conditioner.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/air_conditioner.hpp"

using libs::Bytes;
using namespace libs;
using namespace libs::air_conditioner;

namespace {

class FakeLink final : public transport::Link {
public:
    std::vector<Bytes> sent;
    std::deque<Result<transport::Reply>> replies;
    bool fail_next = false;

    Result<transport::Reply> exchange(const Bytes& request, unsigned timeout_ms = 650,
                                      unsigned idle_ms = 80) override {
        (void)timeout_ms; (void)idle_ms;
        sent.push_back(request);
        if (fail_next) {
            fail_next = false;
            return Error::make(ErrorKind::transport, "cable_unplugged", "fake transport failure");
        }
        if (!replies.empty()) {
            auto r = replies.front();
            replies.pop_front();
            return r;
        }
        transport::Reply reply;      // 默认回显
        reply.bytes = request;
        reply.elapsed_ms = 1;
        return reply;
    }

    std::string endpoint() const override { return "FAKE12"; }

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

}  // namespace

TEST_MAIN_BEGIN()

{
    CASE(read_status_runs_one_transaction_and_decodes);
    FakeLink link;
    // 默认区间 10035..10058 -> 请求与厂商 §7.10 的请求帧完全一致：
    //   08 01 00 22 00 18 9C 93
    // 响应 B0 = 0x05 = 0b00000101 -> bit0 = 10035 制冷 / bit2 = 10037 内风机。
    link.queue(from_hex("08 01 03 05 00 00 2C D6"));
    devices::AirConditioner ac(link, 8);
    auto s = ac.readStatus();
    CHECK(s.ok());
    CHECK_EQ(link.sent.size(), size_t(1));
    CHECK_EQ(to_hex(link.sent[0]), std::string("08 01 00 22 00 18 9C 93"));
    bool cooling = false, indoor = false;
    for (const auto& b : s.value().bits) {
        if (b.address == reg::kCoolingOn) cooling = b.active;
        if (b.address == reg::kIndoorFan) indoor  = b.active;
    }
    CHECK(cooling);
    CHECK(indoor);
    CHECK_EQ(s.value().address, std::uint8_t(8));
    CHECK_EQ(s.value().start, std::uint16_t(10035));
}

{
    CASE(read_status_accepts_the_full_58_bit_span);
    FakeLink link;
    link.queue(from_hex("08 01 08 00 00 00 00 14 00 00 00 1F 71"));
    devices::AirConditioner ac(link, 8);
    auto s = ac.readStatus(10001, 58);
    CHECK(s.ok());
    CHECK_EQ(link.sent.size(), size_t(1));
    CHECK_EQ(to_hex(link.sent[0]), std::string("08 01 00 00 00 3A BC 80"));
}

{
    CASE(read_params_decodes_engineering_units);
    FakeLink link;
    link.queue(from_hex("08 03 04 01 5E 01 22 82 94"));
    devices::AirConditioner ac(link, 8);
    auto p = ac.readParams(30014, 2);
    CHECK(p.ok());
    CHECK_EQ(p.value().registers[0].value, 35.0);
    CHECK_EQ(p.value().registers[1].value, 29.0);
    CHECK_EQ(link.sent.size(), size_t(1));
}

{
    CASE(read_params_reports_a_bad_span_without_touching_the_link);
    FakeLink link;
    devices::AirConditioner ac(link, 8);
    auto p = ac.readParams(30001, 51);
    CHECK(!p.ok());
    CHECK(p.error().kind == ErrorKind::invalid_argument);
    CHECK_EQ(link.sent.size(), size_t(0));
}

{
    CASE(uart_errors_surface_as_transport_failures);
    FakeLink link;
    link.queue(from_hex("08 03 04 01 5E 01 22 82 94"), /*errors=*/3);
    devices::AirConditioner ac(link, 8);
    auto p = ac.readParams(30014, 2);
    CHECK(!p.ok());
    CHECK(p.error().kind == ErrorKind::transport);
    CHECK(p.error().code == "uart_errors");
    CHECK(!p.error().raw.empty());
}

{
    CASE(transport_failure_propagates_untouched);
    FakeLink link;
    link.fail_next = true;
    devices::AirConditioner ac(link, 8);
    auto p = ac.readParams(30014, 2);
    CHECK(!p.ok());
    CHECK(p.error().code == "cable_unplugged");
}

{
    CASE(modbus_exception_propagates_with_its_code);
    FakeLink link;
    // 异常响应首字节是功能码 | 0x80：读参数用 FC03 -> 0x83。
    // 手册 §7.7 的 08 86 是写参数（FC06）的异常样本。
    link.queue(from_hex("08 83 02 10 F3"));
    devices::AirConditioner ac(link, 8);
    auto p = ac.readParams(30001, 25);
    CHECK(!p.ok());
    CHECK(p.error().kind == ErrorKind::device);
    CHECK(p.error().code == "modbus_exception");
}

{
    CASE(set_power_sends_fc05_and_confirms_the_echo);
    FakeLink link;
    link.queue(from_hex("08 05 00 29 FF 00 5D 6B"));
    devices::AirConditioner ac(link, 8);
    auto w = ac.setPower(true);
    CHECK(w.ok());
    CHECK_EQ(to_hex(link.sent[0]), std::string("08 05 00 29 FF 00 5D 6B"));
    CHECK(w.value().response.on);
    CHECK_EQ(w.value().response.value, coil::kOn);
}

{
    CASE(set_power_off_uses_the_documented_zero_value);
    FakeLink link;
    link.queue(from_hex("08 05 00 29 00 00 1C 9B"));
    devices::AirConditioner ac(link, 8);
    auto w = ac.setPower(false);
    CHECK(w.ok());
    CHECK_EQ(to_hex(link.sent[0]), std::string("08 05 00 29 00 00 1C 9B"));
    CHECK(!w.value().response.on);
}

{
    CASE(set_setpoint_scales_engineering_units_to_raw);
    FakeLink link;
    link.queue(from_hex("08 06 00 0D 01 68 18 EE"));
    devices::AirConditioner ac(link, 8);
    auto w = ac.setSetpoint(reg::kCoolingOnSetpoint, 36.0);
    CHECK(w.ok());
    CHECK_EQ(to_hex(link.sent[0]), std::string("08 06 00 0D 01 68 18 EE"));
    CHECK_EQ(w.value().response.value, std::uint16_t(360));
}

{
    CASE(set_setpoint_rejects_out_of_documented_range_without_io);
    FakeLink link;
    devices::AirConditioner ac(link, 8);
    auto hot = ac.setSetpoint(reg::kCoolingOnSetpoint, 90.0);   // 上限 55.0
    CHECK(!hot.ok());
    CHECK(hot.error().kind == ErrorKind::invalid_argument);
    CHECK(hot.error().code == "out_of_range");
    CHECK_EQ(link.sent.size(), size_t(0));

    auto cold = ac.setSetpoint(reg::kHeatingOnSetpoint, -40.0);  // 下限 -30.0
    CHECK(!cold.ok());
    CHECK_EQ(link.sent.size(), size_t(0));
}

{
    CASE(set_setpoint_accepts_negative_temperatures);
    FakeLink link;
    // 30016 = 30001 + 15 = 0x0F; 写 -10.0 degC = -100 = 0xFF9C
    link.queue(from_hex("08 06 00 0F FF 9C F8 C9"));
    devices::AirConditioner ac(link, 8);
    auto w = ac.setSetpoint(reg::kHeatingOnSetpoint, -10.0);
    CHECK(w.ok());
    CHECK_EQ(to_hex(link.sent[0]).substr(0, 11), std::string("08 06 00 0F"));
    CHECK_EQ(link.sent[0][4], std::uint8_t(0xFF));
    CHECK_EQ(link.sent[0][5], std::uint8_t(0x9C));
}

{
    CASE(set_setpoint_rejects_read_only_and_unknown_registers);
    FakeLink link;
    devices::AirConditioner ac(link, 8);
    auto ro = ac.setSetpoint(reg::kIndoorTemp, 25.0);
    CHECK(!ro.ok());
    CHECK(ro.error().code == "not_writable");
    auto unknown = ac.setSetpoint(40099, 1.0);
    CHECK(!unknown.ok());
    CHECK(unknown.error().code == "unknown_register");
    CHECK_EQ(link.sent.size(), size_t(0));
}

{
    CASE(set_mode_writes_the_mode_register);
    FakeLink link;
    link.queue(from_hex("08 06 00 19 00 01 99 54"));
    devices::AirConditioner ac(link, 8);
    auto w = ac.setMode(Mode::cooling);
    CHECK(w.ok());
    CHECK_EQ(to_hex(link.sent[0]).substr(0, 11), std::string("08 06 00 19"));
    CHECK_EQ(link.sent[0][5], std::uint8_t(0x01));
}

{
    CASE(set_mode_does_not_clamp_out_of_document_values);
    // 库不替设备做枚举收敛：原样下发，是否接受由设备决定（多半回异常码 3）。
    FakeLink link;
    link.queue(from_hex("08 86 03 D2 63"));
    devices::AirConditioner ac(link, 8);
    auto w = ac.setMode(static_cast<Mode>(9));
    CHECK(!w.ok());
    CHECK(w.error().code == "modbus_exception");
    CHECK_EQ(link.sent[0][5], std::uint8_t(9));
}

{
    CASE(driver_never_invents_io_for_a_rejected_write);
    FakeLink link;
    devices::AirConditioner ac(link, 0);
    ParamWrite w; w.reg = reg::kCoolingOnSetpoint; w.raw = 350;
    auto r = ac.writeParam(w);   // 广播地址：协议层直接拒绝
    CHECK(!r.ok());
    CHECK_EQ(link.sent.size(), size_t(0));
}

{
    CASE(timeout_is_reported_alongside_partial_results);
    FakeLink link;
    link.queue(from_hex("08 03 04 01 5E 01 22 82 94"), 0, /*timed_out=*/true);
    devices::AirConditioner ac(link, 8);
    auto p = ac.readParams(30014, 2);
    // 帧完整时即使 timed_out 标志为真也照常解码，标志本身交给调用方判断。
    CHECK(p.ok());
}

TEST_MAIN_RETURN()

