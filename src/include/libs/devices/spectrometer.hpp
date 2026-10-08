#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "libs/error.hpp"

namespace libs::devices {

// AvaSpec 光谱仪：动态加载厂商 DLL，USB 枚举与采集。
//
// 本驱动只做"把 SDK 调用翻译成结构化数据"。它不判断激光状态、不做触发方案
// 校验、不替你确认接线——多通道同步与外触发属于上层业务。

struct SpectrometerInfo {
    std::string serial;
    std::string friendly_name;
    std::uint8_t status = 0;
    std::string status_name;
    std::uint16_t pixels = 0;
    bool pixels_known = false;
    double wavelength_min_nm = 0.0;
    double wavelength_max_nm = 0.0;
    std::string fpga_version;
    std::string firmware_version;
    std::string sdk_version;
    int board_type = 0;
    bool board_type_known = false;
};

// ── FPGA 时钟换算（依据 Avantes AvaSoft 手册 §4.9 与 SDK 示例）─────────────
//
// 光谱仪用 48 MHz FPGA 时钟给延时参数定时，SDK 里的单位是"FPGA 计数"，
// 不是纳秒也不是微秒。换算关系逐字取自 SDK 示例 qtdemo.cpp：
//
//     积分延时（正向，qtdemo.cpp:462）  计数 = 6.0 * (ns + 20.84) / 125.0
//     积分延时（反向，qtdemo.cpp:975）  ns   = 125.0 * (计数 - 1) / 6.0
//     激光延时（qtdemo.cpp:479/992）    计数 = 6.0 * ns / 125.0，无偏置
//     激光宽度（qtdemo.cpp:481/995）    同上，无偏置
//
// 125.0/6.0 = 20.833 ns，即手册说的"21 纳秒步进"。
//
// 注意积分延时**有**偏置、激光延时**没有**，这不是笔误：反向换算里
// 积分延时减 1 个计数而激光延时不减，两个方向必须成对使用，不能互换。
// 本项目只用到积分延时（激光器触发光谱仪，接法二），不碰激光延时/宽度——
// 那两个是"光谱仪发 TTL 出去点火激光器"用的，方向反了现场就永远不出光。
//
// 手册 §4.9 同时明确：LIBS 的积分时间必须在激光脉冲**之后**开始，
// 否则测到的是激光本身的散射光而不是等离子体辐射。
// 因此激光触发光谱仪时，积分延时必须为正。
struct FpgaTime {
    // 偏移量常数，来自 qtdemo.cpp 的 +20.84 / (计数-1) 这对正反公式。
    static constexpr double kIntegrationDelayOffsetNs = 20.84;

    // 纳秒 -> FPGA 计数（积分延时专用，含 +20.84 ns 偏置）。
    // 负延时不可用时返回 false：负延时只在"光谱仪驱动激光器"（接法一）下
    // 才有意义，而那时序由光谱仪决定，不由激光器决定。
    static bool nanos_to_cycles(double nanos, std::uint32_t* out) {
        if (out == nullptr) return false;
        // 0 纳秒是合法且常见的"收到触发立刻积分"，此时按公式算出偏置本身。
        if (!(nanos >= 0.0)) return false;      // 负数与 NaN 一律拒绝
        const double shifted = nanos + kIntegrationDelayOffsetNs;
        const double cycles = 6.0 * shifted / 125.0;
        if (cycles > 4294967295.0) return false; // 超出 uint32
        *out = static_cast<std::uint32_t>(cycles);
        return true;
    }

    // 纳秒 -> FPGA 计数（光谱仪 Pin 23 输出脉冲专用）。AvaSoft SDK
    // 对 laser delay/width 使用无偏置公式；不要复用上面的积分延时公式，
    // 否则会额外加入 20.84 ns，导致外部激光器触发时序偏移一个计数。
    static bool laser_nanos_to_cycles(double nanos, std::uint32_t* out) {
        if (out == nullptr || !(nanos >= 0.0)) return false;
        const double cycles = 6.0 * nanos / 125.0;
        if (cycles > 4294967295.0) return false;
        *out = static_cast<std::uint32_t>(cycles);
        return true;
    }

    // FPGA 计数 -> 纳秒（积分延时专用，含 -1 计数偏置）。仅用于回显与日志，
    // 不参与下发。计数为 0 时返回负值，说明这一栏本来就没有有效读数。
    static double cycles_to_nanos(std::uint32_t cycles) {
        return 125.0 * (static_cast<double>(cycles) - 1.0) / 6.0;
    }
};
// 单次采集的参数。全部是显式给出的，没有隐式默认值。
struct CaptureRequest {
    std::vector<std::string> serials;   // 为空表示"当前可用的全部设备"
    std::string master_serial;          // 可选：指定主通道（外触发源 / 出光设备）
    bool synchronized = false;          // 多通道 SYNC 同步
    double integration_ms = 10.0;
    std::uint32_t integration_delay_cycles = 0;   // 原始 FPGA 计数，非微秒
    std::uint32_t timeout_ms = 3000;
    std::uint32_t scans = 1;            // 本驱动做有界多次采集
    std::uint32_t laser_delay_cycles = 0;
    std::uint32_t laser_width_cycles = 0;
    unsigned timeout_ms_override = 0;   // 0 表示用 CaptureRequest::timeout_ms

    // ── 激光触发光谱仪（AvaSoft 手册 §4.9 的第二种硬件接法）───────────────
    //
    // 手册原文：激光器或外部脉冲发生器控制时序时，光谱仪跑**外触发模式**，
    // 从 pin 6 收 TTL 输入；收到脉冲后**再等 integration_delay** 才开始积分。
    // 抖动 21 ns；不同型号有最小积分延时（AvaSpec-3648-USB2 为 0.28 us，
    // AvaSpec-2048-USB2 为 1.28 us）。
    //
    // 默认 false = 自由触发，采集随时可开始，不需要外部信号。
    // 置 true 后没有外触发信号就永远等不到数据，采集会超时。
    bool wait_for_laser_trigger = false;

    // 光谱仪通过 HD-26 Pin 23 LASER OUT 输出 TTL 点火激光器。
    // 该方向使用软件/自由启动的 AVS_Measure；它不能与
    // wait_for_laser_trigger 同时打开，否则会把同一条触发链配置成互相等待。
    bool spectrometer_triggers_laser = false;

    // 收到触发后延迟多久开始积分。**纳秒**，非 FPGA 计数。
    // 典型 LIBS 场景是"等激光烧蚀产生的等离子体冷却发光"，
    // 需要比激光脉冲本身（几十 us）更长，所以给 0 表示"立刻积分"是可以的，
    // 但通常要配一个几十到几百微秒的值。
    double integration_delay_ns = 0.0;
};

struct Spectrum {
    std::string serial;
    std::uint16_t pixels = 0;
    unsigned device_time_ticks = 0;
    std::vector<double> wavelength_nm;
    std::vector<double> counts;
};

struct ScanGroup {
    std::uint32_t shot_id = 1;
    bool complete = false;
    bool hardware_synchronized = false;
    std::vector<Spectrum> spectra;
    std::vector<std::string> arm_order;   // 实际 arm 顺序，非主通道优先
};

struct CaptureResult {
    std::vector<SpectrometerInfo> devices;
    std::vector<ScanGroup> groups;

    // 按 request.scans 期望收到几组 / 实际收到几组。
    // 少收 = 出光窗口里触发没来齐，**不是失败**：已经采到的谱照常交付，
    // 由上层如实标注"期望 N 发 / 实收 M 张"。只有一组都没收到才是失败。
    std::uint32_t expected_groups = 0;
    std::uint32_t received_groups = 0;
};

// AvaSpec 会话。
//
// 生命周期：open() → 使用 → close()。close() 必须在析构前显式调用，
// 它会尽力停止测量、清空脉冲配置并释放句柄；析构函数是兜底，不是正常路径。
class Spectrometer {
public:
    // sdk_dll 必须是绝对路径的 x64 avaspecx64.dll。
    static Result<std::unique_ptr<Spectrometer>> open(const std::string& sdk_dll);
    ~Spectrometer();

    Spectrometer(const Spectrometer&) = delete;
    Spectrometer& operator=(const Spectrometer&) = delete;

    // 只读枚举。不 prepare、不 measure、不改持久参数。
    Result<std::vector<SpectrometerInfo>> enumerate();

    // 有界采集。设备异常帧与超时都通过 Error 返回；已取到的部分结果保留在 error.raw 之外，
    // 调用方若需要部分结果应检查 groups。
    Result<CaptureResult> capture(const CaptureRequest& request);

    // 释放全部句柄并执行清理。重复调用是安全的。
    VoidResult close();

private:
    Spectrometer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ABI 自检信息，便于在日志里确认结构体布局与厂商头文件一致。
struct SdkAbi {
    std::size_t identity_bytes = 0;
    std::size_t handle_bytes = 0;
    std::size_t measurement_bytes = 0;
    std::size_t control_bytes = 0;
    std::size_t trigger_offset = 0;
    std::size_t control_offset = 0;
};
SdkAbi sdk_abi();

}  // namespace libs::devices
