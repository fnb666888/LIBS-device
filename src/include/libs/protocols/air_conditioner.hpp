#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"
#include "libs/protocols/modbus_rtu.hpp"

namespace libs::air_conditioner {

// 空调协议 V2：Modbus RTU 变体，纯编解码，无 I/O。
//
// 手册里寄存器号是"文档地址"（10001/30001/40001 这样的 5 位编号），
// 但报文里出现的是从 0 开始的偏移量：
//   功能码 0x01 读状态：sReg = 文档地址 - 10001
//   功能码 0x03 读参数：sReg = 文档地址 - 30001
//   功能码 0x05 写状态：手册写 - 00001，实际实测是 - 10001（见下）
//   功能码 0x06 写参数：sReg = 文档地址 - 40001
// 本层 API 一律用"文档地址"，偏移换算在内部完成，避免调用方记错基准。
//
// 厂商文档只有一处需要按实测纠正：§4.5 注 1 说 FC05 的 sReg = 寄存器号 - 00001，
// 但厂商自己的样本 §7.11/§7.12 写开关机用的偏移是 0x0029 = 41 = 10042 - 10001。
// 按 00001 换算会得到 10041，设备不会认。本实现以实测样本为准。
//
// 其余部分（§7.8/§7.9/§7.10 散文的位序说明、FC05 的 0x0000/0xFF00 取值）与
// 寄存器表完全自洽，已逐条核对过，不需要任何"纠偏"。
//
// 空调连续 100 s 收不到模式控制命令会自动切回自动运行——这是设备行为，
// 本层不实现任何"保持"逻辑，调用方自己决定要不要周期下发。

using Bytes = libs::Bytes;

// 文档地址。10001-10058 状态位，30001-30035 运行参数。
namespace reg {
constexpr std::uint16_t kStatusFirst  = 10001;   // 10001-10034 保留
constexpr std::uint16_t kCoolingOn    = 10035;   // 制冷状态
constexpr std::uint16_t kHeaterOn     = 10036;   // 加热器状态
constexpr std::uint16_t kIndoorFan    = 10037;   // 内风机状态
constexpr std::uint16_t kOutdoorFan   = 10038;   // 外风机状态
constexpr std::uint16_t kPowerState   = 10042;   // 监控开关机（0 关 / 1 开）
constexpr std::uint16_t kIndoorSensorFault    = 10043;
constexpr std::uint16_t kEvaporatorSensorFault = 10044;
constexpr std::uint16_t kCondenserSensorFault  = 10045;
constexpr std::uint16_t kHumiditySensorFault   = 10047;
constexpr std::uint16_t kHumidityHighAlarm    = 10048;
constexpr std::uint16_t kCondenserHighAlarm   = 10051;
constexpr std::uint16_t kEvaporatorFreezeAlarm = 10052;
constexpr std::uint16_t kCoolingAlarm  = 10053;  // 压缩机 & 制冷系统
constexpr std::uint16_t kHighTempAlarm = 10054;
constexpr std::uint16_t kLowTempAlarm  = 10055;
constexpr std::uint16_t kStatusLast   = 10058;

constexpr std::uint16_t kIndoorTemp      = 30001;  // degC x10，回风温度
constexpr std::uint16_t kEvaporatorTemp = 30002;  // degC x10
constexpr std::uint16_t kCondenserTemp  = 30003;  // degC x10
constexpr std::uint16_t kHumidity       = 30009;  // %RH x10
constexpr std::uint16_t kCoolingOnSetpoint  = 30014;  // degC x10, 20.0~55.0
constexpr std::uint16_t kCoolingOffSetpoint = 30015;  // degC x10, 15~35
constexpr std::uint16_t kHeatingOnSetpoint  = 30016;  // degC x10, -30~15
constexpr std::uint16_t kHeatingOffSetpoint = 30017;  // degC x10, 0~23
constexpr std::uint16_t kIndoorHighAlarmSetpoint = 30018;  // degC x10, 30~60
constexpr std::uint16_t kIndoorLowAlarmSetpoint  = 30019;  // degC x10, -10~10
constexpr std::uint16_t kCondenserHighProtect   = 30020;  // degC x10
constexpr std::uint16_t kEvaporatorFreezeProtect = 30021;  // degC x10
constexpr std::uint16_t kDehumidifyOnHumidity  = 30022;  // %RH x10
constexpr std::uint16_t kDehumidifyOffHumidity = 30023;  // %RH x10
constexpr std::uint16_t kHumidityAlarmLimit    = 30024;  // %RH x10
constexpr std::uint16_t kHumidityCalibration   = 30025;  // x1，可为负
constexpr std::uint16_t kMode                 = 30026;  // 0 待机 1 制冷 2 加热 3 除湿
constexpr std::uint16_t kBaudRate             = 30027;  // 0=9600 1=19200 2=38400
constexpr std::uint16_t kSlaveAddress         = 30028;  // 1~253
constexpr std::uint16_t kParamLast           = 30035;
}  // namespace reg

// 功能码 0x05 写状态位的取值。手册正文写 "0x0000 关 / 0xFF00 开"，
// 与 §7.11/§7.12 的实测样本一致，也符合 Modbus FC05 惯例。
namespace coil {
constexpr std::uint16_t kOff = 0x0000;
constexpr std::uint16_t kOn  = 0xFF00;
}  // namespace coil

enum class Mode : std::uint16_t { standby = 0, cooling = 1, heating = 2, dehumidify = 3 };
const char* to_string(Mode mode) noexcept;

// 状态位：每一位一个开关/告警。0 关，1 开/告警。
struct StatusBit {
    std::uint16_t address = 0;   // 文档地址，如 10035
    bool active = false;
    std::string name;
    bool is_alarm = false;       // true = 告警位，不是运行位
};

struct StatusReading {
    std::uint8_t address = 0;
    std::uint16_t start = 0;     // 文档地址起点
    std::uint16_t count = 0;
    std::vector<StatusBit> bits;              // 只含手册登记过的位
    std::vector<std::uint8_t> raw_bytes;     // 原始位图，保留未登记的位
};

// 功能码 0x03 读运行参数。
struct ParamRegister {
    std::uint16_t address = 0;   // 文档地址，如 30001
    std::uint16_t raw = 0;       // 原始 16 位，高字节在前
    std::string name;
    std::string unit;
    double scale = 1.0;          // 工程量 = raw * scale
    double value = 0.0;          // 按手册变比换算，温度/湿度可带符号
    bool documented = true;      // false = 手册里的保留位
    bool is_signed = false;     // true = raw 按 16 位补码解释（温度、湿度、湿度校正值）
    bool writable = false;       // 该参数同时出现在 4xxxx 空间，可 FC06 写
};

struct ParameterReading {
    std::uint8_t address = 0;
    std::uint16_t start = 0;
    std::uint16_t count = 0;
    std::vector<ParamRegister> registers;
};

// 功能码 0x01：读运行状态位。手册 3.2 限制单次最多 50 个寄存器。
Bytes make_status_request(std::uint8_t address, std::uint16_t start = reg::kStatusFirst,
                           std::uint16_t count = 24);

// 功能码 0x03：读运行参数。手册 3.2 明确超过 50 只返回前 50 个。
Bytes make_param_request(std::uint8_t address, std::uint16_t start = reg::kIndoorTemp,
                         std::uint16_t count = 25);

// 功能码 0x05：写一个状态位（开关机）。
Bytes make_control_request(std::uint8_t address, std::uint16_t reg, bool on);

// 功能码 0x06：写一个运行参数。range_min / range_max 是手册给的可写范围，
// 两者都为 0 表示手册没给范围，不做检查。
struct ParamWrite {
    std::uint16_t reg = 0;
    std::uint16_t raw = 0;
    std::uint16_t range_min = 0;
    std::uint16_t range_max = 0;
};
Result<Bytes> make_param_write_request(std::uint8_t address, const ParamWrite& write);

struct ControlResponse {
    std::uint16_t reg = 0;      // 文档地址
    std::uint16_t value = 0;    // 回送值
    bool on = false;
};
Result<ControlResponse> decode_control_response(const Bytes& capture, std::uint8_t address,
                                               std::uint16_t reg, bool on);

struct ParamWriteResponse {
    std::uint16_t reg = 0;
    std::uint16_t value = 0;
};
Result<ParamWriteResponse> decode_param_write_response(const Bytes& capture, std::uint8_t address,
                                                       const ParamWrite& write);

Result<StatusReading> decode_status_response(const Bytes& capture, std::uint8_t address,
                                            std::uint16_t start, std::uint16_t count);

Result<ParameterReading> decode_param_response(const Bytes& capture, std::uint8_t address,
                                               std::uint16_t start, std::uint16_t count);

// 查表：某个文档地址在参数表里叫什么、什么单位、什么变比。
ParamRegister describe_param(std::uint16_t address, std::uint16_t raw);

}  // namespace libs::air_conditioner


