# LIBS 设备控制库

LIBS（激光诱导荧光光谱）现场检测系统的**底层设备控制库**。本仓库只提供一件事：把激光器、测距仪、报警灯、空调、光谱仪这五类硬件的通信细节封装干净，**不含任何业务逻辑与安全策略**。

> 当前状态：底层库已完成重构并全部离线测试通过。上层业务逻辑（触发方案、多通道同步、激光安全策略、联锁）**尚未建立**，将由你作为独立模块添加。

---

## 这个库做什么、不做什么

**做：**

- 把原始字节翻译成结构化数据（距离、温度、灯态、波长、counts）
- 帧的构造、校验、从串口噪声中定位完整帧
- 串口收发与超时管理
- AvaSpec 厂商 SDK 的动态加载与适配

**不做（有意为之）：**

- 不判断"现在允不允许激光出光"
- 不做操作员确认、物理接线确认、就绪门禁
- 不做联锁 / 急停的策略判断
- 不做触发方案校验与多通道同步编排
- 不含配置文件、JSON 解析、命令行入口

这些**全部属于上层业务**。设备驱动只负责"你让我发这些字节，我发出去并把结果如实带回"，判断权不在这一层。设备自己上报的安全状态位（如 interlock、emergency_stop）会**照常解码上报**，但绝不参与任何门禁——判断留给上层。

---

## 目录结构

```text
src/                          ← 新源码（当前唯一在用的代码）
  include/libs/
    error.hpp                 统一 Result<T> / Error / 错误分类
    hex.hpp                   字节与十六进制文本
    protocols/                第 3 层：纯协议，无 I/O、无状态
    transport/serial.hpp      第 2 层：串口收发与超时
    devices/                  第 1 层：设备驱动与 SDK 适配
  src/                        实现
  tests/                      455 项离线断言
  CMakeLists.txt / build.ps1  构建入口
  .gitignore

cpp/                          ← 旧实现，原样保留作参考，不参与新构建
device-references/            ← 厂家协议手册与 SDK，驱动的依据来源
docs/refactor-report.md       ← 完整重构报告
```

---

## 依赖方向

```text
        你的业务逻辑（待建立）
                 ▲
                 │ 只依赖这一个目标
        ┌────────┴─────────┐
        │   libs_devices   │  设备驱动 / SDK
        └────────▲─────────┘
        ┌────────┴─────────┐
        │  libs_transport  │  传输：串口、超时、端口枚举
        └────────▲─────────┘
        ┌────────┴─────────┐
        │  libs_protocols  │  纯协议：字节 ↔ 值
        └──────────────────┘
```

**严格单向，无反向引用。** 这不是文档约定，而是构建约束：三层是三个独立静态库，反向依赖会在编译或链接期直接失败。

想加业务逻辑时，只依赖聚合目标 `libs` 即可，不需要改动 `src/` 里任何一行代码。

---

## 快速开始

### 构建与测试

```powershell
cd src
pwsh -File build.ps1
```

预期输出：

```text
OK tests/test_protocols.cpp:    120 checks passed
OK tests/test_devices.cpp:       55 checks passed
OK tests/test_air_conditioner_protocol.cpp: 114 checks passed
OK tests/test_air_conditioner_device.cpp:   61 checks passed
OK tests/test_spectrometer.cpp: 105 checks passed
==> all tests passed
```

也支持 CMake + CTest：

```powershell
cd src
cmake -S . -B build/obj -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/obj
ctest --test-dir build/obj --output-on-failure
```

> 生成器用 Ninja 或 MinGW Makefiles 都行。两者都已在 Windows x64 + MinGW GCC 13.1 上实测通过。

### 最小示例

```cpp
#include <libs/devices/rangefinder.hpp>
#include <libs/protocols/rangefinder.hpp>
#include <libs/transport/serial.hpp>

#include <cstdio>

int main() {
    // 打开串口
    auto link = libs::transport::open_serial("COM3", 9600);
    if (!link) {
        // 错误统一是 libs::Error，带 kind / code / message / raw
        std::fprintf(stderr, "%s / %s: %s\n",
                     libs::to_string(link.error().kind),
                     link.error().code.c_str(),
                     link.error().message.c_str());
        return 1;
    }

    // 建驱动：驱动依赖 Link 接口，不需要知道背后是真是串口
    libs::devices::Rangefinder finder(*link.value());

    // 读一次：地址 1，从距离寄存器起读 3 个
    auto read = finder.read(0x01, libs::rangefinder::reg::kDistanceZeroOnOutOfRange, 3);
    if (!read) {
        std::fprintf(stderr, "read failed: %s\n", read.error().message.c_str());
        return 1;
    }

    for (const auto& reg : read.value().reading.registers) {
        std::printf("%-22s raw=%-6u %s %s\n", reg.name.c_str(), unsigned(reg.raw),
                    reg.unit.c_str(), reg.valid ? "" : "(sentinel)");
    }
    return 0;
}
```

上面的示例是**逐字编译验证过的**。注意驱动接收的是 `Link&`；因为 `Result` 的 `operator*` 是显式的，取值要写 `*link.value()`。寄存器语义由 `libs::rangefinder::reg` 提供。

所有可能失败的调用都返回 `Result<T>`：成功带值，失败带 `Error`，**不使用异常做控制流**。只有编程错误（参数越界、索引非法）才抛异常。

---

## 测试

455 项断言，5 个测试程序，**全部离线运行**——不开真实 COM 口、不加载真实 AvaSpec DLL、不发光。

| 测试 | 断言数 | 覆盖 |
| --- | ---: | --- |
| `test_protocols` | 120 | CRC、帧构造/解析、噪声定位、寄存器语义、型号表 |
| `test_devices` | 55 | 三类串口设备，用假传输验证收发与错误传播 |
| `test_air_conditioner_protocol` | 114 | 空调 FC01/03/05/06 编解码，全部用厂商手册 §7 实测帧做向量 |
| `test_air_conditioner_device` | 61 | 空调驱动收发、范围校验、错误传播，用假传输 |
| `test_spectrometer` | 105 | SDK 加载、ABI、枚举、采集顺序、失败注入、超时、幂等清理 |

光谱仪测试用一个自制的替身 DLL（`tests/fake_avaspec_sdk.cpp`），**它是测试夹具，不是厂家 DLL 的替代品**，也不代表任何真实光谱仪的型号或像素数。

---

## 平台要求

- Windows x64
- C++17
- MinGW GCC 13+ 或 MSVC
- 编译零警告（`-Wall -Wextra -Wpedantic`）

无第三方依赖。旧实现里的 `nlohmann/json` 已移除。

---

## 硬件资料

`device-references/` 存放厂家协议手册与 SDK，是所有驱动代码的依据来源：

```text
laser/          风冷激光器 RS232 协议
rangefinder/    TOF101 Modbus 协议与寄存器说明
serial-devices/ 三色灯协议、串口测试工具、USB/485 驱动
spectrometer/   AvaSpec 9.14.0.0 SDK（DLL、示例、手册）
air-conditioning/ 空调协议 V2（RS485 / Modbus RTU 变体）——已接入，见下节
```

**设备协议不靠类比推断。** 重构过程中，报警灯曾被误当作标准 Modbus FC01，实际是厂商私有 8 字节格式（寄存器会回显），与测距仪恰好相反——这类细节只有以厂商文档为准才不会出错。详见重构报告 §7。
---

## 空调（RS485 / Modbus RTU 变体）

空调是唯一一个"协议不是标准 Modbus"的设备：它沿用 Modbus RTU 帧格式，但寄存器号在手册里是 5 位的**文档地址**（10001/30001/40001），报文里出现的是**从 0 开始的偏移量**。换算基准由功能码决定，本库对外一律用文档地址，偏移换算封装在协议层内部。

| 功能码 | 含义 | 报文里的起始偏移 | 数量 |
| --- | --- | --- | --- |
| `0x01` | 读运行状态位 | `文档地址 − 10001` | 每个寄存器 1 位，bit0 在前 |
| `0x03` | 读运行参数 | `文档地址 − 30001` | 每参数 2 字节高字节在前，单次最多 50 |
| `0x05` | 写状态位（开关机） | `文档地址 − 10001` ※ | 1 个线圈值 |
| `0x06` | 写运行参数 | `文档地址 − 40001` | 1 个 16 位值 |

※ **厂商文档唯一的笔误。** 手册 §4.5 注 1 写 `sReg = 寄存器号 − 00001`，但厂商自己的样本 §7.11/§7.12 写开关机用的偏移是 `0x0029 = 41 = 10042 − 10001`。按 `00001` 换算会得到 10041，设备不会认。本实现**以实测样本为准**，并在测试里用一条专门的用例把这个基准钉死。

其余部分（§7.8/§7.9/§7.10 散文的位序说明、`0x0000` 关 / `0xFF00` 开的取值）经逐条核对与寄存器表**完全自洽**，无需任何纠偏。

```cpp
#include <libs/devices/air_conditioner.hpp>
#include <libs/transport/serial.hpp>

using namespace libs;
using namespace libs::air_conditioner;

int main() {
    // 端口、波特率、站号全部在调用时给，库里没有硬编码
    auto link = transport::open_serial("COM12", 9600);
    if (!link) return 1;
    devices::AirConditioner ac(*link.value());

    // FC01：默认读 10035..10058，正好覆盖全部命名状态位
    auto status = ac.read_status(/*address=*/1);
    if (status) {
        for (const auto& bit : status.value().reading.bits)
            if (bit.active)
                std::printf("%u %s%s\n", bit.address, bit.name.c_str(),
                            bit.is_alarm ? "  (告警)" : "");
    }

    // FC03：读运行参数（本例只读两个设定点）
    auto params = ac.read_params(1, reg::kCoolingOnSetpoint, 2);
    if (params)
        std::printf("制冷开启 %.1f / 停止 %.1f\n",
                    params.value().reading.registers[0].value,
                    params.value().reading.registers[1].value);

    // FC06：按手册变比把工程量写成 raw 再下发，超范围一个字节都不发
    ac.set_setpoint(1, reg::kCoolingOnSetpoint, 26.0);

    // FC05：开关机
    ac.set_power(1, true);
    return 0;
}
```

两个**必须知道的设备行为**（本库不实现，只是转达给你）：

1. 手册 §6.2 备注：模式控制（30026）必须**持续下发**，连续 100 s 收不到就自动切回自动运行。需要保活就得由你的业务模块周期调用 `set_mode`。
2. `10042` 开关机位在回读里是**低有效**（0 = 开机），与 FC05 写线圈 `0xFF00` = 开的约定方向相反。协议层如实上报位值、不做反相，语义由调用方按厂商约定解释。

### 各设备对应的接口

端口映射属于**你的业务层**，本库不硬编码、不做端口发现——所有驱动都在调用时接收端口/设备序列号作为参数。按交接文档约定的这套映射：

| 设备 | 接口 | 参数 | 备注 |
| --- | --- | --- | --- |
| 激光器 | COM10 | 38400 / 8N1 | |
| 测距仪 | COM7 | 9600 / 8N1 | 从站 1 |
| 状态灯 | COM8 | 9600 / **8E1** | 从站 1 |
| 空调 | COM12 | 9600 / 8N1 | 从站 1 |
| 光谱仪 ×4 | USB | 序列号 `1706097U4` ~ `1706100U4` | 各 2048 px |

---

## 进一步阅读

- **[docs/refactor-report.md](docs/refactor-report.md)** —— 完整重构报告：分层设计、依赖约束的实现方式、与旧实现的差异、重构中发现的 4 个真实缺陷、已知限制、如何在上层建立业务逻辑
- `cpp/README.md` —— 旧实现的完整技术细节（CLI 参数表、SDK ABI 核查等），仅作参考

