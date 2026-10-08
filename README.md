# LIBS 现场检测系统

LIBS（激光诱导击穿光谱）现场检测系统的完整控制软件。
覆盖激光器、测距仪、三色报警灯、空调、四台光谱仪五类硬件，
从字节协议一直做到业务编排、守护进程和 Qt6 界面。

> **当前状态（2026-10-01）**：设备层收敛、两条业务（就绪判定 / 激光触发光谱检测）、
> 守护进程、UI 全部完成并有测试覆盖。
> 后端依赖关系已收敛为 `business -> devices -> transport`。
> `src/` 下 **12 套**离线测试共 **1084 个断言全部通过**。

---

## 系统组成

系统由**两个进程 + 三层后端运行结构**组成：

```text
  +--------------------------------------------------+
  |  ui/libs-spectrum-ui.exe                        |  Qt6 Widgets + Charts
  |  实时光谱预览 / 参数下发 / 出光按钮 / 门禁指示   |  只链接 Qt6
  +---------------------+----------------------------+
                        |  命名管道 \\.\pipe\libs-businessd
                        |  JSON Lines（一次请求一行，一次响应一行）
                        v
  +--------------------------------------------------+
  |  src/businessd.exe                               |  守护进程，独占设备
  |  facade 命令面（14 条）-> Runtime -> 两条业务      |  <- 判断权全部在这里
  +---------------------+----------------------------+
                        v
             business/   readiness（就绪判定）
                         measurement（激光触发光谱检测）
                         shared（共同底座）+ app（装配）
                        v
             devices/    每台设备一个公开控制器
                         （Laser / Spectrometer / Indicator /
                          AirConditioner / Rangefinder）
                         设备动作、时序、读回确认；protocols/ 编入本层
                        v
             transport/  Link 抽象、Win32 COM 串口收发与超时
```

`ui/` 与 `src/` 是**两个独立的 CMake 工程**，彼此不链接。
UI 与后端唯一的接口是命名管道上的 JSON 行。

> 完整架构说明（逐文件职责、硬约束、设计判断）见
> **[docs/architecture-report.md](docs/architecture-report.md)**。

---

## 这个仓库做什么、不做什么

**做：**

- 把原始字节翻译成结构化数据（距离、温度、灯态、波长、counts）
- 帧的构造、校验、从串口噪声中定位完整帧
- 串口收发与超时管理
- AvaSpec 厂商 SDK 的动态加载与适配
- 站点配置、设备独占会话、**安全门禁与联锁判定**
- 出光时序编排、多通道光谱采集、实时预览流、数据落盘
- 命名管道命令面 + Qt6 现场界面

**不做（有意为之）：**

- 不提供远程 SSH 隧道封装（传输交给系统 `ssh.exe`）
- 不内置光谱到成分的算法（接口已就绪，无算法时如实返回 `unavailable`）
- 不硬编码端口映射（全部来自 `site.json`）
- 不静默回落配置默认值（缺失即报错）

**安全判定只有一个入口。** 设备层解码出的安全状态位（interlock、emergency_stop）
会照常上报，但**绝不参与任何判定**——判断只发生在
`business::readiness::evaluate()`，调用方是 `business::measurement::MeasurementJob`。

---

## 目录结构

```text
D:\LIBS
+- src/                          <- 后端 + 库（唯一在用的 C++ 源码树）
|  +- include/libs/
|  |  +- error.hpp               统一 Result<T> / Error / 错误分类
|  |  +- hex.hpp                 字节与十六进制文本
|  |  +- transport/              串口收发与超时（Link 抽象）
|  |  +- protocols/              设备协议编解码（编入 libs_devices，非独立层）
|  |  +- devices/                每台设备一个公开控制器
|  |  +- business/
|  |  |  +- shared/              共同底座（不依赖任何具体业务）
|  |  |  |  +- config/           站点配置、参数库
|  |  |  |  +- session/          设备独占会话与生命周期
|  |  |  |  +- logging/          运行日志与恢复快照
|  |  |  |  +- json/             自带 JSON 实现（无第三方依赖）
|  |  |  +- readiness/          业务一：就绪判定
|  |  |  +- measurement/         业务二：激光触发光谱检测
|  |  |  +- app/                 依赖装配与生命周期（Runtime / facade）
|  |  +- businessd/              守护进程 IPC 契约
|  +- src/                       实现（与 include 一一对应）
|  +- remote/lpc.py              薄客户端 CLI（Python，零第三方依赖）
|  +- tests/                     12 套离线测试，共 1084 个断言
|  +- CMakeLists.txt             CMake 入口（层间约束在此）
|  +- build.ps1                  免 CMake 一键编译 + 跑测试（含依赖方向守卫）
+- ui/                           <- Qt6 前端（独立工程）
|  +- src/                       main_window / pipe_client / spectrum_plot /
|  |                             status_poller / stream_poller / frame_model /
|  |                             machine_state / widgets / theme
|  +- tests/stream_probe.cpp     无头验证 UI 自己的取帧循环（未进构建脚本）
|  +- CMakeLists.txt
|  +- build.ps1                  一键编译 + windeployqt 部署
+- device-references/            <- 厂家协议手册与 SDK，驱动的依据来源
+- docs/                         架构、业务流程、执行方案与报告
+- data/                         运行时数据（run.jsonl、captures/、parameters.json）
```

---

## 源码文件职责与分层

下表中 `xxx.hpp / xxx.cpp` 表示同一模块的接口声明与实现。
头文件放在 `src/include/libs/`，实现放在 `src/src/`。
**详细逐文件说明见 [docs/architecture-report.md](docs/architecture-report.md)。**

### 公共基础

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `libs/error.hpp` / `error.cpp` | 公共基础 | 统一 `Result<T>`、`VoidResult`、`Error{kind,code,message,raw}` 与 8 种 `ErrorKind`。**不用异常做控制流**（只有编程错误才抛）。`Result` 默认构造是**成功**，因为全库用 `return VoidResult();` 表达"无返回值且成功"。 |
| `libs/hex.hpp` / `hex.cpp` | 公共基础 | `Bytes` 别名 + 大写空格分隔 hex 串，用于日志与断言。 |

### 第 1 层：transport

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `libs/transport/serial.hpp` / `serial.cpp` | transport | `Link` 抽象与 `Reply`；实现 Win32 COM 口打开（`share=0` 独占）、收、发、超时、错误计数、端口枚举。它只搬字节，不解析任何设备帧。 |

`transport::Link` 是所有串口设备离线测试的注入边界；测试里的 `FakeLink` 正是实现这个接口。

### 第 2 层：devices

`devices` 是唯一公开给业务层的设备入口层。每台设备只有一个控制器类：
协议组帧、串口事务、动作时序和读回确认都封装在控制器内部。

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `libs/devices/laser.hpp` / `laser.cpp` | devices | 风冷激光器控制器。`readActual`、`readSettings`、`readTec`、`prepareContinuous`、`openQGate`、`startContinuous`、`stopContinuous`、`fireOnce`、`resetFault`、`clearCounter`；`LaserActual` **保留设备原始极性**（`q_gate=1` 是光闸开），不翻译成安全/危险。`LaserOperationTrace` 留逐帧证据。**安全禁区**：0x15/0x17/0x18 无公开写入口，TEC 只读。 |
| `libs/devices/spectrometer.hpp` / `spectrometer.cpp` | devices | AvaSpec 四通道光谱仪控制器。动态加载厂商 DLL、`open/enumerate/capture/stream/close` 生命周期、触发方向与 SYNC、FPGA 延时换算、逐组流式回调、`sdk_abi()` ABI 自检。不执行激光出光许可。 |
| `libs/devices/indicator.hpp` / `indicator.cpp` | devices | 三色状态灯控制器。`apply`、`applyAll`、`queryAll`、`probeModel`；内部强制相邻命令间隔（>50 ms），业务层只传灯色意图。 |
| `libs/devices/rangefinder.hpp` / `rangefinder.cpp` | devices | TOF101 控制器。`readRawRegisters` 与常用观测窗口 `observeOnce`；不进行距离业务换算。 |
| `libs/devices/air_conditioner.hpp` / `air_conditioner.cpp` | devices | 空调控制器。状态读取、参数读写、`set_mode` / `set_power` / `set_setpoint`；`held_mode_` 只如实记录上次下发时间，不替业务做保活策略。 |

设备协议位于 `libs/protocols/`，但**不再是独立公开层**，而是被编入 `libs_devices`：

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `libs/protocols/modbus_rtu.hpp` / `modbus_rtu.cpp` | devices 内部协议 | CRC16、组帧、**`locate_reply()`**（处理请求回显、前后垃圾字节、CRC 错）。`reject_request_echo` 必须由调用方按协议决定。 |
| `libs/protocols/laser.hpp` / `laser.cpp` | devices 内部协议 | `5A A5` 私有帧的 CRC、组帧、解码、字段提取与 `find_frame()` 三态扫描。 |
| `libs/protocols/air_conditioner.hpp` / `air_conditioner.cpp` | devices 内部协议 | 空调 V2 文档地址与寄存器表、FC01/03/05/06 编解码。 |
| `libs/protocols/indicator.hpp` / `indicator.cpp` | devices 内部协议 | 灯通道/状态/型号映射（`ModelTraits`），查询与写请求编解码。 |
| `libs/protocols/rangefinder.hpp` / `rangefinder.cpp` | devices 内部协议 | TOF101 寄存器表、FC03 读请求与响应解码。 |

### 第 3 层：business

业务层负责"现在允不允许做"和"跨设备流程怎么编排"，不负责单台设备的帧和时序。
分四条子线：`shared`（底座）、`readiness`（业务一）、`measurement`（业务二）、`app`（装配）。

#### shared —— 两条业务的共同底座

> **纪律：`shared/` 绝不能 include `business/readiness/` 或 `business/measurement/`。**
> 由 `build.ps1` 的 grep 守卫强制，反向依赖会**直接让构建失败**。

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `shared/time_util.hpp` / `.cpp` | business/shared | 墙钟与单调时间，**分开不混**：墙钟会被校时改动，单调钟只算间隔。 |
| `shared/json/json.hpp` / `.cpp` | business/shared | 自带最小 JSON。**不用第三方是为跨进程接口不引入 ABI 风险**。解析严格，尾逗号/坏转义一律报错。 |
| `shared/config/site_config.hpp` / `.cpp` | business/shared | **装机配置 = 硬件事实**（错了机器接不上）。端口/波特/超时/使能全部来自文件，**禁止静默回落默认值**。`config validate` 不开串口。 |
| `shared/config/parameter_store.hpp` / `.cpp` | business/shared | **测量参数 = 工艺事实**（错了只是这次测不准）。核心是"设置已完成"的两态语义：收到请求 != 完成。 |
| `shared/logging/run_log.hpp` / `.cpp` | business/shared | 运行日志（JSON Lines 追加写，**不缓冲**——现场断电是常态）+ 恢复快照仓库。快照**只写不自动续跑**。 |
| `shared/session/device_session.hpp` / `.cpp` | business/shared | **进程内唯一持有设备对象的地方**，业务层别处只拿引用。`enabled=false`（配置上没接）与"打开失败"是两个语义。 |
| `shared/data_delivery.hpp` / `.cpp` | business/shared | 交付层。`LocalFileSink` 落盘；`UnsupportedGatewaySink` 返回 `unsupported` + 前置条件，**绝不返回已交付的假象**。 |

#### readiness —— 业务一：就绪判定

流水线：`设备事实 -> monitor（何时判）-> evaluate() 纯函数 -> lamp_states() 纯函数 -> apply_lamp() 唯一写灯点`

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `readiness/facts.hpp` / `.cpp` | business/readiness | **只承载事实，一个判断都不做**。`parse_actual()` 是 B2 -> 安全位的**唯一转换点**，语义统一为 `true = 安全`（逐位取反不是笔误）。读不到 != 安全。 |
| `readiness/verdict.hpp` / `.cpp` | business/readiness | `evaluate()` 是**零设备 I/O 的纯函数**，可离线穷举。`GateVerdict` 不只过/不过，还要说明**为什么不过**。 |
| `readiness/lamp.hpp` / `.cpp` | business/readiness | 状态->灯色**纯函数** + `apply_lamp()` **全库唯一灯下发点**。灯不反向参与判定；灯写失败不改变 verdict。 |
| `readiness/monitor.hpp` / `.cpp` | business/readiness | 回答**何时判**。`FactSource` 用回调而非持设备指针。常驻线程接口先定死、暂不实现。 |
| `readiness/precheck.hpp` / `.cpp` | business/readiness | **唯一**把门禁结论变成物理动作（写灯）的地方。预检与亮灯不拆成两个。`force_offline()` 只给关机路径用。 |

#### measurement —— 业务二：激光触发光谱检测

流程：**连续出光 -> 每次出光被光谱仪采到 -> 落盘 -> 实时显示**

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `measurement/frame.hpp` / `.cpp` | business/measurement | 纯数据结构 + 序列化，零行为。`laser_shot` 区分**测量谱**与**背景预览帧**。通道缺失是第三态 `absent`，**不补零**。帧里不做显示加工。 |
| `measurement/frame_bus.hpp` / `.cpp` | business/measurement | 纯内存发布订阅，**不碰设备、不起线程**。所有权在 Runtime（UI 要能在 start 之前就订阅）。背压=丢最旧帧，如实计 `dropped_frames`。 |
| `measurement/capture_plan.hpp` / `.cpp` | business/measurement | **纯函数**，钉死字段归属：装机配置供"接了哪几台/主从/SYNC"，参数库供"积分时间/超时/延时/次数"。`wait_for_laser_trigger` 硬编码 true。 |
| `measurement/writer.hpp` / `.cpp` | business/measurement | 独立线程 + 有界队列（64，满丢最旧）。**安全路径与数据路径解耦**——收光不能排在磁盘后面。 |
| `measurement/job.hpp` / `.cpp` | business/measurement | 作业主体，**全库唯一同时碰激光器和光谱仪的对象**。七条纪律见文件头。 |

#### app —— 装配与命令面

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `app/runtime.hpp` / `runtime.cpp` | business/app | 依赖装配与生命周期，**不含任何业务逻辑**（文件头明令：不允许出现判定或出光决策）。持有 `device_mutex()`（进程级串口互斥）与出光会话记账。 |
| `app/parameter_service.hpp` / `.cpp` | business/app | **测量参数的唯一来源**。`fire_parameters()` 组装并校验出光参数；`spectrometer_measurement()` 专供预览并回落，避免"没标定"连预览都起不来。 |
| `app/facade.hpp` / `facade.cpp` | business/app | 命令面（14 条）。每个方法对应一条 CLI 子命令；坏 JSON/缺字段/未知命令都返回 JSON 错误，**不抛异常也不静默成功**。 |

### 进程边界与客户端

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `libs/businessd/named_pipe_server.hpp` / `.cpp` | 进程边界 | 命名管道 JSON Lines；每连接一线程，**设备访问仍由 Runtime 串行化**（驱动同步阻塞，多线程交错事务会串帧）。管道 ACL 限当前用户。`handle_line()` 公开以便离线测试。 |
| `src/businessd/main.cpp` | 进程边界 | `businessd.exe` 入口：解析命令行、加载配置、启动 Runtime 与管道服务、注册 Ctrl handler。 |
| `src/remote/lpc.py` | 远程客户端 | 经命名管道发 JSON 的 Python 薄客户端，零第三方依赖；可用于远端经 SSH 执行。 |

### 测试与构建文件

| 文件 | 所属层 | 作用 |
| --- | --- | --- |
| `src/tests/check.hpp` / `check.cpp` | tests | 零依赖测试断言宏与计数。 |
| `src/tests/fake_avaspec_sdk.cpp` | tests | AvaSpec SDK **替身 DLL 夹具**（不是厂家 DLL 的替代品），供光谱仪测试离线运行。 |
| `src/CMakeLists.txt` | build | CMake 目标、依赖约束与 CTest 注册。 |
| `src/build.ps1` | build | 不依赖 CMake 的 MinGW 一键编译与测试脚本。**含 `shared/` 反向依赖 grep 守卫**。 |

---

## 依赖方向

**严格单向，无反向引用。** 这不是文档约定，而是构建约束。

```text
              ui/libs-spectrum-ui        （只链接 Qt6，绝不链接 libs_*）
                       |  命名管道 JSON Lines
                       v
              src/businessd.exe          可执行入口（进程边界，无业务逻辑）
                       |
              business/app               Runtime 装配 + facade 命令面
                       |
              business/measurement       业务二：唯一同时碰激光器与光谱仪
                       |
              business/readiness         业务一：facts -> verdict -> lamp
                       |
              business/shared            共同底座（不得反向依赖上面两条）
                       |
              devices                    每台设备一个公开控制器
                       |                  （protocols/ 编入本层）
                       |
              transport                  串口收发，只搬字节
```

**守卫 1（静态检查）** `build.ps1` 把源文件划成 `libs_transport` / `libs_devices` /
`libs_business` 三组，顺序即依赖顺序；并检查 `business/shared/` 下的源文件
不含 `business/(readiness|measurement)/` 的 include，**违反直接 throw**。

**守卫 2（链接线）** `tests/test_frame_queue.cpp` 只链 4 个纯内存文件——
往 `frame_bus` 加任何设备调用会**直接链接失败**，而不是靠 code review 发现。

`D:\LIBS\ui\CMakeLists.txt` 顶部注释解释了**为什么 UI 连 `libs_business` 都不能链**：
C++ 里 PUBLIC 依赖会传染到下游的链接命令行，判定权一旦漏到界面，分层就名存实亡。

---

## 快速开始

### 构建与测试（后端）

```powershell
cd D:\LIBS\src
$env:PATH="D:\Qt6\Tools\mingw1310_64\bin;$env:PATH"   # 或确保 g++ 在 PATH
pwsh -NoProfile -File build.ps1
```

实际输出（末行 `==> all tests passed`，约 60 秒）：

```text
    shared/ 反向依赖检查通过（7 个文件）
OK tests/test_protocols.cpp:                124 checks passed
OK tests/test_devices.cpp:                   43 checks passed
OK tests/test_air_conditioner_protocol.cpp: 114 checks passed
OK tests/test_air_conditioner_device.cpp:    60 checks passed
OK tests/test_device_actions.cpp:            52 checks passed
OK tests/test_spectrometer.cpp:             216 checks passed
OK tests/test_business_core.cpp:             90 checks passed
OK tests/test_business_services.cpp:         41 checks passed
OK tests/test_business_facade.cpp:          154 checks passed
OK tests/test_business_ipc.cpp:              31 checks passed
OK tests/test_frame_queue.cpp:               68 checks passed
OK tests/test_measurement_job.cpp:           91 checks passed
==> all tests passed
```

也支持 CMake + CTest：

```powershell
cd D:\LIBS\src
cmake -S . -B build/obj -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/obj
ctest --test-dir build/obj --output-on-failure
```

> 生成器用 Ninja 或 MinGW Makefiles 都行。两者都已在 Windows x64 + MinGW GCC 13.1 上实测通过。

### 构建 UI

```powershell
cd D:\LIBS\ui
pwsh -File build.ps1            # 需要 Qt6 mingw_64（Widgets + Charts）
```

产物 `D:\LIBS\ui\build\libs-spectrum-ui.exe`，
Qt 运行时由 `windeployqt` 拷到 `D:\LIBS\ui\build\qt\`。

### 跑起来

```powershell
# 1. 出一份全设备关闭的配置模板，按现场改端口 / 序列号 / SDK 路径
.\src\build\businessd.exe --print-default-config > site.json

# 2. 只做结构检查，不开串口
python .\src\remote\lpc.py config validate --config site.json

# 3. 离线自检：不打开任何设备，只跑纯软件能力
.\src\build\businessd.exe --no-devices

# 4. 正常起守护进程（独占设备）
.\src\build\businessd.exe --config site.json --data-dir .\data

# 5. 另开一个终端发命令
python .\src\remote\lpc.py status --pretty
python .\src\remote\lpc.py gate --args '{"action":"arm"}' --pretty
```

---

## 对外命令面

`businessd` 暴露 **14 条命令**，全部返回 JSON，成功失败都返回，错误在 `error` 字段里。
客户端不需要区分"传输失败"和"业务失败"。

| 命令 | 作用 |
| --- | --- |
| `ping` | 存活探测，返回可用命令列表 |
| `config_show` | 回显**内存里正在用的**配置 |
| `status` | 门禁 + 阶段 + 设备健康（**不宣称物理同步**） |
| `gate` | 门禁判定与 arm/disarm，唯一判定入口 |
| `param_set` / `param_get` | 测量参数，未齐备时报 `parameters_incomplete` |
| `capture` | 一次性光谱采集（多通道） |
| `measurement_start` / `measurement_stop` / `measurement_status` | **连续测量作业**：循环出光 → 每发被光谱仪采到 → 落盘 → 实时显示。start 返回 `settle_ms`；status 返回采集/失败/落盘成功/落盘失败/待写盘计数 |
| `spectrum_stream` | 光谱帧的**订阅**通道（subscribe / unsubscribe / latest / status）。**没有 start/stop**——它不出光、不采集；帧只有一个来源：测量作业 |
| `recovery_list` / `recovery_write` | 恢复快照，只写不自动续跑 |
| `health` | 设备健康，设备没开也能调 |

> **出光只有 `measurement_start` 一条路径。** 原先的
> `laser_fire` / `laser_run` / `laser_stop` 已删除——
> 它们让判定出现了第二个执行点。详见
> [docs/architecture-report.md](docs/architecture-report.md) §4。

---

## 测试

12 套离线测试，**1084 个断言全部通过**——不开真实 COM 口、不加载真实 AvaSpec DLL、不发光。

| 测试套 | 断言 | 覆盖 |
| --- | ---: | --- |
| `test_protocols` | 124 | 帧构造、CRC、极性、噪声定位 |
| `test_devices` | 43 | 三类串口设备，假传输验证收发与错误传播 |
| `test_air_conditioner_protocol` | 114 | FC01/03/05/06 编解码，用厂商手册 §7 实测帧做向量 |
| `test_air_conditioner_device` | 60 | 空调事务、范围校验、错误传播 |
| `test_device_actions` | 52 | **设备动作序列、读回确认、保活、激光器安全禁区** |
| `test_spectrometer` | 216 | SDK 加载、ABI 断言、**触发方向**、FPGA 换算、超时 |
| `test_business_core` | 90 | 配置、JSON、门禁、参数、日志（每条红线一个可执行断言） |
| `test_business_services` | 41 | 采集节奏、采集同步视图、交付出口、状态灯同步 |
| `test_business_facade` | 154 | 命令面失败形状：坏 JSON/缺字段/未知命令 |
| `test_business_ipc` | 31 | 命名管道协议（不真的建管道） |
| `test_frame_queue` | 68 | **保序、丢最旧帧背压、订阅者互不拖累、丢帧不移生产者帧号** |
| `test_measurement_job` | 91 | **门禁拦截每一条、每发必被采到、慢盘不反压采集、停止幂等且确认收光** |

> **手动运行 `test_spectrometer.exe` 必须显式给替身 DLL 路径**
> （argv[1] 或环境变量 `LIBS_FAKE_AVASPEC_DLL`），否则报
> `could not stage the stand-in DLL`。`build.ps1` 已代为传入。

光谱仪测试用的替身 DLL（`D:\LIBS\src\tests\fake_avaspec_sdk.cpp`）
**是测试夹具，不是厂家 DLL 的替代品**。

**离线测试不能替代真机验证。** 出光时序曾照协议文本推出来、1000+ 条断言全绿，
而真机上一次都没出过光，还把机器配置写坏了。真机调试必须分级推进
（只读 -> 单设备 -> 单个物理动作 -> 全流程）。

---

## 平台要求

- Windows x64（传输层用 Win32 COM，光谱仪用 `__stdcall`）
- C++17
- MinGW GCC 13+ 或 MSVC
- 编译零警告（`-Wall -Wextra -Wpedantic`）
- UI 额外需要 Qt 6 的 **mingw_64** 发行版（Widgets + Charts）

后端零第三方依赖；UI 只依赖 Qt6。`lpc.py` 只用标准库 `ctypes`，无需 pywin32。

---

## 硬件资料

`device-references/` 存放厂家协议手册与 SDK，是所有驱动代码的依据来源：

```text
laser/              风冷激光器 RS232 协议
rangefinder/        TOF101 Modbus 协议与寄存器说明
serial-devices/     三色灯协议、串口测试工具、USB/485 驱动
spectrometer/       AvaSpec 9.14.0.0 SDK（DLL、示例、手册）
air-conditioning/   空调协议 V2（RS485 / Modbus RTU 变体）
```

**设备协议不靠类比推断。** 重构过程中，报警灯曾被误当作标准 Modbus FC01，
实际是厂商私有 8 字节格式（寄存器会回显），与测距仪恰好相反——这类细节只有
以厂商文档为准才不会出错。详见 `docs/refactor-report.md` 第 7 节。

### 各设备对应的接口

端口映射属于**业务层的站点配置**，驱动层不硬编码、不做端口发现。
按交接文档约定的这套映射（**必须写进 `site.json`**）：

| 设备 | 接口 | 参数 | 备注 |
| --- | --- | --- | --- |
| 激光器 | COM10 | 38400 / 8N1 | **必须显式写 `"baud": 38400`**，见"已知缺口"第 1 条 |
| 测距仪 | COM7 | 9600 / 8N1 | 从站 1 |
| 状态灯 | COM8 | 9600 / **8E1** | 从站 1 |
| 空调 | COM12 | 9600 / 8N1 | 从站 1 |
| 光谱仪 x4 | USB | 序列号 `1706097U4` ~ `1706100U4` | 各 2048 px |

### 空调的协议特例

空调是唯一一个"协议不是标准 Modbus"的设备：它沿用 Modbus RTU 帧格式，
但寄存器号在手册里是 5 位的**文档地址**（10001 / 30001 / 40001），
报文里出现的是**从 0 开始的偏移量**。换算基准由功能码决定，
本库对外一律用文档地址，偏移换算封装在协议层内部。

| 功能码 | 含义 | 报文里的起始偏移 | 数量 |
| --- | --- | --- | --- |
| `0x01` | 读运行状态位 | 文档地址 - 10001 | 每个寄存器 1 位，bit0 在前 |
| `0x03` | 读运行参数 | 文档地址 - 30001 | 每参数 2 字节高字节在前，单次最多 50 |
| `0x05` | 写状态位（开关机） | 文档地址 - 10001 | 1 个线圈值 |
| `0x06` | 写运行参数 | 文档地址 - 40001 | 1 个 16 位值 |

**厂商文档唯一的笔误**：手册写 `sReg = 寄存器号 - 00001`，
但厂商自己的样本里开关机用的偏移是 `0x0029 = 41 = 10042 - 10001`。
按 `00001` 换算会得到 10041，设备不会认。本实现**以实测样本为准**，
并在测试里用一条专门的用例把这个基准钉死。

两个**必须知道的设备行为**（本库不实现，只是转达）：

1. 模式控制（30026）必须**持续下发**，连续 100 s 收不到就自动切回自动运行。
   需要保活就得由业务模块周期调用 `set_mode`。
2. `10042` 开关机位在回读里是**低有效**（0 = 开机），与 FC05 写线圈
   `0xFF00` = 开的约定方向相反。协议层如实上报位值、不做反相。

---

## 已知缺口

以下是**已核实、但尚未修复**的代码与文档不一致，记录在此以免误判：

1. **激光器波特率默认值。** `SerialPortConfig::baud` 的兜底值是 **9600**
   （`D:\LIBS\src\include\libs\business\shared\config\site_config.hpp`），
   而现场约定是 **38400**。配置若省略 `laser.baud` 就会静默取到错值——
   与"缺配置不猜"的纪律相悖。**务必在 `site.json` 里显式写 `"baud": 38400`。**
2. **`lpc.py` 子命令落后于后端。** 它注册的 `detection_start` / `laser_fire` /
   `composition` 三条在 facade 里**已不存在**（调了会拿到 `unknown_command`）；
   而 `measurement_start` / `measurement_stop` / `measurement_status` /
   `spectrum_stream` 四条**没有**注册，命令行调不到。
   UI 走 `PipeClient` 不受影响，但命令行只能靠 `--args` 手工发 JSON。
3. **激光 TEC 写指令未实现。** 手册第 4.1 节列出 `0x80`~`0x89` 一组 TEC 设置命令，
   `kReadCommands` 目前只有 `0xB1` / `0xB2` / `0xBC`。这与
   `devices/laser.hpp` 声明的"TEC 只读"安全禁区一致。
4. **`0xBC` 载荷长度存疑。** `D:\LIBS\src\src\protocols\laser.cpp` 的
   `payload_length(0xBC)` 返回 **20**，手册第 4.2 节记的是 **22** 个数据字节。
   且 `0xBC` 目前没有解码测试。**以真机实测为准，不要据代码推断。**
5. **`ui/tests/stream_probe.cpp` 没有进任何构建脚本**（`ui/CMakeLists.txt` 与
   `ui/build.ps1` 都不含它），需要手工编译才能跑。

---

## 进一步阅读

| 文档 | 内容 |
| --- | --- |
| **[docs/real-device-parameters-20260929.md](docs/real-device-parameters-20260929.md)** | **2026-09-29 真机参数记录：激光器 80A/200us/1Hz/Q延时 210us，光谱仪 1.28us 积分延时 / 1.05ms 积分时间** |
| **[docs/architecture-report.md](docs/architecture-report.md)** | **架构报告：两进程三层全貌、逐文件职责、横切关注点、五条硬约束与设计判断** |
| **[docs/ui-modification-plan.md](docs/ui-modification-plan.md)** | **UI 修改方案：收敛为「三按钮 + 实时光谱 + 设备状态条」主页面与独立参数页，并指出设备预检缺失的命令出口** |
| [docs/business-layer-two-businesses-execution-plan.md](docs/business-layer-two-businesses-execution-plan.md) | 两条业务（readiness + measurement）的执行方案 |
| [docs/business-layer-scope-report.md](docs/business-layer-scope-report.md) | 12 项能力的范围裁剪与 A/B/C 分级 |
| [docs/device-layer-consolidation-implementation-report.md](docs/device-layer-consolidation-implementation-report.md) | 设备层收敛：单一控制器边界、迁移步骤、安全禁区、验收标准 |
| [docs/device-gaps-and-continuous-fire-plan.md](docs/device-gaps-and-continuous-fire-plan.md) | 设备缺口与连续出光方案 |
| [docs/refactor-report.md](docs/refactor-report.md) | 分层重构：依赖约束、已发现缺陷、已知限制 |
