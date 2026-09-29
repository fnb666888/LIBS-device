#include "pipe_client.hpp"

#include <QDir>
#include <QFile>
#include <QProcessEnvironment>

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

}  // namespace

QString PipeClient::default_pipe_name() {
    const QString from_env =
        QProcessEnvironment::systemEnvironment().value("LIBS_BUSINESSD_PIPE");
    if (!from_env.isEmpty()) return from_env;
    return QStringLiteral(R"(\\.\pipe\libs-businessd)");
}

QJsonObject PipeClient::call(const QJsonObject& request, int timeout_ms) {
    const QString pipe = pipe_name_.isEmpty() ? default_pipe_name() : pipe_name_;

    // 连接超时也要给：守护进程没起时最常见的是"管道不存在"，那是
    // 启动顺序问题（不是业务失败），错误文案要说清楚。
    const HANDLE handle = ::CreateFileW(
        reinterpret_cast<LPCWSTR>(pipe.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        throw QStringLiteral("无法打开命名管道 %1（%2，Win32 %3）。守护进程 businessd 是否已启动？")
            .arg(pipe, win_error_text(err))
            .arg(err);
    }

    DWORD mode = PIPE_READMODE_BYTE;  // 显式设成字节流，与服务端一致
    ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);

    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";
    DWORD written = 0;
    if (!::WriteFile(handle, payload.constData(), static_cast<DWORD>(payload.size()),
                     &written, nullptr) ||
        written != static_cast<DWORD>(payload.size())) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(handle);
        throw QStringLiteral("向守护进程写入请求失败（%1，Win32 %2）")
            .arg(win_error_text(err)).arg(err);
    }

    // 守护进程一行一答：一直读到换行或管道关闭为止。
    QByteArray buffer;
    while (!buffer.contains('\n')) {
        char chunk[4096];
        DWORD got = 0;
        if (!::ReadFile(handle, chunk, sizeof(chunk), &got, nullptr)) {
            const DWORD err = ::GetLastError();
            ::CloseHandle(handle);
            // ERROR_BROKEN_PIPE / ERROR_PIPE_NOT_CONNECTED：服务端中途退了。
            throw QStringLiteral("读取响应失败（%1，Win32 %2）")
                .arg(win_error_text(err)).arg(err);
        }
        if (got == 0) break;  // 对端关闭
        buffer.append(chunk, static_cast<int>(got));
        if (buffer.size() > 64 * 1024 * 1024) {
            ::CloseHandle(handle);
            throw QStringLiteral("响应超过 64MB，判定为异常并断开");
        }
    }
    ::CloseHandle(handle);

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
    Q_UNUSED(timeout_ms)  // 同步读写；守护进程本地管道不会长时间占用
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
    const QJsonObject error = response.value("error").toObject();
    throw QStringLiteral("%1：%2（%3）")
        .arg(error.value("code").toString("unknown"),
             error.value("message").toString("(无消息)"),
             error.value("kind").toString("unknown"));
}

QJsonObject PipeClient::command_payload(const QString& name, const QJsonObject& args,
                                        int timeout_ms) {
    return command(name, args, timeout_ms).value(QStringLiteral("payload")).toObject();
}

}  // namespace libs::ui
