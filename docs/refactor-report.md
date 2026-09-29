# LIBS 重构报告

> 目标：把 `cpp/` 里混在一起的三类关注点拆开，重新写一份只保留最基础依赖方向的源码。
> 新代码在 `src/`，`cpp/` 原样保留作为参考与对照，不参与新构建。
> 报告日期：2026-09-28

---

## 1. 结论速览

| 项目 | 结果 |
| --- | --- |
| 新源码位置 | `src/`（`cpp/` 未改动） |
| 依赖方向 | 设备驱动 → 传输/SDK → 纯协议，单向，无反向引用 |
| 设备约束 / 安全策略 | 无。全部留给上层业务模块 |
| 编译 | `-std=c++17`，`-Wall -Wextra -Wpedantic` 零警告 |
| 平台 | Windows x64（MinGW GCC 13.1 / MSVC 均可） |
| 离线测试 | 455 项断言，5 个测试程序，全部通过 |
| 第三方依赖 | 无（`nlohmann/json` 已移除） |

依赖方向的**可验证性**不是靠约定，而是靠构建结构强制的：`libs_protocols` / `libs_transport` / `libs_devices` 三个静态库逐层链接，反向依赖在链接期就会失败（见 §4）。

---

## 2. 分层结构

```
┌──────────────────────────────────────────────────────────────┐
│  你的业务逻辑（不属于本次交付，需要你单独建立）              │
│  触发方案 · 多通道同步 · 激光安全策略 · 联锁 · 操作员确认     │
└───────────────────────────▲──────────────────────────────────┘
                            │ 只依赖 libs_devices
┌───────────────────────────┴──────────────────────────────────┐
│  第 1 层  设备驱动 / SDK          src/devices/               │
│  laser · rangefinder · indicator · spectrometer             │
│  把原始字节翻译成结构化数据。不做策略判断。                   │
└───────────────────────────▲──────────────────────────────────┘
┌───────────────────────────┴──────────────────────────────────┐
│  第 2 层  传输 / SDK 适配        src/transport/              │
│  串口收发 · 超时 · 端口枚举 · 厂商 DLL 动态加载              │
│  只搬运字节，不理解设备语义。                                 │
└───────────────────────────▲──────────────────────────────────┘
┌───────────────────────────┴──────────────────────────────────┐
│  第 3 层  纯协议                src/protocols/              │
│  modbus_rtu · laser · rangefinder · indicator                │
│  纯函数：字节 ↔ 值。无 I/O、无状态、无策略。                 │
└──────────────────────────────────────────────────────────────┘
```

箭头方向即允许的依赖方向。**任何反向引用都不存在**，例如 `protocols` 里没有任何一处 `#include` 指向 `transport` 或 `devices`。

---

## 3. 目录结构

```
src/
├── include/libs/                 公共头文件
│   ├── error.hpp                 Result<T> / Error / ErrorKind
│   ├── hex.hpp                   十六进制与字节格式化
│   ├── protocols/                第 3 层
│   │   ├── modbus_rtu.hpp        CRC16、帧构造、回包定位
│   │   ├── laser.hpp             激光器私有帧格式
│   │   ├── rangefinder.hpp       测距寄存器语义
│   │   ├── indicator.hpp         报警灯型号 / 通道 / 闪烁档
│   │   └── air_conditioner.hpp   空调 V2 状态位 / 参数表 + FC01/03/05/06
│   ├── transport/serial.hpp      第 2 层：Link 抽象 + Win32 COM
│   └── devices/                  第 1 层
│       ├── laser.hpp
│       ├── rangefinder.hpp
│       ├── indicator.hpp
│       ├── air_conditioner.hpp   空调收发与解码
│       └── spectrometer.hpp      AvaSpec 动态加载适配
├── src/                          实现，与头文件一一对应
├── tests/
│   ├── check.hpp / check.cpp     极简测试骨架（无外部依赖）
│   ├── test_protocols.cpp        120 项断言
│   ├── test_devices.cpp           55 项断言
│   ├── test_air_conditioner_protocol.cpp  114 项断言
│   ├── test_air_conditioner_device.cpp     61 项断言
│   ├── test_spectrometer.cpp     105 项断言
│   └── fake_avaspec_sdk.cpp      AvaSpec 替身 DLL 源码
├── CMakeLists.txt
├── build.ps1                     无需 CMake 的直编脚本
└── .gitignore
```

代码量：库与实现约 2400 行，测试约 1170 行。

---

## 4. 依赖方向如何被强制

`CMakeLists.txt` 用三个静态库表达分层，链接顺序即依赖顺序：

```cmake
add_library(libs_protocols STATIC ...)   # 不链接任何其他库
add_library(libs_transport STATIC ...)  # PUBLIC 依赖 libs_protocols
add_library(libs_devices   STATIC ...)  # PUBLIC 依赖 libs_transport
add_library(libs INTERFACE)             # 上层只需链接这一个
target_link_libraries(libs INTERFACE libs_devices)
```

如果有人在 `protocols` 里写了 `#include "libs/transport/serial.hpp"`，编译就会失败；如果有人在 `transport` 里引用了 `devices`，链接期就会失败。**这不是文档约定，是构建约束。**

同时可见，**没有反向的 `target_link_libraries`**：整个工程里不存在任何一条让低层依赖高层的边。

---

## 5. 各模块职责

### 5.1 第 3 层：纯协议（`libs_protocols`）

| 模块 | 职责 | 明确不做的事 |
| --- | --- | --- |
| `error` | 统一错误分类与 `Result<T>` | 不含业务语义 |
| `hex` | 字节 ↔ 十六进制文本 | — |
| `modbus_rtu` | CRC16、`make_request`、`locate_reply`（含回显剔除、噪声扫描、歧义拒绝） | 不碰串口，不假设设备型号 |
| `laser` | 激光器帧构造 / 解析 / 从噪声中定位完整帧 | 不判断该不该出光 |
| `rangefinder` | 测距寄存器语义与读响应解码 | 不做异常值业务过滤 |
| `indicator` | 报警灯型号表、通道 → 寄存器映射、灯态解析 | 不判断灯该亮不该亮 |
| `air_conditioner` | 空调状态位 / 参数表、文档地址 ↔ 报文偏移换算、FC01/03/05/06 编解码 | 不做温控策略、不做模式保活、不做任何联锁 |

这一层全部是无 I/O 的纯函数，输入字节、输出值。**这是唯一可以脱离硬件完整验证的一层**，所以它的测试最厚（120 + 114 项）。

### 5.2 第 2 层：传输（`libs_transport`）

- `Link` 抽象接口：一次 `exchange()` 就是一次独立请求-响应事务，**不会把上次的残留当成本次回复**。
- `open_serial()`：Win32 COM 口，构造失败返回 `Error` 而不是抛异常。
- `list_serial_ports()`：只列名字，不打开任何一个。
- 奇偶校验、帧、溢出错误不吞掉，通过 `Reply::receive_errors` 上报，由上层决定是否致命。

因为驱动器依赖的是 `Link` 接口而非具体串口实现，**设备测试可以用假传输离线跑**，不需要真实 COM 口。

### 5.3 第 1 层：设备驱动（`libs_devices`）

| 驱动 | 传输方式 | 说明 |
| --- | --- | --- |
| `laser` | 串口 `Link` | 读状态、写入设定值；状态位里含设备自报的 interlock / emergency_stop 字段，**仅作为数据上报，不构成任何门禁** |
| `rangefinder` | 串口 `Link` | 距离 / 速度 / 温度读值，Modbus 异常码透传 |
| `indicator` | 串口 `Link` | 灯态查询与写入，回显校验 |
| `air_conditioner` | 串口 `Link` | 空调状态 / 参数读写与开关机；只跑完一次 Modbus 事务并解码，不含任何温控或保护策略 |
| `spectrometer` | AvaSpec 厂商 DLL | `LoadLibraryExW` 动态加载、枚举、有界多次采集、幂等清理 |

光谱仪驱动有几个刻意的设计选择：

- **ABI 自检**：`static_assert` 校验厂商结构体布局（`AvsIdentity` 75 字节、`MeasureConfig` 41 字节、`trigger` 偏移 22 等）。SDK 换版本导致布局变化时，编译期就会失败，而不是运行时读到错乱内存。
- **单实例互斥**：用命名互斥体保证同一台机器上只有一个 SDK 操作在跑——AvaSpec 的 USB 句柄不是线程安全的。
- **清理是尽力而为且必报告**：`close()` 逐步尝试，任何一步失败都记录但**绝不中途返回**，最后汇总成 `cleanup_incomplete` 返回，而不是假装一切正常。
- **有界采集**：`scans` 限 1..100，总采样点超过 400 万直接拒绝并提示拆分，避免一次采集吃光内存。

---

## 6. 与旧 `cpp/` 的差异

### 6.1 删除的内容

| 旧文件 | 原因 |
| --- | --- |
| `cpp/src/main.cpp` | 程序入口与业务编排，属于上层 |
| `cpp/src/acquisition_coordinator.cpp` | 采集协调器，典型的业务逻辑 |
| `cpp/src/spectrum_export.cpp` | 谱图导出，业务呈现层 |
| `cpp/include/lpc/device_models.hpp` | 设备型号库 + 策略配置 |
| `cpp/third_party/nlohmann/` | JSON 依赖，配置文件解析专用 |
| `cpp/config/*.json` | 策略配置文件 |

### 6.2 安全策略的移除

旧代码把下面这些**硬编码在驱动层**，新代码里一条都没有：

| 旧策略 | 新代码 |
| --- | --- |
| `LaserWritePolicy`（写激光前的策略校验） | 已移除 |
| `laser_readiness` / 就绪门禁 | 已移除 |
| `operator_confirmed`（操作员确认） | 已移除 |
| `physical_wiring_confirmed`（物理接线确认） | 已移除 |
| `TriggerPlan`（触发方案校验） | 已移除 |
| 联锁 / 门禁 gate | 已移除 |
| 旧代码中不存在、但重构时**刻意不加**的：空调温控策略、超温保护、压缩机启停判断、模式保活（连续 100 s 无命令自动回自动）、任何联锁 | 未实现（留给上层业务模块） |

现在 `Laser::write(...)` 只做一件事：**把请求编码成设备能懂的字节并发送**。至于"现在允不允许写"，是上层的判断，驱动无权过问——这正是你要求的边界。

唯一的例外值得说明：激光 B2 状态帧里仍然有 `interlock_1`、`interlock_2`、`emergency_stop`、`overtemperature` 这几个**字段名**。它们是设备自己上报的遥测位，属于协议内容，**只解码上报，不参与任何门禁判断**。如果你希望连这些字段名都不出现在驱动层，可以在解析时丢弃，但那时上层就拿不到设备自报的安全状态了——我倾向于保留原始数据、把判断留给上层。

### 6.3 命名与错误处理

- 命名空间从 `lpc` 改为 `libs`，头文件路径 `lpc/` → `libs/`。
- 统一到 `libs::ErrorKind` 八类分类（`invalid_argument` / `unsupported` / `protocol` / `transport` / `device` / `timeout` / `internal`），调用方看一个 `ok` 即可，不必区分错误来自编解码还是串口。
- 编程错误（参数越界、索引非法）才抛异常；**设备 / 协议失败一律走 `Result` 返回**，不用异常做控制流。

### 6.4 测试对比

| | 旧 `cpp/tests` | 新 `src/tests` |
| --- | --- | --- |
| 测试文件 | 5 个 | 3 个 |
| 断言数 | 未统计 | **455** |
| 设备种类 | 4 | **5**（新增空调） |
| 外部测试框架 | 无 | 无（`check.hpp` 79 行） |
| 覆盖重点 | 协议 + 驱动 + 协调器 + 触发方案 | 协议 + 驱动 + SDK 适配 |

新测试去掉了协调器与触发方案相关的用例（对应代码已删除），并**新增了光谱仪驱动的完整覆盖**（105 项），包括 ABI、枚举、采集顺序、失败注入、超时部分结果保留、幂等清理。

---

## 7. 重构过程中发现并修复的 4 个真实缺陷（另见 §7.5 空调接入）

这几个是原 `cpp/` 源码里就存在的问题，在重写时暴露出来。它们值得单独记录，因为都是"看起来能跑、实际不对"的类型。

### 7.1 激光 CRC 覆盖范围不一致（严重）

**现象**：本地构造的帧无法被自己的解析器接受，甚至段错误。

**原因**：构造帧时用 `frame.size()-3` 计算 CRC 覆盖长度，此时两个 CRC 字节**还没被追加**，导致读到了越界内存；而解析时用的是 `size()-kCrcBytes`。编码与解码用了两套不同的表达式。

**修复**：引入共享常量，并统一成一个表达式：

```cpp
constexpr std::size_t kHeaderBytes = 5;
constexpr std::size_t kCrcBytes     = 2;
constexpr std::size_t kCrcSkipBytes = 3;   // 5A A5 len 不参与 CRC
std::size_t crc_covered(std::size_t frame_size) {
    return frame_size - kCrcSkipBytes - kCrcBytes;
}
```

先追加 2 个占位零字节再算 CRC，使构造、解析、噪声定位三处共用同一表达式。

### 7.2 测距 FC03 校验器要求回包回显起始寄存器（严重）

**现象**：真实设备的所有回包被全部拒绝。

**原因**：校验器要求响应的字节 3、4 等于请求的起始寄存器，但**标准 Modbus FC03 响应根本没有寄存器回显**。这是从旧代码移植时引入的错误——旧代码的校验器只检查字节计数，是对的。

**修复**：只校验 `p[2] == count*2`。

### 7.3 报警灯被误当作标准 FC01（严重）

**现象**：查询回包无法解析。

**原因**：科隆威报警灯是**厂商私有 8 字节格式**，不是标准 FC01。回包布局为：

```
addr, func, 0x00, reg, 0x00, state, crc_lo, crc_hi
```

**寄存器是被回显的**，与测距仪恰好相反。状态字节取值 `0x00` / `0xFF` / `0x11` / `0x22`。

**修复**：按厂商文档实现，校验 `p[2]==0x00 && p[3]==reg && p[4]==0x00` 且状态字节在已知集合内。依据是 `device-references/` 里的厂商 Python 示例，其中写帧 `01 05 00 03 FF 00 7C 3A`（绿灯亮）的 CRC 可精确复现。

> 说明：这个缺陷我在重构中途一度改错方向——先按测距仪的方式"修正"了它，后来核对厂商文档才发现灯是私有格式，于是回退。**设备协议不能靠类比推断，必须以厂商文档为准。**

### 7.4 `Register::valid` 默认值错误（轻微但影响面大）

**现象**：距离、速度、温度读值一律被标记为无效。

**原因**：`Register::valid` 默认 `= false`，而 `describe_register` 只在文档明确的哨兵值情况下才赋值。

**修复**：改为默认 `= true`。

---

## 7.5 空调接入：协议 V2（Modbus RTU 变体）

空调是本轮新增的第 5 类设备。它沿用 Modbus RTU 帧格式和 CRC16（因此直接复用 `libs::modbus`，没有新增传输层），但**寄存器寻址不是标准 Modbus**：

手册用 5 位的**文档地址**（10001 / 30001 / 40001）描述寄存器，报文里出现的是**从 0 开始的偏移量**，换算基准随功能码而变：

| 功能码 | 含义 | 报文起始偏移 | 数据宽度 |
| --- | --- | --- | --- |
| `0x01` | 读运行状态 | `文档地址 − 10001` | 每寄存器 1 位，bit0 在前，高位补零 |
| `0x03` | 读运行参数 | `文档地址 − 30001` | 每参数 2 字节高字节在前，单次 ≤ 50 |
| `0x05` | 写状态位 | `文档地址 − 10001` ※ | 1 个线圈值 |
| `0x06` | 写运行参数 | `文档地址 − 40001` | 1 个 16 位值，落 EEPROM |

对外 API 一律使用**文档地址**，偏移换算封装在协议层内部，调用方不会有机会记错基准。

### 厂商文档唯一的笔误（FC05 偏移基准）

手册 §4.5 注 1 写 `sReg = 寄存器号 − 00001`。但厂商自己的样本 §7.11/§7.12 写开关机用的偏移是 `0x0029 = 41 = 10042 − 10001`，即与 FC01 同一个 1xxxx 基准。按 `00001` 换算会得到 10041，设备不会认。

**本实现以实测样本为准**，并用一条专门的测试用例（`control_request_uses_the_measured_offset_not_the_doc_formula`）把这个基准钉死，防止后来有人"照手册修正"回去。

其余部分经逐条核对**完全自洽，不需要纠偏**：

- §7.8/§7.9/§7.10 散文对位序的描述与寄存器表一致（已用三个样本帧独立复算验证：§7.8 → 10035+10037；§7.9 → 10035+10037+10038+10044+10045；§7.10 → 10035+10037+10038+10039+10044+10045+10054）。
- `0x0000` 关 / `0xFF00` 开的取值在寄存器表、§4.5 注、§7.11/§7.12 三处一致，也符合 Modbus FC05 惯例。

### 两个有符号性的坑

1. **有符号性是"每个参数各自的属性"，不能从单位字符串推断。** `30025` 湿度校正值单位为空、变比 ×1，但样本 `0xFFFC` 就是 **−4**，必须按补码解释；反过来 `30026` 模式 / `30027` 波特率 / `30028` 地址同样无单位，却是无符号的。因此 `ParamRegister` 显式暴露 `is_signed` 字段，`kParamDefs` 表里逐个标注。

2. **范围校验必须按补码比较。** 手册给的可写范围里 `30016` 加热开启点是 −30.0 ~ 15.0 °C，下限 raw 为 `0xFED4`。若用无符号比较，任何负温度（连同 `30019` 的 −10.0 下限 `0xFF9C`）都会被误判为越界而根本发不出去。协议层与驱动层两处比较都已改为 `int16_t` 语义，并有专门用例覆盖（`set_setpoint_accepts_negative_temperatures`）。

### 两个必须转达给调用方的设备行为

本库**不实现**这两点，只是如实记录：

1. **模式控制需持续下发**（手册 §6.2 备注）：连续 100 s 收不到 `30026` 的控制命令，空调自动切回自动运行模式。需要保活必须由业务模块周期调用 `set_mode`。这是设备行为，不是本库可以"顺手帮忙"的地方。
2. **`10042` 开关机位在回读里是低有效**（0 = 开机），与 FC05 写线圈 `0xFF00` = 开的约定方向相反。协议层只如实上报位值、**不做反相**，语义解释权在调用方。

### 一处共享代码的改动

`modbus::locate_reply` 增加了一个带默认值的尾参数 `reject_request_echo`（默认 `true`，保持既有行为不变）。空调 FC05/FC06 的解码传 `false`：这两个功能码的响应与请求**逐字节完全相同**（§7.4~§7.6、§7.11/§7.12 的收发帧一致），若沿用默认的回显剔除，真正的响应反而会被丢掉。

### 明确不做的事

空调驱动只负责"跑完一次 Modbus 事务并解码"。**没有**温控策略、没有超温保护、没有联锁、没有模式保活、没有"当前温度是否该停压缩机"的判断。`set_setpoint` 里的范围检查属于**协议参数合法性校验**（越界的值设备会回异常码，挡在发送前只是省一次事务），不是安全策略。是否启用空调、用什么设定点、什么时候启停，全部属于上层业务模块。

---
## 8. 构建与测试

### 8.1 方式一：`build.ps1`（推荐，无需 CMake）

```powershell
cd src
pwsh -File build.ps1
```

逐文件编译 → 静态链接 → 运行全部测试。产物在 `src/build/`：

```
libs_protocols.a   libs_transport.a   libs_devices.a
test_protocols.exe test_devices.exe   test_spectrometer.exe
test_air_conditioner_protocol.exe  test_air_conditioner_device.exe
fake_avaspec_test.dll
```

可选参数：`-Compiler <g++ 路径>`、`-SkipTests`。

### 8.2 方式二：CMake + CTest

```powershell
cd src
cmake -S . -B build/obj -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/obj
ctest --test-dir build/obj --output-on-failure
```

> **生成器**：Ninja 与 MinGW Makefiles 都已实测通过。若 Ninja 不在 PATH，需显式指定 `-DCMAKE_MAKE_PROGRAM=<ninja.exe 路径>`。
>
> 另：光谱仪测试用例在 `CMakeLists.txt` 里只在 MinGW 工具链下接线（`if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")`），因为需要 `-shared` 产出替身 DLL。

### 8.3 测试结果

```
OK tests/test_protocols.cpp:    120 checks passed
OK tests/test_devices.cpp:       55 checks passed
OK tests/test_air_conditioner_protocol.cpp: 114 checks passed
OK tests/test_air_conditioner_device.cpp:   61 checks passed
OK tests/test_spectrometer.cpp: 105 checks passed
──────────────────────────────────────────────────
                            455 项断言全部通过
```

`ctest` 视角：

```
1/5 Test #1: protocols ................. Passed
2/5 Test #2: devices ................... Passed
3/5 Test #3: air_conditioner_protocol .. Passed
4/5 Test #4: air_conditioner_device ..... Passed
5/5 Test #5: spectrometer ............. Passed
100% tests passed, 0 tests failed out of 5
```

**全部测试离线运行**，不接触任何真实硬件、不需要 COM 口、不需要真实 AvaSpec DLL。

---

## 9. 两个平台细节（踩过的坑）

### 9.1 工程路径与 DLL 路径编码

本仓库最初位于 `D:\桌面\LIBS\`（中文路径），现已迁移到 `D:\LIBS\`。迁移后实测结论：

1. **CMake 生成器不再是问题**。中文路径下 MinGW Makefiles 会报 `No rule to make target`；迁到纯 ASCII 路径后，Ninja 和 MinGW Makefiles 两者都正常。`build.ps1` 不依赖生成器，任何路径下都能跑。
2. **AvaSpec DLL 路径编码要求仍在**（这是驱动的固有契约，与工程路径无关）：`Spectrometer::open()` 要求绝对路径的 **UTF-8**，而 Windows 的 `argv` / 环境变量是**当前 ANSI 代码页**的字节。若代码页不是 UTF-8，直接传中文路径会被驱动拒绝：

   ```
   Error: the SDK DLL path must be valid UTF-8
   ```

   **解决**：光谱仪测试自己先把替身 DLL 复制到纯 ASCII 的临时目录（`%TEMP%\libs_fake_avaspec\`），再把那条 ASCII 路径交给驱动。ASCII 路径的 ANSI 字节本身就是合法 UTF-8，问题消失。测试逻辑对调用方透明。

### 9.2 替身 DLL 必须保持驻留

光谱仪测试的失败注入依赖替身 DLL 的全局状态。最初的做法是每次操作后 `FreeLibrary`，结果**模块被真正卸载**，下次 `open()` 拿到全新实例，之前设进去的失败模式全部丢失，失败注入用例静默失效（断言"没失败"却通过了）。

**修复**：测试全程持有一份 DLL 句柄不释放。这条约束已写进代码注释，因为它是反直觉的。

---

## 10. 如何在上层建立业务逻辑

新库为你的业务层留好了接口，**不需要改动 `src/` 里任何一行代码**。

### 10.1 建一个 `libs_business` 静态库

```cmake
add_library(libs_business STATIC
  src/policy/laser_write_policy.cpp
  src/plan/trigger_plan.cpp
  src/app/acquisition_coordinator.cpp
)
target_link_libraries(libs_business PUBLIC libs)   # 只依赖聚合目标
```

### 10.2 依赖方向

```
libs_business  →  libs  →  libs_devices  →  libs_transport  →  libs_protocols
```

业务层可以自由引用三层的任何东西；反过来，三层**看不到**业务层的任何存在。CMake 在链接期保证这一点。

### 10.3 具体的接入点

1. **写激光的安全策略**（旧 `LaserWritePolicy` 的位置）
   在业务层定义，调用 `Laser::write()` 前先自查：

   ```cpp
   // 业务层举例：门禁完全在你自己手里，驱动一行代码都不知道它存在
   struct Gate {
       bool operator_confirmed        = false;
       bool physical_wiring_confirmed = false;
       bool laser_ready               = false;
   };

   libs::VoidResult emit(libs::devices::Laser& laser, const Gate& gate,
                         std::uint8_t command, const libs::Bytes& payload) {
       if (!gate.laser_ready || !gate.operator_confirmed || !gate.physical_wiring_confirmed)
           return libs::Error::make(libs::ErrorKind::device, "gate_closed",
                                     "激光未就绪或未确认");
       auto result = laser.write(command, payload);   // 驱动不做任何判断
       if (!result)
           return result.error();
       return libs::VoidResult::success(std::monostate{});
   }
   ```

   注意驱动的签名是 `write(std::uint8_t command, const Bytes& payload = {})` ——
   **它接受任意指令码，不阻止任何指令**。旧实现里的 `LaserWritePolicy` 就是从这里
   搬走的，现在它只存在于你的业务模块里。

2. **触发方案与多通道同步**
   `CaptureRequest` 里 `master_serial` / `synchronized` / `laser_width_cycles` 都是**显式字段，没有隐式默认值**。业务层决定填什么，驱动只负责照着执行。`ScanGroup::arm_order` 记录了实际的 arm 顺序（非主通道优先），可用于校验你的同步假设。

3. **统一错误处理**
   全部错误都是 `libs::Error`，带 `kind` / `code` / `message` / `raw`（原始字节）。可以直接落日志：

   ```cpp
   if (!result) {
       log(to_string(result.error().kind),    // "device" / "timeout" / ...
            result.error().code,              // "gate_closed" / "capture_timeout" / ...
            result.error().message,
            result.error().raw);              // 出错时的原始回包，事后可复盘
   }
   ```

---

## 11. 已知限制

1. **仅 Windows x64**。`CMakeLists.txt` 在非 Windows 或 32 位工具链下会直接 `FATAL_ERROR`。原因是传输层用 Win32 COM、光谱仪用 `__stdcall` 导出。源码里的 `#else` 分支已为非 Windows 留了桩（返回 `unsupported`），但未做实际移植。

2. **替身 SDK 只在 MinGW 下接线**。`CMakeLists.txt` 里光谱仪测试用 `-shared` 编替身 DLL，条件编译在 GNU/Clang 分支内。用 MSVC 工具链时该测试不会被注册，编译静态库的路径不受影响。

3. **光谱仪测试路径较绕**。第 9 节的两个问题（DLL 路径编码、DLL 驻留）是环境约束导致的，不是设计选择。

4. **旧 `cpp/` 未做验证**。本次重构只保证了新代码的正确性。旧代码原样保留，是否还编译、行为是否与新代码一致，未做核实。

5. **未做真实硬件验证**。所有测试都是离线替身。接真机时，串口时序和 AvaSpec 的实际行为可能暴露替身覆盖不到的问题——尤其 `AVS_PollScan` 的真实触发时序，替身用的是"armed 即就绪"的简化模型。

---

## 12. 建议的后续步骤

1. **接真机验证**串口四类设备（激光、测距、报警灯、空调）的收发时序；空调尤其要确认 FC05 写开关机的偏移基准与现场设备一致（即 §7.5 记录的 `10001` 而非手册写的 `00001`）。
2. **接真机验证** AvaSpec，重点是 `AVS_PollScan` 的触发等待逻辑。
3. 建立 `libs_business`，把旧 `cpp/` 里的策略、触发方案、协调器按需迁移进去。
4. 若需要跨平台，把传输层抽象到 POSIX 实现，`spectrometer.cpp` 已有 `#else` 桩作为起点。



