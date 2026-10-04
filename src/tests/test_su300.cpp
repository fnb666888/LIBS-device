// SU300 协议/驱动离线测试：不打开真实 COM 口，不向变频器写入任何数据。
#include <deque>
#include <string>
#include <vector>

#include "check.hpp"
#include "libs/devices/su300.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/su300.hpp"

using libs::Bytes;

namespace {

class FakeLink final : public libs::transport::Link {
public:
    std::vector<Bytes> sent;
    std::deque<libs::Result<libs::transport::Reply>> replies;

    libs::Result<libs::transport::Reply> exchange(const Bytes& request,
                                                   unsigned, unsigned) override {
        sent.push_back(request);
        if (replies.empty()) {
            libs::transport::Reply r;
            r.bytes = request;
            r.elapsed_ms = 1;
            return r;
        }
        auto r = replies.front();
        replies.pop_front();
        return r;
    }

    std::string endpoint() const override { return "FAKE-SU300"; }

    void queue(Bytes bytes) {
        libs::transport::Reply r;
        r.bytes = std::move(bytes);
        r.elapsed_ms = 1;
        replies.push_back(libs::Result<libs::transport::Reply>::success(std::move(r)));
    }
};

Bytes read_reply(std::uint8_t address, std::uint16_t start,
                 const std::vector<std::uint16_t>& values) {
    Bytes frame{address, 0x03, static_cast<std::uint8_t>(values.size() * 2)};
    for (const auto value : values) {
        frame.push_back(static_cast<std::uint8_t>(value >> 8));
        frame.push_back(static_cast<std::uint8_t>(value));
    }
    const auto crc = libs::modbus::crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    (void)start;
    return frame;
}

}  // namespace

TEST_MAIN_BEGIN()

{
    CASE(protocol_builds_manual_commands);
    CHECK_EQ(libs::to_hex(libs::su300::make_write_request(1, 0x2000, 0x0012)),
             std::string("01 06 20 00 00 12 02 07"));
    CHECK_EQ(libs::to_hex(libs::su300::make_run_command_request(1, libs::su300::RunCommand::reverse_jog)),
             std::string("01 06 20 00 00 23 C3 D3"));
    CHECK_EQ(libs::to_hex(libs::su300::make_frequency_request(1, 5000)),
             std::string("01 06 20 01 13 88 DE 9C"));
    CHECK_EQ(libs::to_hex(libs::su300::make_frequency_request(1, -1250)),
             std::string("01 06 20 01 FB 1E 10 F2"));
    CHECK_THROWS(libs::su300::make_frequency_request(1, 10001));
}

{
    CASE(protocol_decodes_write_echo_and_status_blocks);
    const Bytes write = libs::su300::make_write_request(1, 0x2000, 0x0001);
    CHECK(libs::su300::decode_write_response(write, 1, 0x2000, 0x0001).ok());
    libs::su300::RegisterBlock block;
    block.address = 1;
    block.start = 0x2100;
    block.count = 2;
    block.values = {0, 1};
    auto status = libs::su300::decode_status(std::vector<libs::su300::RegisterBlock>{block});
    CHECK(!status.ok());
}

{
    CASE(device_writes_frequency_then_run_and_reads_four_status_ranges);
    FakeLink link;
    link.queue(libs::su300::make_frequency_request(1, 2500));
    link.queue(libs::su300::make_run_command_request(1, libs::su300::RunCommand::forward));
    link.queue(read_reply(1, 0x2100, {0, 1, 500, 500, 10, 220, 200, 0}));
    link.queue(read_reply(1, 0x2109, {0, 1500}));
    link.queue(read_reply(1, 0x210D, {250, 0, 0}));
    link.queue(read_reply(1, 0x2111, {0, 0, 0, 0, 1, 1, 0}));

    libs::devices::Su300 motor(link, 1);
    const auto set = motor.set_frequency_percent(2500);
    CHECK(set.ok());
    CHECK(set.value().echo_confirmed);
    const auto run = motor.run(libs::su300::RunCommand::forward);
    CHECK(run.ok());
    CHECK(run.value().echo_confirmed);
    const auto status = motor.read_status();
    CHECK(status.ok());
    CHECK_EQ(link.sent.size(), size_t(6));
    CHECK_EQ(libs::to_hex(link.sent[2]), std::string("01 03 21 00 00 08 4E 30"));
    CHECK_EQ(libs::to_hex(link.sent[3]), std::string("01 03 21 09 00 02 1E 35"));
    CHECK_EQ(libs::to_hex(link.sent[4]), std::string("01 03 21 0D 00 03 9E 34"));
    CHECK_EQ(libs::to_hex(link.sent[5]), std::string("01 03 21 11 00 07 5E 31"));
    CHECK_EQ(status.value().fault, std::uint16_t(0));
    CHECK(status.value().running());
    CHECK_EQ(status.value().direction, std::uint16_t(0));
    CHECK_EQ(status.value().set_frequency_hz(), 5.0);
    CHECK_EQ(status.value().current_fault, std::uint16_t(0));
    CHECK_EQ(status.value().raw_blocks.size(), size_t(4));
}

{
    CASE(device_rejects_non_echo_write_response);
    FakeLink link;
    // Valid FC06 response, but it acknowledges 2000H=0001 instead of the
    // requested forward-run value 0012. The driver must not report success.
    link.queue(libs::su300::make_write_request(1, 0x2000, 0x0001));
    libs::devices::Su300 motor(link, 1);
    CHECK(!motor.run(libs::su300::RunCommand::forward).ok());
}

TEST_MAIN_RETURN()
