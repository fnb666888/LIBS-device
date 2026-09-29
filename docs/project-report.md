# LIBS 激光诱导击穿光谱系统 — 项目报告

- 报告日期：2026-09-29
- 代码基线：`D:\LIBS` 工作区当前状态
- 报告范围：架构现状、能力边界、测试基线、缺陷复盘、未验证风险

---

## 1. 项目概述

LIBS 是一套面向**激光诱导击穿光谱（LIBS）在线检测工位**的工控软件。系统围绕一门固定工位的检测流程组织：门禁与安全联锁 → 设备会话建立 → 触发出光 → 光谱采集 → 实时曲线预览 → 结果落盘与上报。

系统的核心设计取向是**"没有把握就说没有"**：所有尚未具备前置条件的业务能力，都以显式错误或 `null`（未知）返回，绝不返回伪装成成功的假值。这一条贯穿全部业务层设计，测试里有专门的用例守着它（`local_sink_failure_does_not_claim_success`、`ore_judge_failed_read_is_unknown`）。

最新加入的模块是**实时光谱流（`spectrum_stream`）**，用于采集期间以固定节奏推送多通道光谱帧给前端实时绘图。详细设计见 `D:\LIBS\docs\realtime-spectrum-stream.md`。

### 1.1 技术栈

| 项 | 选择 |
|---|---|
| 语言标准 | C++17 |
| 构建 | PowerShell 7 脚本 + MinGW-w64 GCC 13.1（`D:\Qt6\Tools\mingw1310_64`） |
| IPC | Windows 命名管道 + 手写 JSON |
| 序列化 | 自研轻量 JSON（`src/business/json/`） |
| 前端 | Qt 6 Widgets + Qt Charts（独立进程） |
| 远程脚本 | Python（`src/remote/lpc.py`） |
| 第三方 C++ 依赖 | **零** |

零第三方 C++ 依赖是一个刻意的选择：整套系统只依赖 Windows 系统库和 Qt（仅前端），编译链因此极短，工控机上部署只需拷贝 exe + 静态库 + 替身 DLL。代价是 JSON、串口、管道服务器都是手写代码，需要靠测试覆盖来保证正确性。

---

## 2. 架构

### 2.1 四层单向依赖

```
  ┌──────────────────────────────────────┐
  │  libs_protocols      891 行 / 5 模块  │  帧编解码，纯函数，零 I/O
  │  Modbus RTU + 激光/测距/指示灯/空调   │
  └───────────────┬──────────────────────┘
                  ↓
  ┌──────────────────────────────────────┐
  │  libs_transport      193 行 / 1 模块  │  串口收发、超时控制
  └───────────────┬──────────────────────┘
                  ↓
  ┌──────────────────────────────────────┐
  │  libs_devices        983 行 / 5 驱动  │  AvaSpec 光谱仪(631) + 空调 157
  │                                       │  + 激光 91 + 指示灯 67 + 测距 37  │
  └───────────────┬──────────────────────┘
                  ↓
  ┌──────────────────────────────────────┐
  │  libs_business      4138 行 / 19 模块 │  门禁/联锁/采集/出光/实时流/
  │                                       │  配置/日志/JSON                      │
  └───────────────┬──────────────────────┘
                  ↓
  ┌──────────────────────────────────────┐
  │  businessd           326 行 / 2 文件  │  命名管道 JSON 接口（守护进程）
  └───────────────┬──────────────────────┘
                  ↑  仅 JSON over 命名管道
  ┌──────────────────────────────────────┐
  │  ui/  (Qt6)        742 行 / 9 文件   │  纯管道客户端，零链接后端
  └──────────────────────────────────────┘
```

### 2.2 代码量统计

| 层 | 源文件 | 头文件 | 行数 |
|---|---|---|---|
| `libs_protocols` | 5 | 5 | 891 |
| `libs_transport` | 1 | 1 | 193 |
| `libs_devices` | 5 | 5 | 983 |
| `libs_business` | 19 | 19 | 4138 |
| `businessd` | 2 | 1 | 326 |
| **后端合计** | **32** | **31** | **6531** |
| `ui`（Qt6） | 5 | 4 | 742 |
| `tests` | 12 | 2 | 4412 |
| **总计** | **49** | **37** | **11685** |

另有 `src/check.{hpp,cpp}`（测试框架，123 行）与 `src/fake_avaspec_sdk.cpp`（替身 SDK，135 行）位于 `src/` 根，不计入上表的层统计。

代码按功能域分布（`libs_business` 内部）：

| 域 | 行数 | 说明 |
|---|---|---|
| `services/` | 2070 | 门面、采集、出光、实时流、空调联锁、运动、矿判等 |
| `config/` | 635 | 站点参数、检测配置、门禁策略 |
| `json/` | 436 | 自研 JSON 值类型与解析器 |
| `app/` | 374 | 运行时对象图组装与生命周期 |
| `session/` | 327 | 设备会话（打开/关闭/健康） |
| `logging/` | 329 | 运行日志 |

### 2.3 分层红线：**已验证无违规**

对每层源文件与头文件逐文件扫描 `#include`，检查是否存在对上层或同级更高层的引用：

| 检查项 | 违规数 |
|---|---|
| `protocols` 引用 `transport`/`devices`/`business` | 0 |
| `transport` 引用 `devices`/`business` | 0 |
| `devices` 引用 `business` | 0 |
| `libs_devices` 头文件引用 `business` | 0 |
| `ui` 引用 `libs/` 任何内容 | 0 |

**UI 零链接后端**是其中最关键的一条。UI 的全部 `#include` 只有 Qt 类库和自身 4 个头文件：

```
QApplication QMainWindow QWidget QVBoxLayout QPushButton QLabel QStatusBar
QMenuBar QToolBar QAction QMessageBox QCloseEvent
QChart QChartView QLineSeries QValueAxis
QJsonObject QJsonArray QJsonDocument QString QByteArray QVector QDir QFile
QProcessEnvironment QTimer QObject  +  <windows.h>
```

理由是 C++ 的头文件依赖具有传染性：`libs_business` 公开链接 `libs_devices`，UI 一旦 include 业务头文件，链接命令行上就会同时出现设备层，物理上绕过门禁直连硬件。UI 只能看到管道里的 JSON，这是架构上的**强制**而非约定。

---

## 3. 能力清单

### 3.1 管道命令（14 个）

守护进程 `businessd` 通过命名管道（默认 `\\.\pipe\libs-businessd`）接受 JSON 请求，格式为 `{"command": "<名字>", ...args}`，返回 `{"ok": true, ...}` 或 `{"ok": false, "error": {...}}`。

| 命令 | 作用 |
|---|---|
| `ping` | 连通性探测 |
| `config_show` | 读取站点配置 |
| `status` | 全量运行状态快照（含不可用能力清单） |
| `gate` | 门禁布防/撤防/查询 |
| `param_set` | 写参数 |
| `param_get` | 读参数 |
| `detection_start` | 启动一次检测流程 |
| `capture` | 单次光谱采集 |
| `laser_fire` | 触发出光 |
| `spectrum_stream` | 实时光谱流（预览/停止/配置） |
| `composition` | 成分分析（**仅数据结构，无算法**） |
| `recovery_list` | 恢复记录列表 |
| `recovery_write` | 写入恢复记录 |
| `health` | 设备健康巡检 |

> 注：摘要早前版本记为 13 个，实际 `facade.cpp` 的 `commands()` 分派表中有 14 个（`laser_fire` 常被漏数）。

### 3.2 业务域

- **安全**：门禁布防状态机、空调联锁（未标定前不参与门禁）、出光安全互锁
- **检测流程**：设备会话建立 → 触发 → 采集 → 落盘 → 上报
- **实时流**：`spectrum_stream` 采集侧后台线程 + 环形缓冲 + 背压丢帧
- **配置与日志**：站点参数热读写、运行日志、恢复记录

### 3.3 明确做不了的事

`status` 命令的 `unavailable_capabilities` 字段会自报以下 5 项，这是接口的一部分，不是 TODO 注释：

| 能力 | 状态 |
|---|---|
| `compose_algorithm` | 只有结果数据结构与可用性判断，**无成分反演算法** |
| `position_switch` | `UnavailablePositionSwitch` 桩，未实现 |
| `gateway_delivery` | `UnsupportedGatewaySink` 桩，**无网络层**，结果只能本地落盘 |
| `gateway_signals` | `192.168.101.100` 是候选 IPC 地址，无网络层不会写 |
| `ac_interlock` | 需现场标定后才能参与门禁判定 |

### 3.4 已实现但未在真机验证的 3 项

另有 3 项属于"代码完成、测试通过，但从未在真实硬件或人工界面上验证过"，与上表的"未实现"性质不同，性质是**验证缺口**而非功能缺失。逐条说明见第 7.2 节。

---

## 4. 实时光谱流专题

### 4.1 定位

`D:\LIBS\docs\realtime-spectrum-stream.md`（7.8 KB）是这个模块的完整设计文档。本节只做摘要，细节请直接读那份文档。

`spectrum_stream` 的作用：在采集进行期间，以固定节奏把各通道的光谱帧推送给 UI 实时绘图。它是一个**纯消费者**——自己不控制采集，只订阅 `Spectrometer` 已有的 pending 帧。

### 4.2 数据通路

```
AvaSpec 硬件
   ↓ AVS_PollScan（pending 槽）
Spectrometer::poll_frame()        ← devices 层
   ↓
SpectrumStream（后台线程，ring buffer）
   ↓ JSON 帧
businessd 命名管道
   ↓
ui/stream_poller（QTimer 轮询）
   ↓
QLineSeries 追加点 → QChart 重绘
```

### 4.3 五条设计纪律

1. **流只订阅，不驱动**——采集与预览解耦，预览故障不影响采集结果落盘。
2. **背压丢最旧帧**——UI 消费不过来时丢环形缓冲头部，而不是阻塞采集线程或无限增长。
3. **管道 accept 每连接一线程**——否则 UI 的 100 ms 轮询会踩 `ERROR_FILE_NOT_FOUND(2)` / `ERROR_PIPE_BUSY(231)`。
4. **`spectrum_stream_.configure()` 必须在 `devices_.open()` 之后**——否则 `health` 报"已打开"而流报 `spectrometer_not_open`，自相矛盾。
5. **三分层红线**——流逻辑只在 `business` 层，`devices` 层不知道"流"的存在，UI 不知道设备的存在。

### 4.4 端到端实测

用 `fake_avaspec_test.dll` 替身驱动三路（MASTER / SLAVE1 / SLAVE2）连续跑：

```
frames polled ok = 10/10      frame_index 24 → 33
distinct errors  = none       last_error_code = None
restart 0/1/2    -> running = True
```

10 帧全部取到，帧号连续递增，三次"停止 → 开始"重启均成功。`dropped_frames=31` 是背压按设计丢弃最旧帧的计数，**非缺陷**。

### 4.5 UI 侧实现

`D:\LIBS\ui\` 共 9 文件 742 行，核心是 4 个：

- `stream_poller.{hpp,cpp}` — `QTimer` 定时调 `spectrum_stream` 拉取帧
- `frame_model.{hpp,cpp}` — JSON 帧 → Qt 数据结构
- `pipe_client.{hpp,cpp}` — 命名管道 JSON 客户端
- `main_window.{hpp,cpp}` — `QLineSeries` 曲线渲染 + 免责横幅 + 失败置灰

关于"用公开库"的取舍：Qt Charts 是官方公开库，曲线渲染用它；JSON 解析在 UI 侧用 Qt 自带的 `QJsonDocument`，没有引入第三方 JSON 库。实时绘图如果后续要升级到更专业的形式（如瀑布图、热力图、多通道联动缩放），Qt Charts 够用；若需要频域分析（FFT），可再评估引入 `kissfft` 或 `pffft`——但**当前需求是时域强度-波长曲线，Qt Charts 已覆盖**，过早引入 FFT 库属于过度设计。

---

## 5. 测试与质量

### 5.1 基线

**11 个测试程序，995 个断言全部通过，0 失败。**（2026-09-29 实测，退出码均为 0）

| 测试程序 | 断言数 | 覆盖对象 |
|---|---|---|
| `test_spectrometer` | 191 | 光谱仪驱动：字节布局、句柄管理、采集、故障注入 |
| `test_protocols` | 124 | 四类协议编解码 + Modbus RTU |
| `test_air_conditioner_protocol` | 114 | 空调协议帧格式、校验、三态 |
| `test_business_facade` | 110 | 14 个管道命令的入参校验与错误路径 |
| `test_business_services` | 100 | 业务服务：采集、出光、矿判、运动 |
| `test_business_core` | 79 | 门禁策略、检测配置、运行时 |
| `test_laser_fire` | 73 | 出光时序与安全互锁 |
| `test_air_conditioner_device` | 61 | 空调设备驱动 |
| `test_spectrum_stream` | 57 | 实时流：启动/停止/重启/背压 |
| `test_devices` | 55 | 串口设备驱动通用行为 |
| `test_business_ipc` | 31 | 管道服务端并发 |
| **合计** | **995** | |

### 5.2 测试运行方式

`test_spectrometer` 需要先布置替身 DLL，通过 `LIBS_FAKE_AVASPEC_DLL` 环境变量传入：

```powershell
$env:PATH = "D:\Qt6\Tools\mingw1310_64\bin;$env:PATH"
$env:LIBS_FAKE_AVASPEC_DLL = 'D:\LIBS\src\build\fake_avaspec_test.dll'
cd D:\LIBS\src\build
.\test_spectrometer.exe          # → 191 checks passed
.\test_spectrum_stream.exe       # → 57 checks passed
```

> 不设这个变量会直接报 `could not stage the stand-in DLL` 并退出——这不是测试失败，是前置条件没满足。

### 5.3 自研测试框架

`src/check.hpp`（109 行）+ `src/check.cpp`（14 行）提供 `TEST_MAIN_BEGIN_ARGV()`、`check_eq` 等宏。相比 Catch2/gtest，代价是没有参数化测试和死亡测试；收益是零依赖、单文件头、输出格式可控（`OK tests/xxx.cpp: 191 checks passed`）。

---

## 6. 缺陷复盘

本章记录两个真实缺陷的根因分析。这两个 bug 都不是"写错了"，而是**状态机不对称**——这类 bug 最难查，因为症状与原因相隔很远。

### 6.1 缺陷一：`capture` 偶发超时（根因：设备句柄表只增不减）

**文件**：`D:\LIBS\src\src\devices\spectrometer.cpp`

**根因**：`Impl::devices` 曾是一个**只追加、从不清理**的数组。而 `enumerate()`（`health` 命令需要）和 `capture()` **都会**遍历并激活通道。在守护进程的长生命周期里，同一序列号被激活两次、两次拿到**同一个句柄**：

```
devices = [MASTER, SLAVE1, SLAVE2, MASTER, SLAVE1, SLAVE2]
```

这不是"多了一路数据"，而是**那一路被数了两遍**。`AVS_PollScan` 第一次就取走了该通道的 pending 帧，第二条同名记录永远等不到 → `timed out waiting for MASTER, SLAVE1, SLAVE2`，且**每帧递增**。

**定位线索**：`diag2.exe` 正常，因为它从不调用 `enumerate()`。同一套代码、同一个替身 DLL，只因为没走过 `enumerate()` 路径就不复现——这个反差直接指向了句柄表的状态污染。

**修法**（两条缺一不可）：

1. `activate()` 改为按序列号**幂等**：返回 `std::size_t` 索引，已存在则复用句柄，不再新建。
2. `capture()` 只操作**本次选中的通道**（新增 `active` 下标表），不把 `enumerate()` 留下的通道一起 arm。

**回归验证**：已用回退方式确认两个用例能复现原症状——第一个复现 `timed out waiting for MASTER, SLAVE1, SLAVE2`，第二次采集变六路同名；第二个复现 `timed out waiting for SLAVE1`。修复后两例通过。

**回归测试增量**：`test_spectrometer` 151 → **191**，新增 `capture_after_enumerate_does_not_double_activate_a_channel`、`capture_only_touches_the_channels_it_was_asked_for`。

### 6.2 缺陷二：实时流只能启动一次（根因：停止标志不复位）

**文件**：`D:\LIBS\src\src\business\services\spectrum_stream.cpp`

**根因**：`stop()` 置 `stopping_ = true` 但**从不复位**，而 `start()` 的守卫是 `if (!stopping_)`。于是：

- 第一次 `start()` → `stopping_` 为 false → 正常启动
- `stop()` → `stopping_ = true`
- 第二次 `start()` → 守卫判 false → **静默不启动**

**症状**：第二次点"开始预览"，`start()` 返回成功、不报任何错，但 `running` 一直 false、一帧都没有——按钮像坏的。`stop()` 侧"没在跑就直接返回"的短路把这个不对称盖住了，代码读起来完全合理。

**修法**：在 `start()` 里 `stopping_.store(false)` 复位。

**回归测试增量**：`test_spectrum_stream` 39 → **57**，新增 `stream_can_be_restarted_after_stop`、`start_while_running_is_a_noop`（后者守住"已在跑时重复 start 不重复启动"的另一侧语义）。

### 6.3 另外两个已记录的坑

- **管道 accept 循环**改为每连接一线程（原因见 4.3 第 3 条）。
- **`spectrum_stream_.configure()` 调用顺序**必须在 `devices_.open()` 之后（原因见 4.3 第 4 条）。

---

## 7. 能力边界与风险

本章是报告里最重要的一章。以下每一项都经过本次实测核对，**不夸大任何验证范围**。

### 7.1 明确未实现（5 项）

见 3.3 节的表格。`composition` 命令存在但无算法，`gateway_*` 系列无网络层。这些是接口占位与自报机制，不是缺陷。

### 7.2 未验证风险（3 条）

#### 风险 1（最高）：真机 SDK 不在，光谱仪驱动从未在真实硬件上跑过

```
Test-Path 'C:\Avantes\AvaSpecX64-DLL_9.14.0.0'  →  False
Test-Path 'C:\Avantes'                           →  False
```

当前全部 191 个光谱仪测试跑在 `fake_avaspec_test.dll` 替身上。驱动是**按 SDK 头文件的字节布局照抄**的——41 字节 `MeasureConfig`、75 字节 `Identity`，用 `static_assert` 把偏移钉死，替身 DLL 也严格按这个布局实现。字节层面是可信的。

但**行为层面从未验证**。上机第一步大概率要调 `AVS_PrepareMeasure` 的实测行为（超时时长、重复调用的副作用、错误码的实际取值）。这是当前最大的未知数。

#### 风险 2：4 个串口设备全是协议级实现

空调、激光、测距、指示灯四个驱动都只做到协议层——帧编解码正确、串口收发正确，但**没有真实设备联调**。现场需要验证：实际波特率与校验配置、各设备的应答时序（特别是空调三态上报的节奏）、异常断电后的重连行为。

#### 风险 3：UI 只编译通过，未做真人点按验证

数据通路用 Python 客户端跑通了（3 通道真实数据、10/10 帧），但**曲线渲染效果、免责横幅、失败置灰这些视觉行为从未被人看过**。Qt Charts 的坐标轴自动缩放策略、长时间运行的内存增长、多通道曲线的可读性，都需要真人在屏幕前确认。

### 7.3 建议的上机验证顺序

按风险从高到低、依赖从少到多：

1. **光谱仪单机验证**——装真 SDK，替换替身 DLL，`AVS_PrepareMeasure` 实测行为对齐（风险 1）
2. **UI 走查**——起守护进程 + 替身 DLL，真人点一遍预览（风险 3）
3. **串口设备逐个联调**——一次一个设备，先空调（最复杂、时序最密）（风险 2）
4. **全链路联调**——门禁 → 出光 → 采集 → 落盘，跑完整检测流程

---

## 8. 构建与部署

### 8.1 后端构建

```powershell
$env:PATH = "D:\Qt6\Tools\mingw1310_64\bin;$env:PATH"
cd D:\LIBS\src
pwsh -File build.ps1
```

> **必须用 `pwsh`（PowerShell 7），不能用旧版 `powershell`**——构建脚本依赖 PS7 语法。

产物在 `D:\LIBS\src\build\`：4 个静态库（`libs_protocols.a` / `libs_transport.a` / `libs_devices.a` / `libs_business.a`）、`businessd.exe`、11 个测试 exe、替身 DLL。

### 8.2 UI 构建

```powershell
$env:PATH = "D:\Qt6\Tools\mingw1310_64\bin;$env:PATH"
cd D:\LIBS\ui
pwsh -File build.ps1
```

### 8.3 部署清单

| 文件 | 用途 |
|---|---|
| `businessd.exe` | 守护进程（需静态库同时部署） |
| `libs_*.a` × 4 | 静态库 |
| `fake_avaspec_test.dll` | 开发期替身；**上机前替换为真 SDK 的 `AvaSpecLib64.dll`** |
| UI 可执行文件 | 需 Qt6 运行时（Widgets + Charts） |

> 编译或删除产物前先确认 `businessd.exe` 未在运行——Windows 会锁住 `fake_avaspec_test.dll`，构建报 `cannot open output file ... Permission denied`。必要时 `Get-Process businessd | Stop-Process -Force`。

---

## 9. 文档索引

| 文档 | 大小 | 内容 |
|---|---|---|
| `D:\LIBS\docs\realtime-spectrum-stream.md` | 7.8 KB | **实时光谱流设计**：纪律、命令面、帧结构、排障速查 |
| `D:\LIBS\docs\architecture-and-workflows.md` | 16 KB | 架构与业务流程 |
| `D:\LIBS\docs\business-layer-scope-report.md` | 40 KB | 业务层实现范围 |
| `D:\LIBS\docs\refactor-report.md` | 28 KB | 重构报告 |
| `D:\LIBS\docs\business-layer-and-frontend-plan.md` | 61 KB | 业务层与前端实施计划 |
| `D:\LIBS\docs\business-layer-scope-report.md.bak27` | 26 KB | 旧备份，可清理（未获授权，故保留） |

---

## 10. 结论

1. **架构是健康的**——四层单向依赖，逐文件扫描 `#include` 验证零违规；UI 零链接后端，强制走 JSON 管道。
2. **实时流功能已跑通**——端到端 10/10 帧无错误，三次重启均成功，57 个专项断言守着。
3. **测试基线扎实**——11 程序 995 断言全绿，零第三方依赖下靠自研框架 + 替身 DLL 达到这个覆盖率。
4. **两个真 bug 已修复并有回归测试**——都是状态机不对称类，根因分析已写入第 6 章，避免后人重蹈。
5. **最大的风险是光谱仪真机未验证**——191 个断言跑在替身上，字节可信、行为未知。上机第一步应集中攻这一点。
6. **能力边界诚实**——5 项未实现能力由 `status` 自报，不会静默假装成功。

**总体判断：软件侧已具备上机联调条件，但真机验证是必经的一步。**