// AvaSpec SDK 的字节级测试替身：独立实现，故意不做任何 USB/网络/串口 I/O。
//
// 它是测试夹具，不是厂家 avaspecx64.dll 的替代品，也不代表任何真实光谱仪的
// 型号、像素数或标定。Spectrometer 驱动只按函数名绑定导出，因此这里只需要
// 提供驱动真正用到的那几个 AVS_* 导出。
#include <cstring>

namespace {
int mode = 0;   // 0=正常 1=PrepareMeasure 失败 2=PollScan 失败 3=GetScopeData 失败 4=永不 ready（测超时）
int counters[32] = {};        // 0=Prepare 次数 1=Measure 次数 2=Stop 次数 3=Deactivate 次数 4=Done
// mode==5：设备被 AVS_Activate 之后，AVS_GetList 报回来的 status 变成 2
// （"被应用占用"）—— 这是真实 SDK 的行为，也是"采集拒绝自己刚开的通道"那个
// 真机缺陷的复现条件。
bool activated[3] = {};
// mode==6：模拟"上一轮测量残留在设备上"。AVS_Measure 置位、AVS_StopMeasure 清零；
// 残留期间 AVS_PrepareMeasure 返回 -5 = ERR_OPERATION_PENDING。
// 真机现象：强杀守护进程后设备停在等触发状态，下一次采集报 prepare_failed(-5)。
bool pending[3] = {};
// 最近一次 AVS_SetSyncMode 收到的值，按通道留档。
// 多通道同步的接线约定全在这里：厂家 qtdemo 把**主通道**设成 1（"device i is
// the Master"），从通道保持 0、改用 Trigger.Source=1（SYNC input）收触发。
// 写反了现场表现是"从通道永远等不到触发"。
unsigned char sync_mode[3] = {};
unsigned char trigger[3][3] = {};
bool armed[3] = {};
const char* serials[3] = {"MASTER", "SLAVE1", "SLAVE2"};
const unsigned short pixel_counts[3] = {4, 7, 9};

// 最近一次 AVS_PrepareMeasure 收到的配置字节，按通道留档。
// 用来断言"触发方向没写反"：外触发模式下 laser_width/laser_delay 必须为 0。
// 偏移照抄驱动自己校验过的那套 ABI（tests 里 sdk_abi_matches_vendor_header 断言）：
//   stop_pixel   u16 @ 0    integration u32 @ 2   integ_delay u32 @ 6
//   averages     u16 @ 10   dark        4 字节 @ 12
//   trigger      3 字节 @ 22
//   control      16 字节 @ 25
//       strobe u16 @ 25 / laser_delay u32 @ 27 / laser_width u32 @ 31
//       wavelength f32 @ 35 / storeToRam u16 @ 39   -> 整份 41 字节
constexpr int kConfigBytes = 64;
static unsigned char last_config[32][kConfigBytes] = {};
// 驱动按 sizeof(AvsIdentity)==75 的定长数组读取，所以这里也必须是 75 字节步长。
constexpr int kIdentityStride = 75;
template <class T> T read_at(const unsigned char* p, int offset) {
    T value{};
    std::memcpy(&value, p + offset, sizeof(T));
    return value;
}
}  // namespace

#define EXPORT extern "C" __declspec(dllexport)

EXPORT void FakeReset(int failure_mode) {
    mode = failure_mode;
    std::memset(counters, 0, sizeof(counters));
    std::memset(activated, 0, sizeof(activated));
    std::memset(pending, 0, sizeof(pending));
    std::memset(sync_mode, 0, sizeof(sync_mode));
    std::memset(trigger, 0, sizeof(trigger));
    std::memset(armed, 0, sizeof(armed));
    std::memset(last_config, 0, sizeof(last_config));
}
EXPORT int FakeCounter(int i) { return (i >= 0 && i < 32) ? counters[i] : -1; }
EXPORT void FakeSetMode(int value) { mode = value; }

EXPORT void FakeLastConfig(int channel, unsigned char* out) {
    if (channel < 0 || channel >= 32 || !out) return;
    std::memcpy(out, last_config[channel], kConfigBytes);
}
EXPORT unsigned char FakeConfigByte(int channel, int offset) {
    if (channel < 0 || channel >= 32 || offset < 0 || offset >= kConfigBytes) return 0;
    return last_config[channel][offset];
}
EXPORT unsigned int FakeConfigU16(int channel, int offset) { return read_at<unsigned short>(last_config[channel], offset); }
EXPORT unsigned int FakeConfigU32(int channel, int offset) { return read_at<unsigned int>(last_config[channel], offset); }
EXPORT int FakeMode() { return mode; }
EXPORT int FakeSyncMode(int channel) {
    return (channel >= 0 && channel < 3) ? sync_mode[channel] : -1;
}

EXPORT int __stdcall AVS_Init(short) { return 3; }
EXPORT int __stdcall AVS_Done() { ++counters[4]; return 0; }

// capacity==0 是"问需要多大"的探测调用：只填 required，不写 data。
EXPORT int __stdcall AVS_GetList(unsigned int capacity, unsigned int* required, unsigned char* data) {
    *required = 3 * kIdentityStride;
    if (capacity < 3 * kIdentityStride) return -9;
    if (!data) return -9;
    std::memset(data, 0, 3 * kIdentityStride);
    for (int i = 0; i < 3; ++i) {
        std::strcpy(reinterpret_cast<char*>(data + i * kIdentityStride), serials[i]);
        std::strcpy(reinterpret_cast<char*>(data + i * kIdentityStride + 10), "Fake AvaSpec");
    // status: 1 = usb_available，2 = usb_in_use_by_application
    data[i * kIdentityStride + 74] = (mode == 5 && activated[i]) ? 2 : 1;
    }
    return 3;
}

EXPORT int __stdcall AVS_Activate(unsigned char* identity) {
    for (int i = 0; i < 3; ++i)
        if (std::strcmp(reinterpret_cast<char*>(identity), serials[i]) == 0) { activated[i] = true; return i + 1; }
    return 1000;   // kInvalidHandle
}
EXPORT bool __stdcall AVS_Deactivate(int h) {
    ++counters[3];
    if (h >= 1 && h <= 3) activated[h - 1] = false;
    return true;
}
EXPORT int __stdcall AVS_GetNumPixels(int h, unsigned short* n) {
    if (h < 1 || h > 3) return -1;
    *n = pixel_counts[h - 1];
    return 0;
}
EXPORT int __stdcall AVS_GetLambda(int h, double* values) {
    if (h < 1 || h > 3) return -1;
    for (int i = 0; i < pixel_counts[h - 1]; ++i) values[i] = 200.0 + h * 100.0 + i;
    return 0;
}
EXPORT int __stdcall AVS_GetVersionInfo(int, char* fpga, char* firmware, char* library) {
    std::strcpy(fpga, "fake-fpga");
    std::strcpy(firmware, "fake-fw");
    std::strcpy(library, "9.14.0.0");
    return 0;
}
EXPORT int __stdcall AVS_GetDeviceType(int, int* t) { *t = 1; return 0; }
EXPORT int __stdcall AVS_SetSyncMode(int h, unsigned char enable) {
    ++counters[7];
    if (h >= 1 && h <= 3) sync_mode[h - 1] = enable;
    return 0;
}

EXPORT int __stdcall AVS_PrepareMeasure(int h, unsigned char* p) {
    if (h < 1 || h > 3) return -1;
    ++counters[0];
    std::memcpy(trigger[h - 1], p + 22, 3);
    // 留档整份配置，供测试断言触发模式与积分延时。
    // Vendor measurement ABI is 41 bytes; the larger inspection buffer is ours.
    std::memset(last_config[h - 1], 0, kConfigBytes);
    std::memcpy(last_config[h - 1], p, 41);
    if (mode == 1 && h == 2) return -1;
    if (mode == 6 && pending[h - 1]) return -5;   // ERR_OPERATION_PENDING
    return 0;
}
EXPORT int __stdcall AVS_Measure(int h, void*, short n) {
    if (h < 1 || h > 3) return -1;
    if (n != 1) ++counters[6];
    armed[h - 1] = true;
    pending[h - 1] = true;
    ++counters[1];
    return 0;
}
EXPORT int __stdcall AVS_PollScan(int h) {
    if (mode == 8 && h == 3) return -3;  // ERR_DEVICE_NOT_FOUND during acquisition.
    if (h < 1 || h > 3) return -1;
    if (mode == 2 && h == 2) return -4;
    if (mode == 7 && counters[1] > 3) return 0;
    if (mode == 4) return 0;                 // 永远不 ready
    if (!armed[h - 1]) return 0;
    return 1;                                // armed 即视为可取
}
EXPORT int __stdcall AVS_GetScopeData(int h, unsigned int* tick, double* values) {
    if (h < 1 || h > 3) return -1;
    if (mode == 3 && h == 2) return -8;
    *tick = static_cast<unsigned int>(12300 + h);
    for (int i = 0; i < pixel_counts[h - 1]; ++i) values[i] = h * 10.0 + i;
    armed[h - 1] = false;
    return 0;
}
EXPORT int __stdcall AVS_StopMeasure(int h) {
    if (h >= 1 && h <= 3) { ++counters[2]; armed[h - 1] = false; pending[h - 1] = false; }
    return 0;
}

#undef EXPORT

