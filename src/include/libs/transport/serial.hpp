#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "libs/error.hpp"
#include "libs/hex.hpp"

namespace libs::transport {

// 传输层：只负责字节收发与超时，不理解任何设备业务。
//
// 设备驱动依赖这个接口而不是具体串口实现，因此可以用假传输做离线测试，
// 不需要真实 COM 口。打开端口失败属于编程/环境错误，返回 Error；
// 奇偶/帧/溢出错误不吞掉，通过 Reply::receive_errors 上报。

using Bytes = libs::Bytes;

enum class Parity { none, even, odd };

struct Reply {
    Bytes bytes;
    unsigned elapsed_ms = 0;
    unsigned receive_errors = 0;      // CE/PE/FE/OE 等位计数
    std::uint64_t last_receive_tick = 0;  // 单调时钟毫秒，0 表示本事务没收到字节
    bool timed_out = false;           // 在收到完整帧之前就用完了时间预算
};

class Link {
public:
    virtual ~Link() = default;
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // 一次请求-响应事务。每次调用都是独立事务，不会把上次的残留当成回复。
    virtual Result<Reply> exchange(const Bytes& request,
                                   unsigned timeout_ms = 650,
                                   unsigned idle_ms = 80) = 0;

    // 供驱动器识别端口。所有串口都支持的空操作。
    virtual std::string endpoint() const = 0;

protected:
    Link() = default;
};

// Windows COM 口。构造失败返回错误而不是抛异常。
Result<std::unique_ptr<Link>> open_serial(const std::string& port, unsigned baud,
                                          Parity parity = Parity::none,
                                          unsigned data_bits = 8);

// 只列出 Windows 当前存在的串口名（如 COM3），不打开任何一个。
std::vector<std::string> list_serial_ports();

}  // namespace libs::transport
