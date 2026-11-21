#include "libs/transport/serial.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "libs/transport/serial_timing.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace libs::transport {
namespace {

#if defined(_WIN32)
std::string win_error(const std::string& context) {
    return context + " (Win32 " + std::to_string(GetLastError()) + ")";
}

bool valid_com_name(const std::string& port) {
    if (port.size() < 4 || port.compare(0, 3, "COM") != 0) return false;
    return std::all_of(port.begin() + 3, port.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

class SerialLink final : public Link {
    HANDLE handle_ = nullptr;
    std::string endpoint_;
    unsigned baud_;
    Parity parity_;
    unsigned data_bits_;

    void open_handle() {
        const std::string path = "\\\\.\\" + endpoint_;
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw std::runtime_error(win_error("open " + endpoint_));
        try {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (!GetCommState(h, &dcb)) throw std::runtime_error(win_error("GetCommState"));
            dcb.BaudRate = baud_;
            dcb.ByteSize = static_cast<BYTE>(data_bits_);
            dcb.StopBits = ONESTOPBIT;
            dcb.Parity = parity_ == Parity::even ? EVENPARITY
                      : parity_ == Parity::odd  ? ODDPARITY : NOPARITY;
            dcb.fBinary = TRUE;
            dcb.fParity = parity_ != Parity::none;
            // 显式关闭所有流控：现场设备常常不接 RTS/CTS，驱动默认行为会静默丢数据。
            dcb.fOutxCtsFlow = FALSE;
            dcb.fOutxDsrFlow = FALSE;
            dcb.fDtrControl = DTR_CONTROL_DISABLE;
            dcb.fDsrSensitivity = FALSE;
            dcb.fOutX = FALSE;
            dcb.fInX = FALSE;
            dcb.fRtsControl = RTS_CONTROL_DISABLE;
            dcb.fAbortOnError = FALSE;
            dcb.fNull = FALSE;
            dcb.fErrorChar = FALSE;
            if (!SetCommState(h, &dcb)) throw std::runtime_error(win_error("SetCommState"));

            COMMTIMEOUTS timeouts{};
            timeouts.ReadIntervalTimeout = MAXDWORD;
            timeouts.ReadTotalTimeoutConstant = 20;
            timeouts.WriteTotalTimeoutConstant = 300;
            if (!SetCommTimeouts(h, &timeouts)) throw std::runtime_error(win_error("SetCommTimeouts"));
            if (!SetupComm(h, 8192, 8192)) throw std::runtime_error(win_error("SetupComm"));
            handle_ = h;
        } catch (...) {
            CloseHandle(h);
            throw;
        }
    }

    Error transport_failure(const char* code, const char* operation, const Bytes& request) {
        auto error = Error::with_raw(ErrorKind::transport, code, win_error(operation), request);
        CloseHandle(handle_);
        handle_ = nullptr;
        // The next caller will reconnect. Never replay a possibly completed write.
        return error;
    }

public:
    SerialLink(const std::string& port, unsigned baud, Parity parity, unsigned data_bits)
        : endpoint_(port), baud_(baud), parity_(parity), data_bits_(data_bits) {
        open_handle();
    }

    ~SerialLink() override {
        if (handle_) CloseHandle(handle_);
    }

    Result<Reply> exchange(const Bytes& request, unsigned timeout_ms, unsigned idle_ms) override {
        if (request.empty() || request.size() > 1024)
            return Error::make(ErrorKind::invalid_argument, "bad_request_size",
                               "a serial request must be 1..1024 bytes");
        if (timeout_ms > 10000 || idle_ms > timeout_ms)
            return Error::make(ErrorKind::invalid_argument, "bad_timing",
                               "require timeout_ms <= 10000 and idle_ms <= timeout_ms");

        if (!handle_) {
            try { open_handle(); }
            catch (const std::exception& e) {
                return Error::with_raw(ErrorKind::transport, "reopen_failed", e.what(), request);
            }
        }

        // 每次事务先清空缓冲：上一次残留绝不能被当成本次响应。
        if (!PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR))
            return transport_failure("purge_failed", "PurgeComm", request);

        const auto start = std::chrono::steady_clock::now();
        DWORD written = 0;
        if (!WriteFile(handle_, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) ||
            written != request.size())
            return transport_failure("write_failed", "WriteFile did not send the whole request", request);

        Reply reply{};
        for (;;) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            // 帧结束判定：收到字节之后空闲满 idle_ms 就收工，不再白等到 timeout_ms。
            // 修复前这里只判超时，于是每一次串口事务都耗满整份时间预算
            // （实测 elapsed_ms=655、timed_out=true，而回包是一帧完整合法应答）。
            const std::uint64_t now_tick = GetTickCount64();
            const unsigned long long idle_gap_ms =
                reply.last_receive_tick == 0 ? 0 : (now_tick - reply.last_receive_tick);
            const ReadStep step =
                next_read_step(!reply.bytes.empty(), idle_gap_ms,
                               static_cast<unsigned long long>(elapsed), idle_ms, timeout_ms);
            if (step == ReadStep::frame_complete) break;
            if (step == ReadStep::timed_out) { reply.timed_out = true; break; }

            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle_, &errors, &status))
                return transport_failure("status_failed", "ClearCommError", request);
            reply.receive_errors |= errors;

            if (status.cbInQue) {
                std::uint8_t buffer[2048];
                DWORD received = 0;
                const DWORD amount = std::min<DWORD>(status.cbInQue, sizeof(buffer));
                if (!ReadFile(handle_, buffer, amount, &received, nullptr))
                    return transport_failure("read_failed", "ReadFile", request);
                reply.bytes.insert(reply.bytes.end(), buffer, buffer + received);
                if (reply.bytes.size() > 8192)
                    return Error::make(ErrorKind::transport, "reply_too_large",
                                       "serial reply exceeded the 8192-byte bound");
                if (received) reply.last_receive_tick = GetTickCount64();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        reply.elapsed_ms = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
        return reply;
    }

    std::string endpoint() const override { return endpoint_; }
};
#endif  // _WIN32

}  // namespace

Result<std::unique_ptr<Link>> open_serial(const std::string& port, unsigned baud,
                                          Parity parity, unsigned data_bits) {
#if defined(_WIN32)
    if (!valid_com_name(port))
        return Error::make(ErrorKind::invalid_argument, "bad_port",
                           "expected a COM port name such as COM3, got '" + port + "'");
    if (baud == 0)
        return Error::make(ErrorKind::invalid_argument, "bad_baud", "baud rate must be nonzero");
    if (data_bits < 5 || data_bits > 8)
        return Error::make(ErrorKind::invalid_argument, "bad_data_bits", "data bits must be 5..8");
    try {
        return std::unique_ptr<Link>(new SerialLink(port, baud, parity, data_bits));
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::transport, "open_failed", e.what());
    }
#else
    (void)port; (void)baud; (void)parity; (void)data_bits;
    return Error::make(ErrorKind::unsupported, "no_serial",
                       "serial ports are only implemented on Windows in this build");
#endif
}

std::vector<std::string> list_serial_ports() {
    std::vector<std::string> result;
#if defined(_WIN32)
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return result;
    for (DWORD i = 0;; ++i) {
        char name[512];
        BYTE data[512]{};
        DWORD name_len = sizeof(name), data_len = sizeof(data), type = 0;
        const auto code = RegEnumValueA(key, i, name, &name_len, nullptr, &type, data, &data_len);
        if (code == ERROR_NO_MORE_ITEMS) break;
        if (code == ERROR_SUCCESS && type == REG_SZ && data_len < sizeof(data))
            result.emplace_back(reinterpret_cast<char*>(data));
    }
    RegCloseKey(key);
    // 按数字排序，避免 COM10 排在 COM2 前面。
    std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
        try { return std::stoi(a.substr(3)) < std::stoi(b.substr(3)); }
        catch (...) { return a < b; }
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
#endif
    return result;
}

}  // namespace libs::transport
