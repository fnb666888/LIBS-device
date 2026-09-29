# LIBS 业务层架构与业务流程说明

> 本文档描述 `D:\LIBS` 当前代码的实际状态，不是设计愿望书。
> 文中"已实现 / 未实现"均以 `D:\LIBS\src` 里能跑通并有测试覆盖的代码为准。
> 撰写日期：2026-09-29

---

## 1. 一句话概括

这是一个**四层单向依赖**的 C++17 工业设备控制库：
最底下是"把字节翻译成数据"的协议层，往上是串口和厂商 SDK，
再往上是把设备动作编排成业务流程的业务层，最外面是一个独占设备的守护进程，
通过命名管道对外提供 JSON 接口。

**判断权全部集中在业务层。** 驱动层只负责"你让我发这些字节，我发出去并如实带回"，
绝不自己决定"现在能不能出光"。

---

## 2. 目录与分层

```text
D:\LIBS
├─ src\                      ← 唯一在用的源码
│  ├─ include\libs\
│  │  ├─ error.hpp            Result<T> / Error / 错误分类
│  │  ├─ hex.hpp              字节 ↔ 十六进制文本
│  │  ├─ protocols\          第 3 层：纯协议，零 I/O、零状态
│  │  ├─ transport\          第 2 层：串口收发与超时
│  │  ├─ devices\            第 1 层：设备驱动 + 厂商 SDK 适配
│  │  ├─ business\           第 4 层：业务语义（本次重点）
│  │  │  ├─ config\          站点配置与端口映射
│  │  │  ├─ session\         设备独占会话与生命周期
│  │  │  ├─ policy\          门禁、联锁、状态指示
│  │  │  ├─ services\        各业务能力模块
│  │  │  ├─ logging\         运行日志与恢复快照
│  │  │  ├─ json\            自带 JSON 实现（无第三方依赖）
│  │  │  └─ app\             依赖装配与生命周期
│  │  └─ businessd\          守护进程 IPC 契约
│  ├─ src\                    实现（与 include 一一对应）
│  ├─ tests\                  10 套离线测试，共 869 个断言
│  ├─ CMakeLists.txt           CMake 入口
│  └─ build.ps1               免 CMake 一键编译+跑测试
├─ device-references\         厂家协议手册与 SDK（驱动的依据来源）
└─ docs\                      范围报告、方案文档、本文档
```

### 依赖方向（严格单向，反向零引用）

```
  businessd (main.cpp)                 可执行入口
        │
  business/app        Runtime          依赖装配、生命周期
        │
  business/services   12 个能力模块     业务编排
        │
  business/policy     Gate / 联锁       唯一的安全判定入口
        │
  business/session    DeviceSession    进程内唯一设备持有者
        │
  devices             驱动 + SDK       翻译成结构化数据
        │
  transport           串口收发          只搬字节，不懂协议
        │
  protocols           帧编解码          纯函数，无 I/O
```

**约束被编译器和测试同时钉死**：`CMakeLists.txt` 里
`devices -> transport -> protocols` 是单向 `target_link_libraries`，
业务层只允许依赖聚合目标 `libs`。想反向引用，链接期就会失败。

---

## 3. 各层职责

### 3.1 protocols —— 纯协议层

只有字节和值语义，**不碰串口、不碰 SDK、不含任何安全策略**。

| 文件 | 内容 |
| --- | --- |
| `laser.hpp/cpp` | 激光器私有协议帧（0xB2 状态、0x11~0x19 参数、0x21/0x24/0x31/0x34） |
| `rangefinder.hpp/cpp` | 测距仪 Modbus 功能码 0x03 |
| `indicator.hpp/cpp` | 报警灯通道与灯态 |
| `air_conditioner.hpp/cpp` | 空调协议 V2（Modbus RTU 变体） |
| `modbus_rtu.hpp/cpp` | Modbus RTU CRC 与帧骨架 |

### 3.2 transport —— 传输层

Win32 COM 口收发，带超时。**不理解任何协议内容。**

### 3.3 devices —— 设备驱动层

把传输结果或厂商 SDK 的原始返回翻译成结构化数据。

| 设备 | 关键点 |
| --- | --- |
| `Laser` | 激光器；换算与安全位解码在协议层，驱动只负责收发 |
| `Rangefinder` | 测距仪；协议层 `valid=true` 默认是"本层不判断"，**不是**"这一读有效" |
| `Indicator` | 状态灯 |
| `AirConditioner` | 空调；驱动只跑事务，**不做超温保护策略** |
| `Spectrometer` | AvaSpec 光谱仪；动态加载 `avaspecx64.dll`，FPGA 计数换算，触发模式配置 |

光谱仪驱动里有一处**类型层强制**的结构体 ABI 校验：
`AvsIdentity` 75 字节、`MeasureConfig` 41 字节、`trigger` 在偏移 22、`control` 在 25。
这些是 `static_assert` 写死的，SDK 版本一变编译就红。

### 3.4 business —— 业务层

本次的核心。四组子层：

- **config**：站点配置。端口、波特率、超时、设备使能全部来自配置文件，**不硬编码**。
  字段缺失或非法一律 `invalid_argument`，**禁止静默回落默认值**——回落意味着会开错设备、连错口。
- **session**：`DeviceSession` 是**进程内唯一**持有设备对象的地方，其他地方只拿引用。
  设备独占性由三门保证：串口 `share=0`、AvaSpec 命名互斥体、全库同步阻塞。
- **policy**：`Gate` 是**唯一的安全判定入口**。状态灯映射是它的纯函数输出，不反向参与判定。
- **services**：12 个能力模块，见下节。
- **app**：`Runtime` 持有全部依赖，负责打开设备、串门禁事实源、退出时优雅关闭。

### 3.5 businessd —— 守护进程

- 进程模型：C++ 守护进程**独占设备**，Python CLI 经命名管道通信。
- 协议：一次请求一行 JSON，一次响应一行 JSON（JSON Lines）。
- 并发：每连接一线程，但**设备访问串行化**（`Runtime::device_mutex`），
  因为驱动是同步阻塞的，多线程交错事务会串帧。
- 安全：管道 ACL 限定当前用户。设备是独占的，管道也应如此。

---

## 4. 12 个业务能力模块

对应 `docs/business-layer-scope-report.md` 裁剪后的 12 项方案。A = 可直接实施，
B = 有卡点，C = 未确认前提，返回 `unavailable`。

| # | 能力 | 落地文件 | 状态 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | 站点配置与端口映射 | `config/site_config.*` | A 已实现 | 全字段来自配置，缺失即报错 |
| 2 | 设备独占会话与生命周期 | `session/device_session.*` | A 已实现 | 三门全过 |
| 3 | 核心参数管理 | `policy/detection_config.*` + `ParameterStore` | A 已实现 | 门禁的 `parameters_ready` 由它驱动 |
| 4 | 门禁、联锁与状态指示 | `policy/gate.*` | A 已实现 | **全库安全价值最高的一项** |
| 5 | 业务事件模型 | `services/business_event.*` | A/B 已实现 | "开始检测"等事件类型与适配器契约 |
| 6 | 数据交付出口 | `services/data_delivery.*` | A 已实现 / 网关 C | 本地保存**已接入两条采集路径**（`capture` 与 `laser_fire`，共用 `deliver_capture`）；网关交付无协议，只回前置条件 |
| 7 | 运行日志与恢复快照 | `logging/run_log.*` | A 已实现 | JSONL 日志 + 恢复上下文 |
| 8 | 测距与物料判定 | `services/ore_presence.*` | A/B 部分 | 测距已实现；有矿阈值未标定，不填 |
| 9 | 环境采集与联锁 | `services/environment.*` + `ac_interlock.*` | A 已实现 / 联锁 C | 采集三态；联锁阈值未定，**不拦截** |
| 10 | 光谱采集协调 + **激光出光** | `services/capture.*` + `services/laser_fire.*` | A 已实现 | 核心业务，见第 6 节 |
| 11 | 成分分析 | `services/composition.*` | B 接口已实现 / 算法 C | 无算法时返回 `unavailable`，double 字段无假默认值 |
| 12 | 运动与位置切换 | `services/motion.*` | B 部分 / C | 行程时间未标定，位置切换无驱动入口 |

---

## 5. 对外命令面（businessd 门面）

13 条命令，全部返回 JSON，成功失败都返回，错误在 `error` 字段里。
客户端不用区分"传输失败"和"业务失败"。

| 命令 | 作用 | 关键行为 |
| --- | --- | --- |
| `ping` | 存活探测 | 返回 `running` 与可用命令列表 |
| `config_show` | 回显配置 | 返回**内存里正在用的**配置，不是磁盘文件 |
| `status` | 运行状态 | 门禁 + 阶段 + 设备健康；**不宣称物理同步** |
| `gate` | 门禁判定 | 唯一判定入口，逐条列出拦截原因 |
| `param_set` / `param_get` | 核心参数 | 未齐备时报 `parameters_incomplete`，**不假装已生效** |
| `detection_start` | 开始检测 | **只是业务信号，不含"已出光"或"已触发"含义** |
| `capture` | 光谱采集 | 按配置构造请求；无设备返回 `unsupported` |
| `laser_fire` | **出光 + 采集编排** | 见下节 |
| `composition` | 成分结果 | 无算法返回 `unavailable`，不是零值 |
| `recovery_list` / `recovery_write` | 恢复快照 | 只写不自动续跑 |
| `health` | 设备健康 | 不抛异常，设备没开也能调 |

### 关键安全语义

- `detection_start` 成功 **≠** 激光已出光。它只表示"业务层允许进入检测流程"。
- `gate` 的 `allowed=true` **≠** 物理触发已发生。
- `capture` 返回成功 **≠** 多通道物理同步。`ScanGroup::hardware_synchronized`
  必须原样上送，不得由"我同时请求了 4 个通道"推断为"物理上已同步"。

---

## 6. 核心业务流程：激光触发光谱仪

方向依据 AvaSoft 手册 §4.9 **接法二**：
**激光器 TTL 输出接光谱仪 pin 6，光谱仪跑外触发模式等这个脉冲。**

> **2026-09-29 真机订正。** 本节原先描述的是"0x34 单次触发"时序，那是照协议文本
> 推出来的，**真机上一次都没出过光**。实际可用的时序来自厂家工具的运行日志，
> 见 6.1。协议文本描述设备能做什么，厂家日志描述它实际被怎么用；冲突时以日志为准。

### 6.1 时序

```text
门禁放行（含"出光参数是否已标定"）
    → 读 0xB2 存基线（含放电计数器起点）
    → 光谱仪后台线程 arm 到"等 pin 6 TTL"
    → 写会话参数（0x11/0x12/0x13/0x14/0x16/0x19，值来自配置）
    → 0x21 工作=1
    → **轮询 0xB2，等设备进入 01「正在工作且放电」**（先停在 02「自适应中」）
    → 0x24 光闸=1 → 保持 emission_hold_ms 连续出光
        → 每发激光 TTL 到 pin 6 → 光谱仪 +integration_delay 后开始积分
    → 0x24 收光 → 0x21 停机          （无条件执行）
    → 读 0xB2 确认：计数器增量 ≥ min_discharges、光闸确实关、工作确实停
```

两个线程操作的是**不同设备**（USB 光谱仪 vs 串口激光器），不争同一把锁。

**这里没有 0x34。** 协议文本把 0x34 写成"单次触发"，但厂家工具的整个日志历史里
**一次都没用过它**；它跑的是连续出光。本项目原先围绕 0x34 设计整个流程，真机零出光。

**自适应过程不是常数时长。** 设备使能后先停在 02「自适应中」，厂家日志里要
5~6 秒才转到 01「正在工作且放电」。固定 sleep 解决不了：太短则触发落空
（原实现只等 50 ms），太长则白等。所以只能轮询状态位。

### 6.2 参数来自配置，且必须已标定

出光参数（电流/脉宽/频率/放电延时/Q 延时/Q 预热）全部来自
`site.json` 的 `laser_fire` 段，**不再硬编码**。`commissioned=false` 时
门禁直接拦截，**一条写指令都不发**——与 `ac_interlock` 的"未标定不得启用"是同一套纪律。

为什么废掉了"取协议量程最小值"那套：**对限制类参数，量程下限意味着"禁止工作"，
而不是"最小能量"。** 实测后果是整台设备不出光：

| 参数 | 量程下限 | 实际含义 |
| --- | --- | --- |
| 0x17 电压限值 | 0 V | 输出钳死在 0，设备一使能就报 bit5 过压故障 |
| 0x16 Q 延时 | 2 us | 等于不调 Q，没有巨脉冲 |
| 0x11 放电电流 | 15 A | 泵浦能量只有正常的五分之一，低于出等离子体的阈值 |

现场标定值（厂家工具实测）：**80 A / 200 us / 1 Hz / Q 延时 210 us**。

### 6.3 机器配置只读，绝不写

**0x17 电压限值 / 0x18 Q 分频 / 0x15 温度保护**是整机标定值，存在设备 EEPROM 里
被所有工具共享。**厂家工具从不写这三个**（全日志核实），本程序同样只读不写。

**0x31 保存 EEPROM 也不在出光流程内。** 每打一发就往激光器 EEPROM 写一次参数，
任何一个写列表的错误都会被永久固化。保存是独立的、由操作员显式发起的动作。

> 现场事故记录：本项目曾把 0x17 写成 0 V 并在出光流程末尾执行了 0x31，
> 结果**整机不出光，连厂家工具都点不着**，最后靠人工按日志把参数写回才恢复。
> 这两条约束就是那次事故的直接产物，测试逐字节钉死。

### 6.4 六条纪律

1. **门禁是唯一判定入口**，本服务不自己判断能否出光。
2. **门禁不放行时一帧写指令都不许发**（测试逐条钉死）。
3. **失败必须中止到底**，"发了"不等于"成了"。
4. **收光/停机无条件执行**，光闸绝不能留在开的状态。
5. **绝不写机器配置**（0x17/0x18/0x15），**绝不在出光流程里写 0x31**。
6. **出光证据是放电计数器的增量**，不是"我发过指令"——计数器没涨够即判失败。

### 6.5 方向不得写反

`control.laser_width` / `laser_delay` 在本流程**恒为 0**。
那两栏是"光谱仪发 TTL 出去点火激光器"（接法一）。一旦填非零，
现场表现为**激光永远不出光、光谱仪一直等触发直到超时**。
该约束由 `tests/test_spectrometer.cpp` 逐字节钉死。

### 6.6 FPGA 换算（已确认，不再是猜）

逐字取自 SDK 示例 `qtdemo.cpp`：

| 参数 | 正向（ns → FPGA 计数） | 反向 |
| --- | --- | --- |
| 积分延时 | `6.0 * (ns + 20.84) / 125.0` | `125.0 * (计数 - 1) / 6.0` |
| 激光延时/宽度 | `6.0 * ns / 125.0`（**无偏置**） | `125.0 * 计数 / 6.0` |

125/6 = 20.833 ns，即手册说的 21 ns 步进。
积分延时**有**偏置、激光延时**没有**，两个方向必须成对使用。

默认配置：`wait_for_laser_trigger = true`，`integration_delay_ns = 5000`（5 us）。
手册给的型号下限为 0.28 us（AvaSpec-3648-USB2）与 1.28 us（AvaSpec-2048-USB2），
5 us 两者都满足；现场按样品标定。

---

## 7. 贯穿全库的工程纪律

这些不是口号，每一条都有对应测试：

| 纪律 | 落地方式 |
| --- | --- |
| 未知 ≠ 正常 | `AcSnapshot::availability` 是三态枚举 `available/stale/unavailable`，类型层没有两态"正常/异常" |
| 缺配置不猜 | `WorkFrequency` 的值是 `optional`，缺失返回 `configured=false`，**不回落某个 Hz** |
| 不用假默认值 | 成分分析 10 个字段全 `optional<double>`，无算法时 `unavailable` |
| 安全位极性只有一处转换 | `policy::parse_actual_frame` 是 0xB2 → 安全位的**唯一**转换点（曾发生两处都写反的缺陷） |
| 4 字节计数器不被截断 | `Field` 加 `raw32`/`wide`，放电计数器用 `quantity32()` 工厂 |
| 子项不得互相代偿 | 空调联锁（3.9.2 C 类）未标定前，门禁里那个拦截点**必须保持不拦截** |
| 业务信号 ≠ 物理触发 | `DetectionStartRequested` 不携带任何"已出光"含义 |

---

## 8. 测试现状

11 套离线测试，**1030 个断言全部通过**，约 45 秒编完并跑完。

| 测试套 | 断言 | 覆盖面 |
| --- | --- | --- |
| protocols | 124 | 帧构造、CRC、极性 |
| devices | 55 | 驱动收发、超范围拒发 |
| air_conditioner_protocol | 114 | 寄存器映射与功能码 |
| air_conditioner_device | 61 | 空调事务 |
| spectrometer | 191 | **触发方向、FPGA 换算、超时** |
| business_core | 96 | 配置、门禁、日志（含出光参数标定校验） |
| business_services | 100 | 各能力模块 |
| business_facade | 116 | 命令面（含 `laser_fire` 必须可见） |
| business_ipc | 31 | 命名管道协议 |
| laser_fire | 85 | **连续出光时序、机器配置只读、计数器判据、失败收光** |
| spectrum_stream | 57 | 实时光谱流纪律与背压 |

**全部离线**。串口用替身，光谱仪用替身 DLL（`fake_avaspec_sdk.cpp`）。

**离线测试不能替代真机验证。** 2026-09-29 的现场事故就是这句话的代价：出光时序
照协议文本推出来、1000+ 条离线断言全绿，而真机上一次都没出过光，还把机器配置写坏了。
真机调试必须分级推进（只读 → 单设备 → 单个物理动作 → 全流程），并且在碰任何
会写 EEPROM 的路径之前，先用厂家工具建立"已知可行"的基线。

---

## 9. 要让全流程真跑起来，还差什么

按优先级：

1. **接线**：激光器 TTL 输出 → 光谱仪 **pin 6**。这是物理事实，代码救不了。
   手册上激光器有没有独立同步输出端子（还是只有 Q 闸输出）需现场确认。
2. **激光器出光**：需要用 AvaSoft 自带软件或激光器厂家工具先验证一次
   "0x34 能真的打出一发光"，再谈本流程。
3. **设备配置**：把 `--print-default-config` 的输出改成现场实际的 COM 口、
   序列号、SDK 绝对路径，然后 `config validate`（不开串口）。
4. **成分算法**：现在是 `unavailable`。要出真实成分结果，得实现算法或接外部模型。
5. **有矿阈值 / 空调联锁阈值 / 位置切换**：三项都因卡点未标定而返回 `unavailable`，
   前置条件都写在各自头文件顶部。

---

## 10. 怎么构建与自检

```powershell
# 一键编译 + 跑全部测试
cd D:\LIBS\src
$env:PATH="D:\Qt6\Tools\mingw1310_64\bin;$env:PATH"
pwsh -NoProfile -File build.ps1

# 只编译不跑测试
pwsh -NoProfile -File build.ps1 -SkipTests

# 出一份默认配置模板
.\build\businessd.exe --print-default-config

# 离线自检：不打开任何设备
.\build\businessd.exe --no-devices
```

环境要求：Windows x64（传输层用 Win32 COM，光谱仪用 `__stdcall`）、
C++17、MinGW g++（或 MSVC，走 CMake）。

---

## 附录 A：关键文件速查

| 想知道 | 看这个 |
| --- | --- |
| 怎么构建 | `D:\LIBS\src\build.ps1` |
| 分层与依赖 | `D:\LIBS\src\CMakeLists.txt` |
| 有哪些命令 | `D:\LIBS\src\src\business\services\facade.cpp` 的 `handle()` |
| 出光序列 | `D:\LIBS\src\src\business\services\laser_fire.cpp` |
| 门禁规则 | `D:\LIBS\src\include\libs\business\policy\gate.hpp` |
| 安全位极性 | `D:\LIBS\src\src\business\policy\gate.cpp` 的 `parse_actual_frame` |
| 光谱触发配置 | `D:\LIBS\src\src\devices\spectrometer.cpp` 的 `capture()` |
| FPGA 换算 | `D:\LIBS\src\include\libs\devices\spectrometer.hpp` 的 `FpgaTime` |
| 12 项能力的前身 | `D:\LIBS\docs\business-layer-scope-report.md` |
