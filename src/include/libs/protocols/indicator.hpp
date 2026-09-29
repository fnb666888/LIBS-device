#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "libs/error.hpp"
#include "libs/hex.hpp"

namespace libs::indicator {

// 科隆威三色灯协议：纯编解码，无 I/O。
//
// 两套地址空间不要混用：
//   查询（功能码 0x01）：红 1 / 绿 2 / 蓝 3 / 黄 4 / 白 5 / 蜂鸣 6
//   写线圈（功能码 0x05）：红黄绿蜂鸣 = 1/2/3/4（K50 黄绿互换），蓝/白 = 0x11/0x12，全关 = 0
// 闪烁档位在基础色上是 线圈+5（1 Hz）和 线圈+0x15（2 Hz）；
// 蓝/白不是这个规律（+0x10 / +0x12）。

using Bytes = libs::Bytes;

enum class Channel { red, yellow, green, blue, white, buzzer };
enum class State  { off, on, blink_1hz, blink_2hz };

// 解析失败返回错误而不是静默 fallback。
Result<Channel> parse_channel(const std::string& name);
Result<State>   parse_state(const std::string& name);
const char* to_string(Channel channel) noexcept;
const char* to_string(State state) noexcept;

// 已知型号。没有登记的型号返回空字符串——写命令需要据此判断能不能构造。
struct ModelTraits {
    std::string model;
    bool k50 = false;              // CL50S+K50-485：黄/绿通道与普通灯相反
    bool rgb_only = false;         // RL30-24V：文档没有蜂鸣器命令
    bool has_blue_white = false;   // 只有 5 层型号登记过蓝/白
    bool individual_off = true;    // 是否登记过单通道熄灭命令
    bool slow_blink_is_1hz = true; // K50 的慢闪频率没有记录为精确 1 Hz
};

// 厂家协议 v2.19 登记的型号。model 为空则返回 false。
bool lookup_model(const std::string& model, ModelTraits& traits);

// 查询寄存器号（1..6）。未知通道抛 std::invalid_argument。
std::uint16_t query_register(Channel channel);

// 查询请求（功能码 0x01）。未知通道抛 std::invalid_argument。
Bytes make_query_request(std::uint8_t address, Channel channel);

struct QueryResponse {
    Channel channel{};
    std::uint16_t register_address = 0;
    std::uint8_t raw_state = 0;
    State state{};
};

// 解码查询响应。
Result<QueryResponse> decode_query_response(const Bytes& capture, std::uint8_t address, Channel channel);

struct WriteRequest {
    std::string model;
    Channel channel{};
    State state{};
};

// 构造写帧（功能码 0x05）。
//
// 型号未登记、该型号没有这个通道、或该状态该型号不支持时返回错误，
// 一个字节都不生成。这是编解码层的结构约束，不是安全策略。
Result<Bytes> make_write_request(std::uint8_t address, const WriteRequest& request);

}  // namespace libs::indicator
