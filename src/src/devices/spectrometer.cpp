#include "libs/devices/spectrometer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace libs::devices {
namespace {
using Clock = std::chrono::steady_clock;


#if defined(_WIN32)

// ── 厂商 ABI ────────────────────────────────────────────────────────────────
// 结构体布局取自 SDK examples/MFC/avaspec.h，全部 pack(1)。
// AvsHandle 在 x64 Windows 上仍是 32 位 long，函数指针一律 __stdcall。
using AvsHandle = std::int32_t;
constexpr AvsHandle kInvalidHandle = 1000;

#pragma pack(push, 1)
struct AvsIdentity {
    char serial[10];
    char friendly_name[64];
    std::uint8_t status;
};
struct DarkCorrection { std::uint8_t enabled, forget_percentage; };
struct Smoothing { std::uint16_t pixels; std::uint8_t model; };
struct Trigger { std::uint8_t mode, source, source_type; };
struct Control {
    std::uint16_t strobe;
    std::uint32_t laser_delay, laser_width;
    float laser_wavelength;
    std::uint16_t store_to_ram;
};
struct MeasureConfig {
    std::uint16_t start_pixel, stop_pixel;
    float integration_ms;
    std::uint32_t integration_delay, averages;
    DarkCorrection dark;
    Smoothing smoothing;
    std::uint8_t saturation_detection;
    Trigger trigger;
    Control control;
};
#pragma pack(pop)

static_assert(sizeof(AvsIdentity) == 75, "AvaSpec identity ABI mismatch");
static_assert(sizeof(AvsHandle) == 4, "AvaSpec handle ABI mismatch");
static_assert(sizeof(Trigger) == 3 && sizeof(Control) == 16, "AvaSpec nested ABI mismatch");
static_assert(sizeof(MeasureConfig) == 41, "AvaSpec measurement ABI mismatch");
static_assert(offsetof(MeasureConfig, trigger) == 22, "AvaSpec trigger offset mismatch");
static_assert(offsetof(MeasureConfig, control) == 25, "AvaSpec control offset mismatch");
static_assert(offsetof(Control, laser_width) == 6, "AvaSpec pulse width offset mismatch");

std::wstring utf8_to_wide(const std::string& value) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) throw std::runtime_error("the SDK DLL path must be valid UTF-8");
    std::wstring result(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), n);
    return result;
}

class Dll {
    HMODULE handle_ = nullptr;
public:
    explicit Dll(const std::string& path) {
        const auto wide = utf8_to_wide(path);
        if (!std::filesystem::path(wide).is_absolute())
            throw std::runtime_error("sdk_dll must be an absolute path");
        handle_ = LoadLibraryExW(wide.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!handle_)
            throw std::runtime_error("cannot load the AvaSpec DLL; Win32 error " +
                                     std::to_string(GetLastError()));
    }
    ~Dll() { if (handle_) FreeLibrary(handle_); }
    Dll(const Dll&) = delete;
    Dll& operator=(const Dll&) = delete;

    template <class T>
    T get(const char* name) {
        const auto address = GetProcAddress(handle_, name);
        if (!address) throw std::runtime_error(std::string("missing AvaSpec export: ") + name);
        // GetProcAddress 保证返回的就是被请求的导出函数；按指针表示逐字节拷贝，
        // 避免 FARPROC 的泛型转换警告，同时保留精确签名。
        static_assert(sizeof(T) == sizeof(address), "Windows function pointer ABI mismatch");
        T function{};
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }
};

struct Api {
    Dll dll;
    int  (__stdcall* init)(short);
    int  (__stdcall* done)();
    int  (__stdcall* list)(unsigned int, unsigned int*, AvsIdentity*);
    AvsHandle (__stdcall* activate)(AvsIdentity*);
    bool (__stdcall* deactivate)(AvsHandle);
    int  (__stdcall* pixels)(AvsHandle, unsigned short*);
    int  (__stdcall* wavelength)(AvsHandle, double*);
    int  (__stdcall* version)(AvsHandle, char*, char*, char*);
    int  (__stdcall* device_type)(AvsHandle, int*);
    int  (__stdcall* sync)(AvsHandle, unsigned char);
    int  (__stdcall* prepare)(AvsHandle, MeasureConfig*);
    // AVS_Measure 的第二个参数在 Windows 上是 HWND，不是回调。
    // 传 nullptr 关闭窗口通知，结果用 AVS_PollScan 取。
    int  (__stdcall* measure)(AvsHandle, void*, short);
    int  (__stdcall* poll)(AvsHandle);
    int  (__stdcall* scope)(AvsHandle, unsigned int*, double*);
    int  (__stdcall* stop)(AvsHandle);

    explicit Api(const std::string& path) : dll(path) {
#define LIBS_BIND(member, name) member = dll.get<decltype(member)>(name)
        LIBS_BIND(init, "AVS_Init");            LIBS_BIND(done, "AVS_Done");
        LIBS_BIND(list, "AVS_GetList");         LIBS_BIND(activate, "AVS_Activate");
        LIBS_BIND(deactivate, "AVS_Deactivate");LIBS_BIND(pixels, "AVS_GetNumPixels");
        LIBS_BIND(wavelength, "AVS_GetLambda"); LIBS_BIND(version, "AVS_GetVersionInfo");
        LIBS_BIND(device_type, "AVS_GetDeviceType"); LIBS_BIND(sync, "AVS_SetSyncMode");
        LIBS_BIND(prepare, "AVS_PrepareMeasure");LIBS_BIND(measure, "AVS_Measure");
        LIBS_BIND(poll, "AVS_PollScan");        LIBS_BIND(scope, "AVS_GetScopeData");
        LIBS_BIND(stop, "AVS_StopMeasure");
#undef LIBS_BIND
    }
};

std::string bounded_string(const char* buffer, std::size_t maximum) {
    std::size_t length = 0;
    while (length < maximum && buffer[length]) ++length;
    return std::string(buffer, length);
}

const char* status_name(std::uint8_t value) {
    const char* names[] = {"unknown", "usb_available", "usb_in_use_by_application",
                           "usb_in_use_by_other", "ethernet_available",
                           "ethernet_in_use_by_application", "ethernet_in_use_by_other",
                           "ethernet_already_in_use_usb"};
    return value < 8 ? names[value] : "unrecognized";
}

// 同一台机器上只允许一个 SDK 操作在跑：AvaSpec 驱动与 USB 句柄不是线程安全的。
class OwnerLock {
    HANDLE handle_ = nullptr;
public:
    OwnerLock() {
        handle_ = CreateMutexW(nullptr, FALSE, L"Local\\LibsAvaSpecSdkOwner");
        if (!handle_) throw std::runtime_error("cannot create the AvaSpec owner mutex");
        const auto status = WaitForSingleObject(handle_, 0);
        if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) {
            CloseHandle(handle_);
            handle_ = nullptr;
            throw std::runtime_error("another local AvaSpec operation is already running");
        }
    }
    ~OwnerLock() { if (handle_) { ReleaseMutex(handle_); CloseHandle(handle_); } }
    OwnerLock(const OwnerLock&) = delete;
    OwnerLock& operator=(const OwnerLock&) = delete;
};

#endif  // _WIN32
}  // namespace

SdkAbi sdk_abi() {
    SdkAbi abi;
#if defined(_WIN32)
    abi.identity_bytes = sizeof(AvsIdentity);
    abi.handle_bytes = sizeof(AvsHandle);
    abi.measurement_bytes = sizeof(MeasureConfig);
    abi.control_bytes = sizeof(Control);
    abi.trigger_offset = offsetof(MeasureConfig, trigger);
    abi.control_offset = offsetof(MeasureConfig, control);
#endif
    return abi;
}

#if defined(_WIN32)

struct Device {
    AvsHandle handle = kInvalidHandle;
    std::string serial;
    std::uint16_t pixels = 0;
    std::vector<double> wavelengths;
    MeasureConfig config{};
    bool touched = false;
    bool output_possible = false;   // 该通道是否被写入过脉冲参数
};

struct Spectrometer::Impl {
    OwnerLock owner;
    Api api;
    bool initialized = false;
    bool closed = false;
    std::vector<Device> devices;

    // 本会话是否已经持有这台设备（按序列号）。
    //
    // 为什么需要它：`AVS_GetList` 报的 status 是**设备当前被谁占用**，
    // 由本进程 AVS_Activate 过之后，重新枚举回来就会变成"被应用占用"。
    // 于是"这台设备是我自己开的"和"这台设备被别人占着"在状态字上长得一样。
    // 采集前若只按 status==1 判断可用性，就会**拒绝自己刚打开的通道**——
    // 现场表现是：出光成功、采集却每一帧都报 serial_not_available。
    bool owns(const std::string& serial) const {
        for (const Device& d : devices) if (d.serial == serial) return true;
        return false;
    }

    explicit Impl(const std::string& dll_path) : api(dll_path) {}

    void initialize() {
        if (initialized) return;
        // 只初始化 USB：不改网络配置，也不去打开任意 COM 口。
        const int rc = api.init(0);
        if (rc < 0)
            throw std::runtime_error("AVS_Init failed: SDK return " + std::to_string(rc));
        initialized = true;
    }

    std::vector<AvsIdentity> identities() {
        unsigned int required = 0;
        const int first = api.list(0, &required, nullptr);
        if (first < 0 && first != -9)
            throw std::runtime_error("AVS_GetList size query failed: " + std::to_string(first));
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (!required) return {};
            if (required % sizeof(AvsIdentity) != 0 || required > sizeof(AvsIdentity) * 256)
                throw std::runtime_error("AVS_GetList returned an invalid required size");
            std::vector<AvsIdentity> ids(required / sizeof(AvsIdentity));
            const auto capacity = required;
            const int count = api.list(capacity, &required, ids.data());
            if (required > capacity) continue;   // 设备列表在变，重试
            if (count < 0 || static_cast<std::size_t>(count) > ids.size())
                throw std::runtime_error("AVS_GetList failed or returned an inconsistent count: " +
                                         std::to_string(count));
            ids.resize(static_cast<std::size_t>(count));
            return ids;
        }
        throw std::runtime_error("the device list kept changing during enumeration");
    }

    // 激活一台设备并把它留在 impl_->devices 里，**幂等**。
    //
    // 同一序列号只激活一次：enumerate() 与 capture() 都会走到这里，若每次都
    // 追加一条，同一个 serial 就会在 devices 里出现两次、且拿到**同一个句柄**。
    // 那不是"多点几路"，是同一条路数了两遍：AVS_PollScan 第一次就把该通道的
    // pending 状态取走，第二条同名记录永远等不到，整次采集必然以
    // "timed out waiting for MASTER, SLAVE1, SLAVE2" 收场。
    // 复用句柄同时也让连续多帧采集不必每帧 activate/deactivate 一轮 USB 往返。
    std::size_t activate(const AvsIdentity& identity) {
        const std::string serial = bounded_string(identity.serial, sizeof(identity.serial));
        for (std::size_t i = 0; i < devices.size(); ++i)
            if (devices[i].serial == serial) return i;
        AvsIdentity copy = identity;
        Device d;
        d.serial = serial;
        d.handle = api.activate(&copy);
        if (d.handle == kInvalidHandle || d.handle < 0)
            throw std::runtime_error("AVS_Activate failed for " + d.serial);
        try {
            devices.push_back(std::move(d));
        } catch (...) {
            api.deactivate(devices.back().handle);
            devices.pop_back();
            throw;
        }
        auto& device = devices.back();
        int rc = api.pixels(device.handle, &device.pixels);
        if (rc != 0)
            throw std::runtime_error("AVS_GetNumPixels " + device.serial + " failed: " + std::to_string(rc));
        if (!device.pixels)
            throw std::runtime_error("zero-pixel device: " + device.serial);
        if (device.pixels > 16384)
            throw std::runtime_error("pixel count exceeds the adapter buffer limit: " + device.serial);
        device.wavelengths.resize(device.pixels);
        rc = api.wavelength(device.handle, device.wavelengths.data());
        if (rc != 0)
            throw std::runtime_error("AVS_GetLambda " + device.serial + " failed: " + std::to_string(rc));
        if (!std::all_of(device.wavelengths.begin(), device.wavelengths.end(),
                         [](double v) { return std::isfinite(v); }))
            throw std::runtime_error("non-finite wavelength calibration: " + device.serial);
        return devices.size() - 1;
    }

    // 尽力清理：先停可能产生脉冲的主通道，再停其余；然后清零瞬态配置并释放句柄。
    // 任何一步失败都记录下来，但继续清理其余设备，绝不中途返回。
    VoidResult close_all() {
        std::vector<std::string> errors;
        if (closed) return VoidResult::success(std::monostate{});
        closed = true;

        for (int output_pass = 1; output_pass >= 0; --output_pass)
            for (auto& d : devices)
                if (d.touched && d.output_possible == static_cast<bool>(output_pass)) {
                    const int rc = api.stop(d.handle);
                    if (rc) errors.push_back("AVS_StopMeasure " + d.serial + " -> " + std::to_string(rc));
                }
        for (auto& d : devices) {
            if (d.touched) {
                auto quiet = d.config;
                quiet.control = {};
                quiet.trigger = {};
                int rc = api.prepare(d.handle, &quiet);
                if (rc) errors.push_back("AVS_PrepareMeasure quiet " + d.serial + " -> " + std::to_string(rc));
                rc = api.sync(d.handle, 0);
                if (rc) errors.push_back("AVS_SetSyncMode off " + d.serial + " -> " + std::to_string(rc));
            }
            if (!api.deactivate(d.handle))
                errors.push_back("AVS_Deactivate " + d.serial + " failed");
            d.handle = kInvalidHandle;
        }
        if (initialized) {
            const int rc = api.done();
            if (rc) errors.push_back("AVS_Done -> " + std::to_string(rc));
            initialized = false;
        }
        if (!errors.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < errors.size(); ++i) {
                if (i) joined += "; ";
                joined += errors[i];
            }
            return Error::make(ErrorKind::internal, "cleanup_incomplete", joined);
        }
        return VoidResult::success(std::monostate{});
    }
};

Spectrometer::~Spectrometer() {
    if (impl_ && !impl_->closed) {
        try { impl_->close_all(); } catch (...) {}
    }
}

Result<std::unique_ptr<Spectrometer>> Spectrometer::open(const std::string& sdk_dll) {
    try {
        auto owned = std::unique_ptr<Spectrometer>(new Spectrometer());
        owned->impl_ = std::make_unique<Impl>(sdk_dll);
        return std::unique_ptr<Spectrometer>(owned.release());
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::transport, "sdk_load_failed", e.what());
    }
}

VoidResult Spectrometer::close() {
    if (!impl_) return VoidResult::success(std::monostate{});
    try { return impl_->close_all(); }
    catch (const std::exception& e) {
        return Error::make(ErrorKind::internal, "cleanup_failed", e.what());
    }
}

Result<std::vector<SpectrometerInfo>> Spectrometer::enumerate() {
    if (!impl_) return Error::make(ErrorKind::internal, "not_open", "the session is not open");
    try {
        impl_->initialize();
        std::vector<SpectrometerInfo> out;
        for (const auto& identity : impl_->identities()) {
            SpectrometerInfo info;
            info.serial = bounded_string(identity.serial, sizeof(identity.serial));
            info.friendly_name = bounded_string(identity.friendly_name, sizeof(identity.friendly_name));
            info.status = identity.status;
            info.status_name = status_name(identity.status);
            if (identity.status != 1) {          // 只有 usb_available 才激活
                out.push_back(std::move(info));
                continue;
            }
            auto& device = impl_->devices[impl_->activate(identity)];
            info.pixels = device.pixels;
            info.pixels_known = true;
            info.wavelength_min_nm = *std::min_element(device.wavelengths.begin(), device.wavelengths.end());
            info.wavelength_max_nm = *std::max_element(device.wavelengths.begin(), device.wavelengths.end());
            char fpga[16] = {}, firmware[16] = {}, library[16] = {};
            int board_type = 0;
            if (impl_->api.version(device.handle, fpga, firmware, library) == 0) {
                info.fpga_version = bounded_string(fpga, 16);
                info.firmware_version = bounded_string(firmware, 16);
                info.sdk_version = bounded_string(library, 16);
            }
            if (impl_->api.device_type(device.handle, &board_type) == 0) {
                info.board_type = board_type;
                info.board_type_known = true;
            }
            out.push_back(std::move(info));
        }
        return out;
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::transport, "enumerate_failed", e.what());
    }
}

Result<CaptureResult> Spectrometer::capture(const CaptureRequest& request) {
    if (!impl_) return Error::make(ErrorKind::internal, "not_open", "the session is not open");
    if (request.integration_ms < 0.002 || request.integration_ms > 60000.0 ||
        !std::isfinite(request.integration_ms))
        return Error::make(ErrorKind::invalid_argument, "bad_integration",
                           "integration_ms must be finite and in [0.002, 60000]");
    if (request.scans < 1 || request.scans > 100)
        return Error::make(ErrorKind::invalid_argument, "bad_scans", "scans must be 1..100");
    if (request.timeout_ms < 1)
        return Error::make(ErrorKind::invalid_argument, "bad_timeout", "timeout_ms must be positive");
    if (!request.master_serial.empty() &&
        std::find(request.serials.begin(), request.serials.end(), request.master_serial) == request.serials.end())
        return Error::make(ErrorKind::invalid_argument, "master_not_listed",
                           "master_serial must appear in the explicit serials list");

    CaptureResult result;
    try {
        impl_->initialize();
        const auto identities = impl_->identities();

        // 确定要激活哪些设备：显式列表优先，否则用当前所有可用设备。
        std::vector<const AvsIdentity*> selected;
        if (request.serials.empty()) {
            for (const auto& id : identities)
                if (id.status == 1) selected.push_back(&id);
        } else {
            for (const auto& serial : request.serials) {
                const auto it = std::find_if(identities.begin(), identities.end(),
                    [&](const AvsIdentity& v) { return bounded_string(v.serial, sizeof(v.serial)) == serial; });
                if (it == identities.end())
                    return Error::make(ErrorKind::device, "serial_not_found",
                                       "no attached spectrometer has serial '" + serial + "'");
                // 已经由本会话激活过的通道是**我们自己的**：状态字此时会显示
                // "被应用占用"，那正是我们自己，不能因此判定不可用。
                if (!impl_->owns(serial) && it->status != 1)
                    return Error::make(ErrorKind::device, "serial_not_available",
                                       "spectrometer '" + serial + "' is present but not available (status " +
                                           std::string(status_name(it->status)) + ")");
                selected.push_back(&*it);
            }
        }
        if (selected.empty())
            return Error::make(ErrorKind::device, "no_devices", "no available spectrometer was found");

        impl_->devices.reserve(impl_->devices.size() + selected.size());

        // 本次采集**只**动 selected 里的通道。句柄可以复用（见 activate 注释），
        // 但复用不等于"把上次留下的人也一起采了"：impl_->devices 里可能有
        // enumerate() 激活过、而这次请求没点的通道。那些通道留在设备上不采，
        // 也不该出现在 arm 顺序、采样上限或结果里。
        std::vector<std::size_t> active;   // selected 在 impl_->devices 中的下标
        active.reserve(selected.size());
        for (const auto* id : selected) {
            const std::size_t index = impl_->activate(*id);
            active.push_back(index);
            auto& device = impl_->devices[index];
            SpectrometerInfo info;
            info.serial = device.serial;
            info.pixels = device.pixels;
            info.pixels_known = true;
            info.wavelength_min_nm = *std::min_element(device.wavelengths.begin(), device.wavelengths.end());
            info.wavelength_max_nm = *std::max_element(device.wavelengths.begin(), device.wavelengths.end());
            result.devices.push_back(std::move(info));
        }

        // 结果体量上限，避免一次采集把内存吃光。批量采集应拆成多次调用。
        std::uint64_t total_samples = 0;
        for (std::size_t i : active) {
            const auto pixels = impl_->devices[i].pixels;
            total_samples += static_cast<std::uint64_t>(pixels) * request.scans;
        }
        if (total_samples > 4000000)
            return Error::make(ErrorKind::invalid_argument, "too_many_samples",
                               "the requested capture exceeds the four-million-sample in-memory limit; "
                               "split it into smaller runs");

        // 非主通道先 arm，主通道最后，给 SYNC 链留出建立时间。
        std::vector<std::size_t> order;
        for (std::size_t i : active)
            if (impl_->devices[i].serial != request.master_serial) order.push_back(i);
        for (std::size_t i : active)
            if (impl_->devices[i].serial == request.master_serial) order.push_back(i);

        // ── 触发方向：激光器触发光谱仪（AvaSoft 手册 §4.9 接法二）──────────
        //
        // 手册把 LIBS 分成两种接法：
        //   接法一：光谱仪发 TTL 出（pin 23）去点火激光器，积分延时与该输出同源。
        //   接法二：激光器/外部脉冲发生器控制时序，光谱仪跑**外触发模式**，
        //           从 pin 6 收 TTL 输入，收到后再等积分延时才开始积分。
        //
        // 本项目走**接法二**。所以这里绝不能设 laser_width：
        // 那会让光谱仪反过来去点火激光器，方向与现场接线相反，
        // 现场表现为"激光永远不出光，光谱仪一直等触发直到超时"。
        const bool laser_fires = request.wait_for_laser_trigger;

        // 积分延时：调用方给纳秒，这里按手册换算成 FPGA 计数。
        std::uint32_t integration_delay_cycles = 0;
        if (!FpgaTime::nanos_to_cycles(request.integration_delay_ns, &integration_delay_cycles)) {
            return Error::make(ErrorKind::invalid_argument, "bad_integration_delay",
                               "integration_delay_ns must be a finite non-negative number; "
                               "negative delays are only valid when the spectrometer drives the laser");
        }
        if (request.integration_delay_ns > 0.0 && !laser_fires) {
            // 自由触发模式下没有"触发"可等，延时会变成采集开始前的空等。
            // 不静默忽略：直接报出来，避免现场白等一场。
            return Error::make(ErrorKind::invalid_argument, "delay_without_trigger",
                               "integration_delay_ns is set but wait_for_laser_trigger is false; "
                               "a delay only makes sense when an external laser trigger starts the integration");
        }

        for (auto i : order) {
            auto& d = impl_->devices[i];
            const bool master = d.serial == request.master_serial;
            d.config = MeasureConfig{};
            d.config.stop_pixel = static_cast<std::uint16_t>(d.pixels - 1);
            d.config.integration_ms = static_cast<float>(request.integration_ms);
            d.config.integration_delay = integration_delay_cycles;
            d.config.averages = 1;
            d.config.dark.forget_percentage = 100;

            // 触发模式：外触发(1) 等待激光器 TTL 输入；自由触发(0) 随时可采。
            //
            // **只由调用方那句话决定**，不再拿"配了 master"去兜底。
            // 曾经的写法是 `laser_fires ? 1 : (has_master ? 1 : 0)`：本意是
            // 多通道 SYNC 时主通道也得跟着外触发，但它的副作用是 ——
            // 只要站点配置里写了 master_serial，**任何**"不要等触发"的采集
            // 都会被顶成外触发。实时预览正好是这个组合（配置里有 master，
            // 预览走自由触发），于是四个通道一起等一个永远不来的 TTL，
            // 每帧 capture_timeout：现场表现就是"点了开始检测，一条谱都没有"。
            // 调用方说"不等 trigger"，就该真的不等。
            d.config.trigger.mode = laser_fires ? 1 : 0;
            if (request.synchronized && !master) { d.config.trigger.mode = 1; d.config.trigger.source = 1; }

            // 走接法二时**不设** laser_width/laser_delay：
            // 那两个字段是"光谱仪发 TTL 出去点火激光器"用的，
            // 在本方向上必须为 0，否则方向反了。
            d.config.control.laser_width = 0;
            d.config.control.laser_delay = 0;
            d.output_possible = false;
            d.touched = true;

            // 下发前先把设备上可能残留的上一次测量停掉。
            //
            // 为什么必须无条件清：超时（capture_timeout）那条路径会直接 return，
            // 被 arm 过的通道就留在 pending 状态；如果残留是**上一个进程**留下的
            // （守护进程被强杀，close_all 根本没跑），进程内清理同样救不了。
            // 残留的表现是下一次 AVS_PrepareMeasure 返回 -5 = ERR_OPERATION_PENDING，
            // 采集从那一刻起永远起不来，而且报的是"准备失败"，跟真正的原因隔着好几层。
            // 这是清场动作、不是断言，所以返回码不参与判定——紧接着的 prepare 会说话。
            (void)impl_->api.stop(d.handle);

            int rc = impl_->api.sync(d.handle, request.synchronized && master ? 1 : 0);
            if (rc != 0)
                return Error::make(ErrorKind::device, "set_sync_failed",
                                   "AVS_SetSyncMode " + d.serial + " failed: " + std::to_string(rc));
            rc = impl_->api.prepare(d.handle, &d.config);
            if (rc != 0)
                return Error::make(ErrorKind::device, "prepare_failed",
                                   "AVS_PrepareMeasure " + d.serial + " failed: " + std::to_string(rc));
        }

        for (std::uint32_t shot = 0; shot < request.scans; ++shot) {
            ScanGroup group;
            group.shot_id = shot + 1;
            group.hardware_synchronized = request.synchronized;
            const auto deadline = Clock::now() + std::chrono::milliseconds(request.timeout_ms);

            for (auto i : order) {
                if (Clock::now() >= deadline)
                    return Error::make(ErrorKind::timeout, "arm_deadline",
                                       "the deadline passed before every channel was armed");
                const int rc = impl_->api.measure(impl_->devices[i].handle, nullptr, 1);
                if (rc != 0)
                    return Error::make(ErrorKind::device, "measure_failed",
                                       "AVS_Measure " + impl_->devices[i].serial + " failed: " + std::to_string(rc));
                group.arm_order.push_back(impl_->devices[i].serial);
            }

            // 同样是"只等本次选中的通道"：ready 的长度取 active，索引是 impl_->devices 的下标。
            std::vector<bool> ready(active.size(), false);
            std::size_t remaining = ready.size();
            while (remaining) {
                if (Clock::now() >= deadline) {
                    // 超时必须先把已 arm 的通道停掉再返回。否则它们留在 pending，
                    // 下一次采集的第一条 PrepareMeasure 就会拿到 -5。
                    for (auto i : order) (void)impl_->api.stop(impl_->devices[i].handle);
                    std::string missing;
                    for (std::size_t k = 0; k < ready.size(); ++k)
                        if (!ready[k]) { if (!missing.empty()) missing += ", "; missing += impl_->devices[active[k]].serial; }
                    result.groups.push_back(std::move(group));
                    return Error::make(ErrorKind::timeout, "capture_timeout",
                                       "timed out waiting for " + missing + "; the partial group is kept "
                                       "in the result and no shot is retried");
                }
                for (std::size_t k = 0; k < active.size(); ++k) {
                    if (ready[k]) continue;
                    auto& d = impl_->devices[active[k]];
                    const int rc = impl_->api.poll(d.handle);
                    if (rc < 0)
                        return Error::make(ErrorKind::device, "poll_failed",
                                           "AVS_PollScan " + d.serial + " failed: " + std::to_string(rc));
                    if (rc == 0) continue;
                    if (rc != 1)
                        return Error::make(ErrorKind::device, "poll_unexpected",
                                           "AVS_PollScan " + d.serial + " returned " + std::to_string(rc));
                    std::vector<double> counts(d.pixels);
                    unsigned int ticks = 0;
                    const int src = impl_->api.scope(d.handle, &ticks, counts.data());
                    if (src != 0)
                        return Error::make(ErrorKind::device, "scope_failed",
                                           "AVS_GetScopeData " + d.serial + " failed: " + std::to_string(src));
                    if (!std::all_of(counts.begin(), counts.end(), [](double v) { return std::isfinite(v); }))
                        return Error::make(ErrorKind::device, "non_finite_spectrum",
                                           "the spectrum from " + d.serial + " contains non-finite values");
                    Spectrum spectrum;
                    spectrum.serial = d.serial;
                    spectrum.pixels = d.pixels;
                    spectrum.device_time_ticks = ticks;
                    spectrum.wavelength_nm = d.wavelengths;
                    spectrum.counts = std::move(counts);
                    group.spectra.push_back(std::move(spectrum));
                    ready[k] = true;
                    --remaining;
                }
                if (remaining) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            group.complete = true;
            result.groups.push_back(std::move(group));
        }
        return result;
    } catch (const std::exception& e) {
        return Error::make(ErrorKind::transport, "capture_failed", e.what());
    }
}

Result<CaptureResult> Spectrometer::stream(
        const CaptureRequest& request,
        const std::function<VoidResult(const ScanGroup&)>& on_group) {
    const auto result = capture(request);
    if (!result) return result.error();
    for (const auto& group : result.value().groups) {
        const VoidResult consumed = on_group(group);
        if (!consumed) return consumed.error();
    }
    return result;
}

#else  // !_WIN32

struct Spectrometer::Impl {};
Spectrometer::~Spectrometer() = default;

Result<std::unique_ptr<Spectrometer>> Spectrometer::open(const std::string& sdk_dll) {
    (void)sdk_dll;
    return Error::make(ErrorKind::unsupported, "no_sdk",
                       "the AvaSpec SDK adapter is only implemented on Windows in this build");
}
Result<std::vector<SpectrometerInfo>> Spectrometer::enumerate() {
    return Error::make(ErrorKind::unsupported, "no_sdk", "not available on this platform");
}
Result<CaptureResult> Spectrometer::capture(const CaptureRequest& request) {
    (void)request;
    return Error::make(ErrorKind::unsupported, "no_sdk", "not available on this platform");
}
Result<CaptureResult> Spectrometer::stream(
        const CaptureRequest& request,
        const std::function<VoidResult(const ScanGroup&)>& on_group) {
    (void)request;
    (void)on_group;
    return Error::make(ErrorKind::unsupported, "no_sdk", "not available on this platform");
}
VoidResult Spectrometer::close() { return VoidResult::success(std::monostate{}); }

#endif  // _WIN32

}  // namespace libs::devices

