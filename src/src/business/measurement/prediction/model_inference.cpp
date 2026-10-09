#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "libs/business/measurement/prediction/model_inference.hpp"
#include "libs/business/measurement/prediction/algorithm_contract.hpp"

#include <filesystem>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace business::measurement::prediction {
namespace {
namespace fs = std::filesystem;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { close(); }
    void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};

libs::Error failure(libs::ErrorKind kind, const char* code, const std::string& message) {
    return libs::Error::make(kind, code, message);
}

std::wstring environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::vector<wchar_t> buffer(size);
    GetEnvironmentVariableW(name, buffer.data(), size);
    return buffer.data();
}

fs::path default_directory() {
    const auto override_dir = environment(L"LIBS_ALGORITHM_DIR");
    if (!override_dir.empty()) return fs::path(override_dir);
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) return {};
    const fs::path bin = fs::path(std::wstring(buffer.data(), size)).parent_path();
    if (fs::exists(bin / L"algorithms" / L"predict_worker.py")) return bin / L"algorithms";
    return bin.parent_path() / L"algorithms";
}

// Windows argv quoting, including trailing backslashes; no shell is involved.
std::wstring quoted(const std::wstring& arg) {
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'\"';
}

libs::Result<json::Value> read_json(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return failure(libs::ErrorKind::unsupported, "model_file_missing", "无法读取模型运行配置/响应: " + path.u8string());
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0 || size > 32 * 1024 * 1024)
        return failure(libs::ErrorKind::protocol, "model_file_size", "模型 JSON 文件超过 32 MB 限制");
    stream.seekg(0);
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), size);
    json::Value value;
    const auto parsed = json::parse(text, value);
    if (!parsed.ok || !value.is_object())
        return failure(libs::ErrorKind::protocol, "model_bad_json", "模型 JSON 无效: " + parsed.message);
    return value;
}

struct TempRequest {
    fs::path input, output;
    bool create() {
        wchar_t dir[MAX_PATH], file[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"lpm", 0, file)) return false;
        input = file;
        output = input.wstring() + L".response";
        return true;
    }
    ~TempRequest() {
        if (!input.empty()) DeleteFileW(input.c_str());
        if (!output.empty()) DeleteFileW(output.c_str());
    }
};
}  // namespace

struct ModelInference::Impl {
    mutable std::mutex mutex;
    fs::path directory;
    Handle process, job, input_pipe, output_pipe;
    std::string pending;
    DWORD timeout_ms = 60000;
    json::Value last;
    std::atomic<bool> cancelled{false};

    void reset() {
        if (process.value) TerminateProcess(process.value, 1);
        job.close();
        if (process.value) WaitForSingleObject(process.value, 1000);
        process.close(); input_pipe.close(); output_pipe.close(); pending.clear();
    }

    libs::Result<json::Value> read_line(ULONGLONG deadline) {
        while (GetTickCount64() < deadline) {
            if (cancelled.load())
                return failure(libs::ErrorKind::internal, "model_cancelled", "模型任务已取消");
            const auto newline = pending.find('\n');
            if (newline != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                json::Value value;
                const auto parsed = json::parse(line, value);
                if (!parsed.ok || !value.is_object())
                    return failure(libs::ErrorKind::protocol, "model_worker_protocol", "模型工作进程返回无效协议");
                return value;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(output_pipe.value, nullptr, 0, nullptr, &available, nullptr))
                return failure(libs::ErrorKind::internal, "model_worker_exited", "模型工作进程已退出");
            if (available) {
                char buffer[4096]; DWORD count = 0;
                if (!ReadFile(output_pipe.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr))
                    return failure(libs::ErrorKind::internal, "model_worker_read", "读取模型工作进程失败");
                pending.append(buffer, count);
                if (pending.size() > 65536)
                    return failure(libs::ErrorKind::protocol, "model_worker_protocol", "模型控制响应过长");
            } else {
                if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
                    return failure(libs::ErrorKind::internal, "model_worker_exited", "模型工作进程意外退出");
                Sleep(5);
            }
        }
        return failure(libs::ErrorKind::timeout, "model_inference_timeout", "模型加载/预测超时，工作进程将重启");
    }

    libs::VoidResult start() {
        if (cancelled.load())
            return failure(libs::ErrorKind::internal, "model_cancelled", "模型任务已取消");
        if (process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) return {};
        reset();
        if (directory.empty()) directory = default_directory();
        fs::path config = directory / L"runtime.local.json";
        if (!fs::exists(config)) config = directory / L"runtime.json";
        const auto loaded = read_json(config);
        if (!loaded) return loaded.error();
        const auto& cfg = loaded.value();
        fs::path python = fs::u8path(cfg["python"].as_string());
        const auto override_python = environment(L"LIBS_ALGORITHM_PYTHON");
        if (!override_python.empty()) python = override_python;
        const fs::path worker = directory / L"predict_worker.py";
        fs::path model = cfg["model_dir"].is_string() ? fs::u8path(cfg["model_dir"].as_string()) : fs::path(L"iron_ore/model/current");
        if (model.is_relative()) model = directory / model;
        if (!python.is_absolute() || !fs::exists(python) || !fs::exists(worker) || !fs::exists(model / L"manifest.json"))
            return failure(libs::ErrorKind::unsupported, "model_runtime_missing", "请配置 Python 绝对路径、predict_worker.py 和完整模型包");
        if (cfg.has("timeout_ms")) {
            const auto& t = cfg["timeout_ms"];
            if (!t.is_number() || t.as_number() < 1000 || t.as_number() > 120000 || std::floor(t.as_number()) != t.as_number())
                return failure(libs::ErrorKind::invalid_argument, "model_timeout_invalid", "模型 timeout_ms 须为 1000..120000 整数");
            timeout_ms = static_cast<DWORD>(t.as_number());
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle child_in, child_out, null_error;
        if (!CreatePipe(&child_in.value, &input_pipe.value, &security, 4096) ||
            !CreatePipe(&output_pipe.value, &child_out.value, &security, 4096)) {
            reset();
            return failure(libs::ErrorKind::internal, "model_worker_pipe", "创建模型进程管道失败");
        }
        SetHandleInformation(input_pipe.value, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_pipe.value, HANDLE_FLAG_INHERIT, 0);
        null_error.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &security, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_in.value;
        startup.hStdOutput = child_out.value;
        startup.hStdError = null_error.value;
        std::wstring command = quoted(python.wstring()) + L" -u " + quoted(worker.wstring()) + L" --model-dir " + quoted(model.wstring());
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &info)) {
            reset();
            return failure(libs::ErrorKind::unsupported, "model_worker_start", "启动模型 Python 失败，Win32=" + std::to_string(GetLastError()));
        }
        process.value = info.hProcess;
        Handle thread; thread.value = info.hThread;
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job.value, process.value)) {
            reset();
            return failure(libs::ErrorKind::internal, "model_worker_job", "无法管理模型工作进程生命周期");
        }
        ResumeThread(thread.value);
        child_in.close(); child_out.close();
        const auto ready = read_line(GetTickCount64() + timeout_ms);
        if (!ready) { const auto error = ready.error(); reset(); return error; }
        if (!ready.value()["ready"].as_bool()) {
            const auto error = failure(libs::ErrorKind::unsupported, "model_load_failed", "模型加载失败: " + ready.value()["error"].as_string());
            reset(); return error;
        }
        return {};
    }

    libs::Result<json::Value> call(const json::Value& request) {
        const auto started = start();
        if (!started) return started.error();
        TempRequest files;
        if (!files.create()) return failure(libs::ErrorKind::internal, "model_temp_file", "创建预测临时文件失败");
        const std::string blob = json::stringify(request);
        if (blob.size() > 32 * 1024 * 1024)
            return failure(libs::ErrorKind::invalid_argument, "model_input_size", "模型输入超过 32 MB 限制");
        { std::ofstream stream(files.input, std::ios::binary);
          stream.write(blob.data(), static_cast<std::streamsize>(blob.size()));
          if (!stream) return failure(libs::ErrorKind::internal, "model_input_write", "写入模型输入失败"); }
        json::Value control = json::object();
        control.set("input_file", files.input.u8string());
        control.set("output_file", files.output.u8string());
        const std::string line = json::stringify(control) + "\n";
        DWORD count = 0;
        if (!WriteFile(input_pipe.value, line.data(), static_cast<DWORD>(line.size()), &count, nullptr) || count != line.size()) {
            reset(); return failure(libs::ErrorKind::internal, "model_worker_write", "发送模型请求失败");
        }
        const auto done = read_line(GetTickCount64() + timeout_ms);
        if (!done) { const auto error = done.error(); reset(); return error; }
        if (!done.value()["done"].as_bool()) {
            reset(); return failure(libs::ErrorKind::protocol, "model_worker_protocol", "模型响应缺少完成标记");
        }
        const auto response = read_json(files.output);
        if (!response) return response.error();
        if (!response.value()["ok"].as_bool())
            return failure(response.value()["error_kind"].as_string() == "invalid_argument" ? libs::ErrorKind::invalid_argument : libs::ErrorKind::internal,
                           "model_prediction_failed", response.value()["error"].as_string());
        if (!response.value()["payload"].is_object())
            return failure(libs::ErrorKind::protocol, "model_worker_protocol", "模型预测响应缺少 payload");
        return response.value()["payload"];
    }
};

ModelInference::ModelInference() : impl_(new Impl) {}
ModelInference::~ModelInference() { stop(); }
void ModelInference::configure(const std::string& directory) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->reset(); impl_->last = json::Value();
    impl_->cancelled.store(false);
    impl_->directory = fs::absolute(directory.empty() ? default_directory() : fs::u8path(directory));
}
libs::Result<json::Value> ModelInference::status() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    json::Value request = json::object(); request.set("action", "status");
    return impl_->call(request);
}
libs::Result<json::Value> ModelInference::analyze(const json::Value& input) {
    const auto checked = algorithm::validate_input(input);
    if (!checked) return checked.error();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    json::Value request = json::object(); request.set("action", "analyze"); request.set("input", input);
    auto result = impl_->call(request);
    if (result) {
        result.value().set("validation", checked.value());
        impl_->last = result.value();
    }
    return result;
}
json::Value ModelInference::latest() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->last.is_object()) return impl_->last;
    json::Value value = json::object();
    value.set("availability", "unavailable"); value.set("executed", false);
    value.set("reason", "no_prediction_yet");
    return value;
}
void ModelInference::stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex); impl_->reset();
}
void ModelInference::cancel() noexcept { impl_->cancelled.store(true); }
}  // namespace business::measurement::prediction
