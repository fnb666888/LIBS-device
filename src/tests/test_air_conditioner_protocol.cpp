// 空调协议 V2 编解码测试：全部使用厂商手册 §7 的实测样本帧做向量，
// 加上边界与错误路径。纯编解码，不碰串口。
#include <string>
#include <vector>

#include "check.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/air_conditioner.hpp"

using libs::Bytes;
using namespace libs;
using namespace libs::air_conditioner;

TEST_MAIN_BEGIN()

// 手册 §7.1 的 25 寄存器响应。期望值一律用 to_hex 的"大写、逐字节空格分隔"格式。
static const char* kVendorParamFrame =
    "01 03 32 01 36 01 36 01 36 00 00 00 00"
    "00 00 00 00 00 00 01 C3 00 00 00 00 00"
    "00 00 00 01 5E 01 22 00 32 00 96 02 26"
    "00 00 03 34 00 00 03 20 02 EE 03 84 FF"
    "FC EF 7E";

// 手册 §7.8：一次读满 10001..10058（58 个状态位 -> 8 字节位图）。
static const char* kVendorStatusFrame = "08 01 08 00 00 00 00 14 00 00 00 1F 71";

// ── 组帧：手册 §7 的 [发送] 行 ───────────────────────────────────────────────

{
    CASE(status_request_matches_vendor_sample_7_8);
    CHECK_EQ(to_hex(make_status_request(8, 10001, 58)), std::string("08 01 00 00 00 3A BC 80"));
}

{
    CASE(status_request_defaults_read_24_bits);
    CHECK_EQ(make_status_request(1)[0], std::uint8_t(1));
    CHECK_EQ(make_status_request(1)[1], std::uint8_t(0x01));
    CHECK_EQ(make_status_request(1)[4], std::uint8_t(0x00));   // nReg 高字节
    CHECK_EQ(make_status_request(1)[5], std::uint8_t(0x18));   // nReg 低字节 = 24
}

{
    CASE(status_request_rejects_out_of_range_span);
    CHECK_THROWS(make_status_request(1, 10001, 0));
    CHECK_THROWS(make_status_request(1, 10001, 59));   // 越过 10058
    CHECK_THROWS(make_status_request(1, 9900, 4));     // 起点不在 10001..10058
    CHECK_THROWS(make_status_request(0, 10001, 4));     // 广播地址被拒
    CHECK_THROWS(make_status_request(248, 10001, 4));
}

{
    CASE(param_request_matches_vendor_sample_7_1);
    CHECK_EQ(to_hex(make_param_request(1, 30001, 25)), std::string("01 03 00 00 00 19 84 00"));
}

{
    CASE(param_request_matches_vendor_sample_7_3);
    CHECK_EQ(to_hex(make_param_request(8, 30014, 2)), std::string("08 03 00 0D 00 02 55 51"));
}

{
    CASE(param_request_rejects_more_than_50_registers);
    // 手册 3.2：单次超过 50 个只上报前 50 个。这里直接拒绝而不是静默截断。
    CHECK_THROWS(make_param_request(1, 30001, 51));
    CHECK_THROWS(make_param_request(1, 30001, 36));   // 越过 30035
    CHECK_THROWS(make_param_request(1, 30035, 2));
}

{
    CASE(control_request_matches_vendor_samples_7_11_and_7_12);
    CHECK_EQ(to_hex(make_control_request(8, reg::kPowerState, false)), std::string("08 05 00 29 00 00 1C 9B"));
    CHECK_EQ(to_hex(make_control_request(8, reg::kPowerState, true)),  std::string("08 05 00 29 FF 00 5D 6B"));
}

{
    CASE(control_request_uses_the_measured_offset_not_the_doc_formula);
    // 手册 §4.5 注 1 写 "sReg = 寄存器号 - 00001"，那样 10042 会变成 10041 = 0x0028。
    // 但厂商样本 §7.11/§7.12 的偏移是 0x0029 = 41 = 10042 - 10001。
    // 这条用例把实测基准钉死，避免以后有人"照手册修正"回去。
    Bytes frame = make_control_request(8, reg::kPowerState, true);
    CHECK_EQ(frame.size(), size_t(8));
    CHECK_EQ(frame[2], std::uint8_t(0x00));
    CHECK_EQ(frame[3], std::uint8_t(0x29));
    CHECK_EQ(to_hex(frame), std::string("08 05 00 29 FF 00 5D 6B"));
    // 同一条报文必须也能被自己的解码器认回来。
    CHECK(decode_control_response(frame, 8, reg::kPowerState, true).ok());
}
{
    CASE(control_request_only_drive_the_power_register);
    // FC05 在本协议里只登记了开关机；写别的状态位是编解码层就不该生成的帧。
    CHECK_THROWS(make_control_request(1, reg::kCoolingOn, true));
    CHECK_THROWS(make_control_request(1, 10043, true));
}

{
    CASE(param_write_matches_vendor_samples_7_4_to_7_6);
    ParamWrite on;  on.reg = 30014;  on.raw = 0x0168;   // §7.4 制冷开启点 = 36.0
    CHECK_EQ(to_hex(make_param_write_request(8, on).value()), std::string("08 06 00 0D 01 68 18 EE"));

    ParamWrite off; off.reg = 30015; off.raw = 0x012C;   // §7.5 制冷停止点 = 30.0
    CHECK_EQ(to_hex(make_param_write_request(8, off).value()), std::string("08 06 00 0E 01 2C E8 DD"));

    ParamWrite calib; calib.reg = 30025; calib.raw = 2;  // §7.6 湿度校正值 = 2
    CHECK_EQ(to_hex(make_param_write_request(8, calib).value()), std::string("08 06 00 18 00 02 88 95"));
}

{
    CASE(param_write_rejects_undocumented_registers);
    ParamWrite read_only; read_only.reg = 30001; read_only.raw = 100;
    CHECK(!make_param_write_request(8, read_only).ok());     // 温度是只读量

    ParamWrite reserved; reserved.reg = 30004; reserved.raw = 0;
    CHECK(!make_param_write_request(8, reserved).ok());

    ParamWrite unknown; unknown.reg = 40099; unknown.raw = 0;
    CHECK(!make_param_write_request(8, unknown).ok());

    // 广播地址（站号 0）在 RS485 上不回包，本层直接拒绝。
    ParamWrite broadcast; broadcast.reg = 30014; broadcast.raw = 350;
    CHECK(!make_param_write_request(0, broadcast).ok());
}

{
    CASE(param_write_enforces_documented_range);
    ParamWrite ok;  ok.reg = 30014; ok.raw = 0x0168;          // 36.0，在 20~55 内
    CHECK(make_param_write_request(8, ok).ok());

    ParamWrite hot; hot.reg = 30014; hot.raw = 900;          // 90.0，越界
    hot.range_min = 200; hot.range_max = 550;
    CHECK(!make_param_write_request(8, hot).ok());
}

// ── 解码：手册 §7 的 [接收] 行 ───────────────────────────────────────────────

{
    CASE(param_decode_matches_vendor_sample_7_1);
    auto reading = decode_param_response(from_hex(kVendorParamFrame), 1, 30001, 25);
    CHECK(reading.ok());
    const auto& regs = reading.value().registers;
    CHECK_EQ(regs.size(), size_t(25));
    CHECK_EQ(regs[0].name,  std::string("indoor_temp"));
    CHECK_EQ(regs[0].value, 31.0);              // 0x0136 = 310, 变比 10
    CHECK_EQ(regs[0].unit,  std::string("degC"));
    CHECK_EQ(regs[8].name,  std::string("humidity"));
    CHECK_EQ(regs[8].value, 45.1);              // 0x01C3 = 451
    CHECK_EQ(regs[13].name, std::string("cooling_on_setpoint"));
    CHECK_EQ(regs[13].value, 35.0);             // 0x015E
    CHECK_EQ(regs[14].name, std::string("cooling_off_setpoint"));
    CHECK_EQ(regs[14].value, 29.0);             // 0x0122
    CHECK_EQ(regs[17].name, std::string("indoor_high_alarm_setpoint"));
    CHECK_EQ(regs[17].value, 55.0);             // 0x0226
    CHECK_EQ(regs[24].name, std::string("humidity_calibration"));
    CHECK_EQ(regs[24].value, -4.0);             // 0xFFFC 补码 -4, 变比 x1
}

{
    CASE(param_decode_matches_vendor_sample_7_3);
    auto reading = decode_param_response(from_hex("08 03 04 01 5E 01 22 82 94"), 8, 30014, 2);
    CHECK(reading.ok());
    CHECK_EQ(reading.value().registers.size(), size_t(2));
    CHECK_EQ(reading.value().registers[0].value, 35.0);
    CHECK_EQ(reading.value().registers[1].value, 29.0);
}

{
    CASE(status_decode_matches_vendor_sample_7_8);
    auto reading = decode_status_response(from_hex(kVendorStatusFrame), 8, 10001, 58);
    CHECK(reading.ok());
    CHECK_EQ(reading.value().raw_bytes.size(), size_t(8));
    CHECK_EQ(reading.value().bits.size(), size_t(15));   // 手册登记的 15 个状态位

    // 位图第 5 字节 0x14 = 0b00010100 -> bit2 = 10035 制冷, bit4 = 10037 内风机。
    // 与 §7.8 散文 "b2=1代表制冷开启 / b4=1代表内风机开启" 一致；外风机(bit5) 为 0。
    bool cooling = false, indoor = false, outdoor = false, power = false;
    for (const auto& b : reading.value().bits) {
        if (b.address == reg::kCoolingOn)  cooling = b.active;
        if (b.address == reg::kIndoorFan)  indoor  = b.active;
        if (b.address == reg::kOutdoorFan) outdoor = b.active;
        if (b.address == reg::kPowerState) power   = b.active;
    }
    CHECK(cooling);
    CHECK(indoor);
    CHECK(!outdoor);
    CHECK(!power);   // 10042 落在 B5 bit1, 本帧 B5 = 0x00
}

{
    CASE(status_decode_matches_vendor_sample_7_9);
    // §7.9 接收 08 01 08 00 00 00 00 34 18 00 00 94 B6
    // B0 = 0x34 = 0b00110100 -> bit2 制冷 / bit4 内风机 / bit5 外风机（与散文 "b2/b4/b5" 一致）
    // B5 = 0x18 = 0b00011000 -> bit3 蒸发器传感器故障 / bit4 冷凝器传感器故障
    // 散文的 "b1=0 代表空调开机状态" 说的就是 10042 = 第 41 位 = B5 bit1，本帧为 0。
    // 注意该位 0 = 开机（低有效），与 FC05 写线圈 0xFF00 = 开的约定方向相反。
    // 本层只如实上报位值，不做反相；语义由调用方按厂商约定解释。
    auto reading = decode_status_response(
        from_hex("08 01 08 00 00 00 00 34 18 00 00 94 B6"), 8, 10001, 58);
    CHECK(reading.ok());
    bool cooling = false, heater = false, outdoor = false, indoor = false, power = false;
    bool evap_fault = false, cond_fault = false;
    for (const auto& b : reading.value().bits) {
        if (b.address == reg::kCoolingOn)              cooling    = b.active;
        if (b.address == reg::kHeaterOn)              heater     = b.active;
        if (b.address == reg::kOutdoorFan)            outdoor    = b.active;
        if (b.address == reg::kIndoorFan)             indoor     = b.active;
        if (b.address == reg::kPowerState)            power      = b.active;
        if (b.address == reg::kEvaporatorSensorFault) evap_fault = b.active;
        if (b.address == reg::kCondenserSensorFault)  cond_fault = b.active;
    }
    CHECK(cooling);
    CHECK(indoor);
    CHECK(outdoor);
    CHECK(!heater);     // 10036 -> B0 bit3，本帧为 0
    CHECK(evap_fault);
    CHECK(cond_fault);
    CHECK(!power);      // 10042 -> B5 bit1，本帧为 0（厂商散文：0 = 开机）
}
{
    CASE(status_decode_matches_vendor_sample_7_10);
    // §7.10 接收 08 01 03 1D 06 08 AE B7，只请求 10035..10058（3 字节位图），
    // 因此位下标必须相对 10035 算，而不是相对 10001。
    auto reading = decode_status_response(from_hex("08 01 03 1D 06 08 AE B7"), 8, 10035, 24);
    CHECK(reading.ok());
    CHECK_EQ(reading.value().raw_bytes.size(), size_t(3));
    // B0 = 0x1D = 0b00011101 -> bit0 制冷 / bit2 内风机 / bit3 外风机
    // （bit4 = 10039，散文叫"应急风机"，寄存器表标保留，故不进命名位表）
    bool cooling = false, indoor = false, outdoor = false;
    for (const auto& b : reading.value().bits) {
        if (b.address == reg::kCoolingOn)  cooling = b.active;
        if (b.address == reg::kIndoorFan)  indoor  = b.active;
        if (b.address == reg::kOutdoorFan) outdoor = b.active;
    }
    CHECK(cooling);
    CHECK(indoor);
    CHECK(outdoor);
    // B1 = 0x06 -> bit1 = 10044 蒸发器传感器故障 / bit2 = 10045 冷凝器传感器故障，
    // 10043 柜内传感器故障本帧为 0。
    // B2 = 0x08 -> bit3 = 10054 柜内高温告警。
    bool indoor_sensor = false, evap_sensor = false, cond_sensor = false, high_temp = false;
    for (const auto& b : reading.value().bits) {
        if (b.address == reg::kIndoorSensorFault)     indoor_sensor = b.active;
        if (b.address == reg::kEvaporatorSensorFault) evap_sensor   = b.active;
        if (b.address == reg::kCondenserSensorFault)  cond_sensor   = b.active;
        if (b.address == reg::kHighTempAlarm)         high_temp     = b.active;
    }
    CHECK(!indoor_sensor);
    CHECK(evap_sensor);
    CHECK(cond_sensor);
    CHECK(high_temp);
    // 10039（散文称"应急风机"）以及 10001..10034、10046、10049、10050、
    // 10056..10058 等保留位只在 raw_bytes 里保留，不进命名位表。
    CHECK_EQ(reading.value().bits.size(), size_t(15));
}
{
    CASE(status_decode_places_the_power_bit_per_the_register_table);
    // 只置 10042（= 0-based 第 41 位 -> B5 bit1），验证按寄存器表定位该位。
    // 帧尚未含 CRC，所以 CRC 覆盖除最后两字节外的全部内容。
    // 10042 -> 相对 10001 第 41 位 -> 第 6 个数据字节的 bit1
    Bytes frame{8, 0x01, 8, 0, 0, 0, 0, 0, 0x02, 0, 0};
    const auto crc = modbus::crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    CHECK_EQ(to_hex(frame), std::string("08 01 08 00 00 00 00 00 02 00 00 BB 41"));
    auto reading = decode_status_response(frame, 8, 10001, 58);
    CHECK(reading.ok());
    bool found = false;
    for (const auto& b : reading.value().bits)
        if (b.address == reg::kPowerState) { CHECK(b.active); found = true; }
    CHECK(found);
}

{
    CASE(status_decode_rejects_wrong_byte_count);
    Bytes frame{8, 0x01, 7, 0, 0, 0, 0, 0x02, 0, 0};
    const auto crc = modbus::crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    CHECK(!decode_status_response(frame, 8, 10001, 58).ok());
}

{
    CASE(param_decode_surfaces_modbus_exception);
    // §7.7 的样本 08 86 02 13 A3 是 FC06 的异常响应（0x86 = 0x06|0x80），
    // 异常码 2 = ILLEGAL DATA ADDRESS。喂给 FC03 解码器应当找不到匹配响应。
    auto mismatched = decode_param_response(from_hex("08 86 02 13 A3"), 8, 30001, 25);
    CHECK(!mismatched.ok());
    CHECK(mismatched.error().code == "no_matching_reply");

    // 同一台设备用 FC03 报错时是 08 83。
    auto reading = decode_param_response(from_hex("08 83 02 10 F3"), 8, 30001, 25);
    CHECK(!reading.ok());
    CHECK(reading.error().kind == ErrorKind::device);
    CHECK(reading.error().code == "modbus_exception");
    CHECK(!reading.error().raw.empty());
}

{
    CASE(control_decode_matches_vendor_samples);
    auto off = decode_control_response(from_hex("08 05 00 29 00 00 1C 9B"), 8, reg::kPowerState, false);
    CHECK(off.ok());
    CHECK_EQ(off.value().reg, reg::kPowerState);
    CHECK(!off.value().on);

    auto on = decode_control_response(from_hex("08 05 00 29 FF 00 5D 6B"), 8, reg::kPowerState, true);
    CHECK(on.ok());
    CHECK(on.value().on);
    CHECK_EQ(on.value().value, coil::kOn);
}

{
    CASE(control_decode_rejects_a_different_register_echo);
    CHECK(!decode_control_response(from_hex("08 05 00 28 00 00 00 00"), 8, reg::kPowerState, false).ok());
}

{
    CASE(param_write_decode_matches_vendor_sample_7_4);
    ParamWrite w; w.reg = 30014; w.raw = 0x0168;
    auto echoed = decode_param_write_response(from_hex("08 06 00 0D 01 68 18 EE"), 8, w);
    CHECK(echoed.ok());
    CHECK_EQ(echoed.value().reg, std::uint16_t(30014));
    CHECK_EQ(echoed.value().value, std::uint16_t(0x0168));
}

{
    CASE(decoders_ignore_a_leading_request_echo);
    // RS485 转换器常把请求原样送回；拼接在真实响应前面不应影响解码。
    Bytes capture = make_param_request(8, 30014, 2);
    const Bytes tail = from_hex("08 03 04 01 5E 01 22 82 94");
    capture.insert(capture.end(), tail.begin(), tail.end());
    auto reading = decode_param_response(capture, 8, 30014, 2);
    CHECK(reading.ok());
    CHECK_EQ(reading.value().registers[0].value, 35.0);
}

{
    CASE(decoders_reject_a_request_echo_only_capture);
    const Bytes only_echo = make_param_request(8, 30014, 2);
    CHECK(!decode_param_response(only_echo, 8, 30014, 2).ok());
}

{
    CASE(decoders_reject_another_slaves_frame);
    const Bytes capture = from_hex("01 03 04 01 5E 01 22 82 94");   // 站号 1, 不是 8
    CHECK(!decode_param_response(capture, 8, 30014, 2).ok());
}

{
    CASE(param_table_lookup_covers_documented_registers);
    CHECK_EQ(describe_param(30001, 310).value, 31.0);
    CHECK_EQ(describe_param(30026, 2).name,  std::string("mode"));
    CHECK_EQ(describe_param(30027, 1).name,  std::string("baud_rate"));
    CHECK_EQ(describe_param(30028, 8).name,  std::string("slave_address"));
    CHECK(describe_param(30001, 0).writable   == false);
    CHECK(describe_param(30014, 0).writable   == true);
    CHECK(describe_param(30004, 0).documented == false);
    CHECK_EQ(describe_param(30099, 0).name, std::string("unknown_raw"));
}

{
    CASE(mode_names_round_trip);
    CHECK_EQ(std::string(to_string(Mode::standby)),     std::string("standby"));
    CHECK_EQ(std::string(to_string(Mode::cooling)),     std::string("cooling"));
    CHECK_EQ(std::string(to_string(Mode::heating)),     std::string("heating"));
    CHECK_EQ(std::string(to_string(Mode::dehumidify)), std::string("dehumidify"));
}

TEST_MAIN_RETURN()




