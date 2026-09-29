#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"

namespace libs::laser {

// 风冷激光器 RS-232 协议。
//
// 本文件只做一件事：字节的编码与解码。它不打开串口、不判断设备状态、
// 不决定"这条写指令是否应该发"。调用方拿到 Bytes 之后要不要发、什么时候发，
// 属于业务/安全层的决定，本层一概不参与。

using Bytes = libs::Bytes;

// 指令码。命名来自《风冷激光器 RS-232 通讯协议整理》。
namespace command {
constexpr std::uint8_t kDischargeCurrent = 0x11;
constexpr std::uint8_t kDischargePulseWidth = 0x12;
constexpr std::uint8_t kDischargeFrequency = 0x13;
constexpr std::uint8_t kDischargeDelay = 0x14;
constexpr std::uint8_t kLdTemperatureLimit = 0x15;
constexpr std::uint8_t kQDelay = 0x16;
constexpr std::uint8_t kVoltageLimit = 0x17;
constexpr std::uint8_t kQDivider = 0x18;
constexpr std::uint8_t kQWarmup = 0x19;
constexpr std::uint8_t kSetAddress = 0x20;   // 协议标注 NC
constexpr std::uint8_t kWorkState = 0x21;    // 0=不工作, 1=工作
constexpr std::uint8_t kClockMode = 0x22;     // 0/1/2
constexpr std::uint8_t kDischargeClock = 0x23;// 0=暂停, 1=恢复
constexpr std::uint8_t kQGate = 0x24;         // 0=光闸关, 1=光闸开
constexpr std::uint8_t kSaveToEeprom = 0x31;
constexpr std::uint8_t kClearCounter = 0x32;
constexpr std::uint8_t kResetFault = 0x33;
constexpr std::uint8_t kSingleTrigger = 0x34;
constexpr std::uint8_t kLdSettings = 0xB1;
constexpr std::uint8_t kLdActual = 0xB2;
constexpr std::uint8_t kTec = 0xBC;
}  // namespace command

constexpr std::uint8_t kReadCommands[] = {0xB1, 0xB2, 0xBC};

bool is_read_command(std::uint8_t command) noexcept;

std::uint16_t crc16(const std::uint8_t* bytes, std::size_t size);

// 组帧：5A A5 <len> 00 <cmd> <data...> <crc_lo> <crc_hi>
// CRC 覆盖从地址字节开始到数据末尾（不含 CRC 自身）。
// payload 超过 251 字节抛 std::invalid_argument（长度字段只有 1 字节）。
Bytes make_frame(std::uint8_t command, const Bytes& payload = {});

// 只读请求。command 不是 B1/B2/BC 时抛 std::invalid_argument。
Bytes make_read_request(std::uint8_t command);

// 写请求的裸组帧：只检查长度，不判断这条指令是否允许发。
// 需要限制时由调用方自己做，不要指望本函数替你把关。
Bytes make_write_frame(std::uint8_t command, const Bytes& payload);

// 一条解码后的字段。每个字段都带 raw；scale_* 为空表示协议没有给出换算。
struct Field {
    std::uint16_t raw = 0;
    std::string name;
    std::string unit;         // 协议标注的单位，可能为空
    double scale = 1.0;       // raw * scale 得到 value
    bool scaled = true;       // false 表示协议未确认换算，value 不应直接使用
    // raw32 保存协议里的完整 32 位值。放这个字段的原因：放电计数器是 4 字节，
    // 塞进 uint16 会被静默截断（曾经发生过），raw32 是唯一无损的读法。
    // wide=false 时 raw32 与 raw 相等；wide=true 时必须读 raw32。
    std::uint32_t raw32 = 0;
    bool wide = false;        // true 表示本字段用 raw32 而非 raw
};

enum class Command : std::uint8_t {
    settings = 0xB1,
    actual = 0xB2,
    tec = 0xBC,
};

// 一次成功解码的响应。
struct Frame {
    Command command = Command::actual;
    Bytes payload;            // 不含帧头帧尾的原始数据段
    std::string kind;         // "ld_settings" / "ld_actual" / "tec"
    std::vector<Field> fields;
    std::string notes;        // 协议文本里标注的 NC / 语义不明之处
};

// 解码一条完整的响应帧。
//
// 严格校验：帧头、长度字段必须与实际字节数完全一致、地址为 00、
// 命令码与预期一致、CRC 正确、读响应负载长度固定（B1=18, B2=22, BC=20）。
// 任何不符都返回错误，不做"尽力解析"，也不把回显当响应。
//
// 注意本函数只接受"刚好一帧"。字节流里有噪声时，请用 laser::find_frames
// 先切分，或者直接用设备驱动里的 transact()。
Result<Frame> decode_frame(const Bytes& bytes, std::uint8_t expected_command);

enum class FrameStatus { complete, partial, invalid };

struct FrameScan {
    FrameStatus status = FrameStatus::invalid;
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string reason;       // status != complete 时说明原因
};

// 在可能含噪声的字节流里扫描激光帧。
//
// 用于"一次串口事务可能带回显、粘包或多帧"的场景：
//   * complete —— 从 offset 开始是一帧完整且自洽的帧；
//   * partial  —— 帧头匹配但数据还没收全，可以继续等；
//   * invalid  —— 帧头匹配但内容自相矛盾，继续等也不会变好，应当放弃。
// 返回第一个 status != invalid 的结果；全是噪声时返回 invalid。
FrameScan find_frame(const Bytes& capture);

}  // namespace libs::laser
