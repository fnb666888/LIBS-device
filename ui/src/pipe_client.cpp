#include "pipe_client.hpp"

#include <QDir>
#include <QFile>
#include <QProcessEnvironment>

#include <algorithm>
#include <limits>

// Win32 命名管道。走 kernel32 而不是 Qt 的 QLocalSocket：Windows 上
// QLocalSocket 底层是 AF_UNIX/命名管道，但语义（字节流模式、每次连接
// 一个实例）和这里的守护进程实现不保证一致；直接用 CreateFileW
// 跟 `remote/lpc.py`（ctypes 版）逐行对应，行为可对照、可排障。
#include <windows.h>

namespace libs::ui {
namespace {

QString win_error_text(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    // 老写法（QString::fromWCharArray），不用 QString::reinterpret_cast：
    // 后者是 Qt 6.8 才加的，而这个工程要能在现场机上跑，Qt 版本不由我们定。
    QString text = n ? QString::fromWCharArray(buffer).trimmed()
                     : QStringLiteral("未知错误");
    if (buffer) LocalFree(buffer);
    return text;
}

// A synchronous ReadFile/WriteFile on a named pipe has no usable timeout.  Use
// one overlapped operation per transfer and wait on its event instead.  The
// handle is closed by the caller after a timeout; CancelIoEx is issued first so
// the kernel does not retain a pending request against a handle that we reuse.
bool overlapped_transfer(HANDLE handle, bool writing, void* data, DWORD size,
                        DWORD timeout_ms, DWORD* transferred, DWORD* error) {
    if (transferred) *transferred = 0;
    if (error) *error = ERROR_SUCCESS;

    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        if (error) *error = ::GetLastError();
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    DWORD done = 0;
    const BOOL started = writing
        ? ::WriteFile(handle, data, size, &done, &overlapped)
        : ::ReadFile(handle, data, size, &done, &overlapped);

    if (!started) {
        const DWORD start_error = ::GetLastError();
        if (start_error != ERROR_IO_PENDING) {
            ::CloseHandle(event);
            if (error) *error = start_error;
            return false;
        }

        const DWORD wait_result = ::WaitForSingleObject(event, timeout_ms);
        if (wait_result == WAIT_TIMEOUT) {
            ::CancelIoEx(handle, &overlapped);
            // OVERLAPPED lives on this stack frame. It must not be released
            // until the cancelled request has completed; otherwise a slow
            // cancellation can make the next stack user look like a pipe
            // protocol failure. CancelIoEx is followed by an unconditional
            // completion wait for that reason.
            ::WaitForSingleObject(event, INFINITE);
            ::CloseHandle(event);
            if (error) *error = WAIT_TIMEOUT;
            return false;
        }
        if (wait_result != WAIT_OBJECT_0) {
            const DWORD wait_error = ::GetLastError();
            ::CancelIoEx(handle, &overlapped);
            ::WaitForSingleObject(event, INFINITE);
            ::CloseHandle(event);
            if (error) *error = wait_error ? wait_error : ERROR_OPERATION_ABORTED;
            return false;
        }
    }

    DWORD result = 0;
    if (!::GetOverlappedResult(handle, &overlapped, &result, FALSE)) {
        const DWORD result_error = ::GetLastError();
        ::CloseHandle(event);
        if (transferred) *transferred = result;
        if (error) *error = result_error;
        return false;
    }
    ::CloseHandle(event);
    if (transferred) *transferred = result;
    return true;
}

DWORD remaining_ms(ULONGLONG deadline) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) return 0;
    const ULONGLONG remaining = deadline - now;
    return remaining > std::numeric_limits<DWORD>::max()
        ? std::numeric_limits<DWORD>::max()
        : static_cast<DWORD>(remaining);
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~ScopedHandle() { close(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    void reset(HANDLE handle) {
        close();
        handle_ = handle;
    }
    void close() {
        if (valid()) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }
private:
    HANDLE handle_;
};

}  // namespace

QString PipeClient::default_pipe_name() {
    const QString from_env =
        QProcessEnvironment::systemEnvironment().value("LIBS_BUSINESSD_PIPE");
    if (!from_env.isEmpty()) return from_env;
    return QStringLiteral(R"(\\.\pipe\libs-businessd)");
}

QJsonObject PipeClient::call(const QJsonObject& request, int timeout_ms) {
    const QString pipe = pipe_name_.isEmpty() ? default_pipe_name() : pipe_name_;

    // A non-positive timeout used to create an unbounded ReadFile/WriteFile.
    // Keep the public API compatible, but never allow a UI request to wait
    // forever when a caller accidentally passes 0 or -1.
    const DWORD effective_timeout = static_cast<DWORD>(
        timeout_ms > 0 ? timeout_ms : 5000);

    // 服务端是每连接一线程，但 CreateNamedPipe/ConnectNamedPipe 之间仍有
    // 一个很短的窗口；多个 UI 轮询器同时到达时，CreateFileW 会暂时返回
    // ERROR_PIPE_BUSY(231) 或 ERROR_FILE_NOT_FOUND(2)。这不是“后端掉线”，
    // 必须在客户端等待同一条管道，而不是立刻把瞬时竞争显示成红色故障。
    ScopedHandle handle;
    const ULONGLONG deadline = ::GetTickCount64() + effective_timeout;
    DWORD last_error = ERROR_SUCCESS;
    DWORD backoff_ms = 10;
    for (;;) {
        handle.reset(::CreateFileW(
            reinterpret_cast<LPCWSTR>(pipe.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
        if (handle.valid()) break;

        last_error = ::GetLastError();
        const bool transient = last_error == ERROR_PIPE_BUSY ||
                               last_error == ERROR_FILE_NOT_FOUND;
        if (!transient) break;

        const DWORD remaining = remaining_ms(deadline);
        if (remaining == 0) {
            last_error = WAIT_TIMEOUT;
            break;
        }

        // WaitNamedPipe is useful for a busy instance.  For a not-yet-created
        // pipe it returns FILE_NOT_FOUND, so sleep with exponential backoff
        // rather than spinning and flooding the named-pipe namespace.
        const DWORD wait_ms = std::min(backoff_ms, remaining);
        if (last_error == ERROR_PIPE_BUSY) {
            if (!::WaitNamedPipeW(reinterpret_cast<LPCWSTR>(pipe.utf16()), wait_ms)) {
                const DWORD wait_error = ::GetLastError();
                if (wait_error != ERROR_PIPE_BUSY &&
                    wait_error != ERROR_FILE_NOT_FOUND &&
                    wait_error != ERROR_SEM_TIMEOUT) {
                    last_error = wait_error;
                    break;
                }
            }
        } else {
            ::Sleep(wait_ms);
        }
        backoff_ms = std::min<DWORD>(backoff_ms * 2, 250);
    }

    if (!handle.valid()) {
        throw QStringLiteral("无法打开命名管道 %1（%2，Win32 %3）。守护进程 businessd 是否已启动？")
            .arg(pipe, win_error_text(last_error))
            .arg(last_error);
    }

    DWORD mode = PIPE_READMODE_BYTE;  // 显式设成字节流，与服务端一致
    if (!::SetNamedPipeHandleState(handle.get(), &mode, nullptr, nullptr)) {
        const DWORD err = ::GetLastError();
        throw QStringLiteral("设置命名管道读模式失败（%1，Win32 %2）")
            .arg(win_error_text(err)).arg(err);
    }

    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    DWORD offset = 0;
    while (offset < static_cast<DWORD>(payload.size())) {
        const DWORD remaining = remaining_ms(deadline);
        if (remaining == 0) {
            throw QStringLiteral("向守护进程写入请求超时（%1 ms）").arg(effective_timeout);
        }
        DWORD written = 0;
        DWORD err = ERROR_SUCCESS;
        if (!overlapped_transfer(handle.get(), true,
                                 const_cast<char*>(payload.constData()) + offset,
                                 static_cast<DWORD>(payload.size()) - offset,
                                 remaining, &written, &err)) {
            if (err == WAIT_TIMEOUT) {
                throw QStringLiteral("向守护进程写入请求超时（%1 ms）").arg(effective_timeout);
            }
            throw QStringLiteral("向守护进程写入请求失败（%1，Win32 %2）")
                .arg(win_error_text(err)).arg(err);
        }
        if (written == 0) {
            throw QStringLiteral("向守护进程写入请求失败：管道未写入数据");
        }
        offset += written;
    }

    // 守护进程一行一答：一直读到换行或管道关闭为止。
    QByteArray buffer;
    while (!buffer.contains('\n')) {
        char chunk[4096];
        DWORD got = 0;
        const DWORD remaining = remaining_ms(deadline);
        if (remaining == 0) {
            throw QStringLiteral("读取守护进程响应超时（%1 ms）").arg(effective_timeout);
        }
        DWORD err = ERROR_SUCCESS;
        if (!overlapped_transfer(handle.get(), false, chunk, sizeof(chunk),
                                 remaining, &got, &err)) {
            if (err == WAIT_TIMEOUT) {
                throw QStringLiteral("读取守护进程响应超时（%1 ms）").arg(effective_timeout);
            }
            // ERROR_BROKEN_PIPE / ERROR_PIPE_NOT_CONNECTED：服务端中途退了。
            throw QStringLiteral("读取响应失败（%1，Win32 %2）")
                .arg(win_error_text(err)).arg(err);
        }
        if (got == 0) break;  // 对端关闭
        buffer.append(chunk, static_cast<int>(got));
        if (buffer.size() > 64 * 1024 * 1024) {
            throw QStringLiteral("响应超过 64MB，判定为异常并断开");
        }
    }
    const int nl = buffer.indexOf('\n');
    if (nl < 0) {
        throw QStringLiteral("守护进程关闭了连接但没有返回任何内容");
    }
    QJsonParseError parse_error{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(buffer.left(nl), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        throw QStringLiteral("响应不是一行合法 JSON 对象：%1").arg(parse_error.errorString());
    }
    return doc.object();
}

QJsonObject PipeClient::command(const QString& name, const QJsonObject& args, int timeout_ms) {
    QJsonObject request;
    request.insert("command", name);
    request.insert("args", args);
    const QJsonObject response = call(request, timeout_ms);
    if (response.value("ok").toBool(false)) return response;
    // ok=false 不是传输问题，是业务层明确给出的判定。原样上送，
    // 不要在 UI 这里改写它的含义。
    // businessd has emitted both the old object form
    //   {error:{code,message,kind}}
    // and the newer compact form
    //   {error:"...", error_code:"...", error_kind:"..."}.
    // Keep both forms readable so a backend upgrade cannot turn a useful
    // device error into the unhelpful "unknown/(无消息)" shown by the UI.
    QString code = response.value(QStringLiteral("error_code")).toString();
    QString kind = response.value(QStringLiteral("error_kind")).toString();
    QString message;
    const QJsonValue error_value = response.value(QStringLiteral("error"));
    if (error_value.isObject()) {
        const QJsonObject error = error_value.toObject();
        if (code.isEmpty()) code = error.value(QStringLiteral("code")).toString();
        if (code.isEmpty()) code = error.value(QStringLiteral("error_code")).toString();
        if (kind.isEmpty()) kind = error.value(QStringLiteral("kind")).toString();
        if (kind.isEmpty()) kind = error.value(QStringLiteral("error_kind")).toString();
        message = error.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) message = error.value(QStringLiteral("detail")).toString();
    } else if (error_value.isString()) {
        message = error_value.toString();
    }
    if (message.isEmpty()) message = response.value(QStringLiteral("message")).toString();
    if (message.isEmpty()) message = response.value(QStringLiteral("detail")).toString();
    if (code.isEmpty()) code = response.value(QStringLiteral("code")).toString();
    if (kind.isEmpty()) kind = response.value(QStringLiteral("kind")).toString();
    if (code.isEmpty()) code = QStringLiteral("unknown");
    if (kind.isEmpty()) kind = QStringLiteral("unknown");
    if (message.isEmpty()) message = QStringLiteral("(无消息)");
    throw QStringLiteral("%1：%2（%3）")
        .arg(code, message, kind);
}

QJsonObject PipeClient::command_payload(const QString& name, const QJsonObject& args,
                                        int timeout_ms) {
    return command(name, args, timeout_ms).value(QStringLiteral("payload")).toObject();
}

}  // namespace libs::ui
