#include "libs/transport/serial.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

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

public:
    SerialLink(const std::string& port, unsigned baud, Parity parity, unsigned data_bits)
        : endpoint_(port) {
        const std::string path = "\\\\.\\" + port;
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            throw std::runtime_error(win_error("open " + port));
        try {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (!GetCommState(h, &dcb)) throw std::runtime_error(win_error("GetCommState"));
            dcb.BaudRate = baud;
            dcb.ByteSize = static_cast<BYTE>(data_bits);
            dcb.StopBits = ONESTOPBIT;
            dcb.Parity = parity == Parity::even ? EVENPARITY
                      : parity == Parity::odd  ? ODDPARITY : NOPARITY;
            dcb.fBinary = TRUE;
            dcb.fParity = parity != Parity::none;
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

        // 每次事务先清空缓冲：上一次残留绝不能被当成本次响应。
        if (!PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR))
            return Error::with_raw(ErrorKind::transport, "purge_failed",
                                   win_error("PurgeComm"), request);

        const auto start = std::chrono::steady_clock::now();
        DWORD written = 0;
        if (!WriteFile(handle_, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) ||
            written != request.size())
            return Error::with_raw(ErrorKind::transport, "write_failed",
                                   win_error("WriteFile did not send the whole request"), request);

        Reply reply{};
        for (;;) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= static_cast<long long>(timeout_ms)) { reply.timed_out = true; break; }

            DWORD errors = 0;
            COMSTAT status{};
            if (!ClearCommError(handle_, &errors, &status))
                return Error::with_raw(ErrorKind::transport, "status_failed",
                                       win_error("ClearCommError"), request);
            reply.receive_errors |= errors;

            if (status.cbInQue) {
                std::uint8_t buffer[2048];
                DWORD received = 0;
                const DWORD amount = std::min<DWORD>(status.cbInQue, sizeof(buffer));
                if (!ReadFile(handle_, buffer, amount, &received, nullptr))
                    return Error::with_raw(ErrorKind::transport, "read_failed",
                                           win_error("ReadFile"), request);
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
