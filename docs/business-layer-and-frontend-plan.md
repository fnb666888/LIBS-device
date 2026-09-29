# 业务层组织与远程控制 / UI 方案

> **文档定位**：这是一份"**下一步怎么建**"的方案设计，不是已完成工作的记录。
> 已完成的重构工作见 [`refactor-report.md`](./refactor-report.md)，工程总览见 [`../README.md`](../README.md)。
>
> 编写日期：2026-09-29 ｜ 适用工程：`D:\LIBS`
> 前提约束：库内**不含任何设备/安全/业务约束**；端口映射属于业务层，不进 `src/` 里的驱动层。

---

## 1. 结论速览

| 问题 | 结论 |
| --- | --- |
| 业务逻辑放哪一层？ | 新增**第 4 层**（业务层），位于 `libs_devices` 之上，通过已存在的 `libs` INTERFACE target 消费设备能力 |
| 业务层放哪个仓库/目录？ | 留在**同一个 `src/` 树内**，作为独立 CMake target（`libs_business`），这样单次 CMake 配置就能强制"业务层不能反向依赖驱动层" |
| 端口对应怎么处理？ | 做成业务层的**可加载站点配置文件**（`SiteConfig`），换工控机改配置、不重编译；**绝不硬编码进库** |
| 怎么通过 SSH 控制工控机？ | sshd **已部署**，链路直接可用。**C++ 守护进程独占设备**跑在工控机，**Python CLI** 跑在控制端调系统 `ssh.exe` 转发，经**命名管道**通信；**零新增第三方依赖** |
| 守护进程和 CLI 有什么区别？ | 守护进程 = 设备的"身体"（C++，独占 COM 口与 SDK 句柄，常驻工控机）；CLI = "遥控器"（Python，只讲 JSON、不碰设备、用完就退）。详见 §7.6 |
| 设备是否被其他程序占用？ | **没有**（已确认），不存在厂商上位机抢占串口的问题 |
| UI 必须用 Qt6 吗？ | **不是必须**，但**已确认需要、可延后**（阶段 7）。本机只有 MinGW Qt 6.11.2，推荐 **Qt6 Widgets**；必须**全链路 MinGW**，不能与 MSVC 混用 |
| 工控机怎么连？ | `~/.ssh/config` 已配好别名：`myserver3`（有线 192.168.101.100）、`myserver5`（无线 100.82.118.99），用户 `user`，密钥 `~/.ssh/ipc_key`。代码里**不存 IP 与私钥路径**，直接用别名 |
| SDK 在哪？ | 工控机 `C:\Avantes\AvaSpecX64-DLL_9.14.0.0`（**x64 only**，所以守护进程必须 x64）。路径写进 `SiteConfig`，**不写死在代码里** |
| 日后改用 Windows 服务麻烦吗？ | **不麻烦**。守护进程只对外暴露命名管道，启动方式对上层不可见。换壳成本 = 约 100 行服务壳 + 一个安装脚本；管道协议 / 业务层 / CLI / UI **零改动**。详见 §10.8 |
| 怎么编译部署？ | **控制端编译 + 拷贝部署**（已确认）。两台机同为 x64，无需交叉编译；工控机只装运行时（`businessd.exe` + MinGW 运行库 + `site.json`），零开发环境。详见 §10.5 |

## 2. 现状盘点（已逐项核实）

### 2.1 磁盘实际结构

```text
D:\LIBS\
├── device-references\        厂商原始资料（laser / rangefinder / serial-devices /
│                             spectrometer / air-conditioning）
├── docs\                     本文档所在目录
├── output\
├── README.md
└── src\
    ├── include\libs\        error.hpp  hex.hpp  protocols\  transport\  devices\
    ├── src\                  error.cpp  hex.cpp  protocols\  transport\  devices\
    ├── tests\                check.hpp/cpp  5 个测试程序 + fake_avaspec_sdk
    ├── CMakeLists.txt
    └── build.ps1
```

`src/build/`（`obj-nj`、`obj-mk`、`cmake-*` 等）是构建临时目录，已忽略。

### 2.2 CMake target 拓扑

```text
libs_protocols   STATIC   error/hex/modbus_rtu/laser/rangefinder/indicator/air_conditioner
libs_transport   STATIC   serial.cpp                 ── PUBLIC ──> libs_protocols
libs_devices     STATIC   laser/rangefinder/indicator/spectrometer/air_conditioner
                                                     ── PUBLIC ──> libs_transport
libs             INTERFACE                          ── INTERFACE > libs_devices
libs_warnings    INTERFACE   /W4 /permissive-  或  -Wall -Wextra -Wpedantic
```

- `libs_devices` 里没有任何反向引用，是干净的设备驱动层。
- **`libs` 这个 INTERFACE target 就是留给上层的挂载点**，业务层链接它即可，不必逐个列举子库。
- 测试共 5 个程序，`build.ps1` 全绿：**455 条断言、5 个程序、0 警告**；CMake 下 ctest 5/5 通过（Ninja 与 MinGW Makefiles 两种生成器均验证过）。

### 2.3 设备驱动对外 API 摘要（写业务层时的调用面）

```cpp
// laser
Result<LaserTransaction> write(std::uint8_t command, const Bytes& payload = {});
Result<LaserTransaction> send (std::uint8_t command, std::uint8_t value);

// rangefinder
Result<RangefinderRead> read(std::uint8_t address, std::uint16_t start, std::uint16_t count);

// indicator（状态灯）
enum class Channel { red, yellow, green, blue, white, buzzer };
enum class State   { off, on, blink_1hz, blink_2hz };
Result<LampQuery> query(std::uint8_t address, Channel channel);
Result<LampWrite> write (std::uint8_t address, const libs::indicator::WriteRequest&);

// air_conditioner
Result<StatusRead>   read_status (std::uint8_t address, uint16_t start = reg::kCoolingOn, uint16_t count = 24);
Result<ParamRead>    read_params (std::uint8_t address, uint16_t start = reg::kIndoorTemp, uint16_t count = 25);
Result<ControlWrite> set_power   (std::uint8_t address, bool on);
Result<ParamWrite>   write_param (std::uint8_t address, const libs::air_conditioner::ParamWrite&);
Result<ParamWrite>   set_setpoint(std::uint8_t address, uint16_t reg, double value);
Result<ParamWrite>   set_mode    (std::uint8_t address, libs::air_conditioner::Mode mode);

// spectrometer
static Result<std::unique_ptr<Spectrometer>> open(const std::string& sdk_dll);
Result<std::vector<SpectrometerInfo>> enumerate();
Result<CaptureResult> capture(const CaptureRequest& request);   // 字段全显式，无隐式默认值
VoidResult close();
```

传输层：

```cpp
enum class Parity { none, even, odd };
struct Reply { bytes, elapsed_ms, receive_errors, last_receive_tick, timed_out };
class Link { virtual Result<Reply> exchange(bytes, timeout_ms = 650, idle_ms = 80) = 0;
            virtual std::string endpoint() const = 0; };
Result<std::unique_ptr<Link>> open_serial(port, baud, Parity = none, data_bits = 8);
std::vector<std::string> list_serial_ports();
```

错误模型：`ErrorKind { none, invalid_argument, unsupported, protocol, transport, device, timeout, internal }`，
全部走 `Result<T>` / `VoidResult`，业务层只需 `switch` 一次错误类型，不用解析字符串。
---

## 3. 并发与独占性约束（本方案的硬依据）

这一节是后面所有架构决策的**根因**，请勿跳过。

1. **串口是独占资源。** `src/src/transport/serial.cpp:40` 打开句柄时
   `CreateFileA(path, GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)`
   的共享模式是 **0**。同一 COM 口第二次 `open_serial()` 一定失败。
   → 进程内、跨进程都不能有两个持有者去开同一个口。
2. **AvaSpec SDK 全机单实例。** `src/src/devices/spectrometer.cpp:159-176` 使用命名互斥体
   `Local\LibsAvaSpecSdkOwner`，`WaitForSingleObject(handle, 0)` 非阻塞；被占用时直接抛
   `another local AvaSpec operation is already running`。
3. **整个库是同步阻塞的。** `exchange()` 默认阻塞 650 ms；采集 `timeout_ms` 默认 3000 ms；
   激光脉冲的 delay/width 是 FPGA 周期参数。库**没有**异步 / 事件模型。

**由此推出三条铁律：**

- 任何"多用户同时操作设备"的诉求，不能靠"多开几个进程"解决，只能靠**单一持有者 + 外部排队**。
- 设备持有者必须是**长驻进程**，不能是"执行完就退"的脚本。
- 远程操作是**跨网络**行为，串口本身**绝对不能**暴露给 SSH 会话。

---

## 4. 业务层的定位

### 4.1 它是新增的第 4 层

```text
        ┌───────────────────────────────────────────┐
   UI   │  ui/          Qt6 Widgets（很薄）         │
  SSH   │  remote/      薄 CLI 客户端（很薄）        │
        └───────────────────┬───────────────────────┘
                            │ 只能调用
        ┌───────────────────▼───────────────────────┐
        │  business/  services/  AcquisitionService │  ← 第 4 层（本次要建）
        │  config  policy  plan  session  app        │
        └───────────────────┬───────────────────────┘
                            │ 只依赖
        ┌───────────────────▼───────────────────────┐
        │  devices/    驱动层（Laser / AirConditioner…）│
        │  transport/  串口 / SDK                      │
        │  protocols/  纯协议编解码（无设备知识）        │
        └───────────────────────────────────────────┘
```

**库内三条纪律继续有效：**
- `protocols/*` 只 include `error / hex / modbus_rtu`，不认识任何设备。
- `devices/*` 只 include `transport/serial` + 自己协议的 header。
- 业务层是**唯一**允许知道"哪个设备在哪个口、什么时候允许出光"的地方。

### 4.2 为什么留在同一个 `src/` 树里，而不是另开一个仓库 / 顶层工程

- 拆成两个独立工程后，两者之间的**依赖方向就无法在构建期强制** —— 只能靠 code review 拦，容易悄悄写出 `business → devices → business` 的环。
- 留在同一棵树、作为独立 target，CMake 单次 configure 就能把"业务层只能通过 `libs` 进入"变成**硬约束**：
  `libs_business` 只 `target_link_libraries(... PUBLIC libs)`，底层三个库不 link 它，环在配置期就会报错。
- 单次 `cmake --build` 即可全量构建与联调，不需要手工维护两套编译参数。

### 4.3 依赖方向规则（建议写进 CI 检查）

| target | 允许依赖 | 禁止依赖 |
| --- | --- | --- |
| `libs_protocols` | 仅 C++ 标准库 | transport / devices / business |
| `libs_transport` | `libs_protocols` | devices / business |
| `libs_devices` | `libs_transport` | business |
| `libs_business`（新） | `libs` | `remote` / `ui` |
| `libs_businessd`（新，可执行） | `libs_business` | — |
| `lpc-cli`（新，**Python**） | 仅标准库 `subprocess` | 链接任何 C++ 库 / `libs_*` |
| `ui`（新，可执行） | `libs_business`（或仅 IPC 客户端） | `libs_devices` |
---

## 5. 业务层目录组织

```text
src/
├── business\                     ★ C++
│   ├── config\      site_config.hpp / .cpp      端口与站点参数（可加载，非硬编码）
│   ├── policy\      gate.hpp                    出光门禁、联锁、温控等规则
│   ├── plan\        trigger_plan.hpp            一次测量该做什么（时序方案）
│   ├── session\     device_session.hpp / .cpp   打开/关闭/健康检查/重连，独占持有全部设备对象
│   ├── services\    acquisition_service.hpp     ★ 对外唯一门面
│   └── app\         application.hpp             应用编排（替代旧 main.cpp）
├── businessd\                    ★ C++ 可执行（阶段 4）
│   ├── named_pipe_server.cpp                     命名管道服务端
│   └── main.cpp                                  守护进程入口
├── remote\                       ★ Python（阶段 5，不参与 C++ 构建）
│   ├── lpc_cli\
│   │   ├── __main__.py                           CLI 入口
│   │   ├── ssh_transport.py                      调系统 ssh.exe（见 §7.5）
│   │   ├── ipc_client.py                         命名管道 JSON 客户端
│   │   └── commands/                             各子命令实现
│   └── README.md                                 部署与用法说明
└── ui\                           ★ C++/Qt（阶段 7，可选）
    └── main.cpp
```

> `remote/` 里的 Python **不参与 CMake 构建**，也不链接任何 `libs_*`。
> 它的存在形式就是几个 `.py` 文件，拷到工控机（或控制端）即可运行。
> 若将来改用 C++ 重写 CLI，只需替换整个 `remote/`，`businessd` 与管道协议不变。

各目录职责一句话：

| 目录 | 职责 | 不该出现在这里的东西 |
| --- | --- | --- |
| `config/` | 端口表、站号、超时、设备使能开关 | 任何协议字节 |
| `policy/` | "现在允许出光吗"这类**规则与联锁** | 串口读写 |
| `plan/` | 把一次测量拆成有序的设备动作序列 | 直接操作设备 |
| `session/` | 独占持有 `Laser` / `Rangefinder` / `Indicator` / `AirConditioner` / `Spectrometer`，负责开关与重连 | 业务判断 |
| `services/` | 对外唯一门面，UI 与 SSH 都只调它 | 直接 `CreateFile` |
| `app/` | 进程生命周期、依赖装配、退出清理 | 协议细节 |
| `businessd/` | 把 `services` 暴露为命名管道 JSON 服务，注册为开机自启 | 任何业务判断（应下沉到 `business/`） |
| `remote/` | 参数解析 + SSH 转发 + 管道客户端 | 任何设备知识、任何协议字节 |

### 5.1 关键约定

- **`services/` 是唯一门面。** UI 和 SSH 都只能调 `AcquisitionService`，**任何一处都不允许直接调 `Laser::write()` 或 `AirConditioner::set_power()`**。做到这一点之后，"加远程控制"就完全不需要改业务逻辑。
- `session/` 是**唯一**持有设备对象的地方。设备对象生命周期收敛到这一层，进程退出时统一 `close()`。
- 业务层可以有自己的错误枚举，但**不要**把库的 `ErrorKind` 直接透给 UI；应做一次映射（例如 `transport` → "串口被占用" 的可读文案），否则前端要跟着库的枚举演进。

### 5.2 代码草案（仅示意，尚未实现）

**站点配置**（端口表在这里，不在库里）：

```cpp
// business/config/site_config.hpp
namespace libs::business::config {

struct SerialEndpoint {
    std::string port;                 // "COM12"
    std::uint32_t baud{};
    transport::Parity parity{transport::Parity::none};
    std::uint8_t data_bits{8};
    std::uint8_t station{1};
};

struct SiteConfig {
    SerialEndpoint laser          = {"COM10", 38400, transport::Parity::none, 8, 1};
    SerialEndpoint rangefinder    = {"COM7",  9600,  transport::Parity::none, 8, 1};
    SerialEndpoint indicator      = {"COM8",  9600,  transport::Parity::even, 8, 1};
    SerialEndpoint air_conditioner = {"COM12", 9600,  transport::Parity::none, 8, 1};

    std::vector<std::string> spectrometer_dlls;    // AvaSpec SDK 路径
    std::vector<std::string> spectrometer_serials;  // 4 台：1706097U4 ~ 1706100U4

    // 从 JSON/TOML 加载；失败返回 ErrorKind::invalid_argument，不静默用默认值
    static Result<SiteConfig> load(const std::filesystem::path& file);
};

} // namespace libs::business::config
```

**门禁 / 联锁**（业务规则的落点）：

```cpp
// business/policy/gate.hpp
namespace libs::business::policy {

enum class GateState { closed, open };

class Gate {
public:
    VoidResult arm();     // 校验所有条件通过后才允许下一次出光
    VoidResult disarm();
    GateState state() const noexcept;
private:
    bool chamber_ready_{};
    bool interlock_ok_{};
    bool temperature_ok_{};
};

} // namespace libs::business::policy
```

> 注意：`set_setpoint()` 里的范围检查属于**协议参数合法性**，留在库里是对的；
> "室温 28 ℃ 时能不能把设定温度设成 18 ℃"是**业务规则**，必须放在 `policy/`。

**唯一门面**：

```cpp
// business/services/acquisition_service.hpp
namespace libs::business::services {

class AcquisitionService {
public:
    static Result<std::unique_ptr<AcquisitionService>> create(const config::SiteConfig&);

    Result<plan::TriggerPlan> plan(const AcquisitionRequest&);

    VoidResult submit(const plan::TriggerPlan&);        // 内部：gate.arm() -> 触发 -> gate.disarm()
    Result<AcquisitionReport> wait_for(const std::string& plan_id);

    Result<TelemetrySnapshot> telemetry();
    Result<std::vector<libs::devices::spectrometer::SpectrometerInfo>> devices();
    VoidResult emergency_stop();
};

} // namespace libs::business::services
```

**CMake 骨架**：

```cmake
add_library(libs_business STATIC
    business/config/site_config.cpp
    business/policy/gate.cpp
    business/session/device_session.cpp
    business/services/acquisition_service.cpp
)
target_link_libraries(libs_business PUBLIC libs)
target_link_libraries(libs_business PRIVATE libs_warnings)
```
---

## 6. 端口与设备配置

### 6.1 现场端口对应表

| 设备 | 接口 | 参数 | 站号 | 备注 |
| --- | --- | --- | --- | --- |
| 激光器 | **COM10** | 38400 / 8 / N / 1 | — | 调出光与激光参数 |
| 测距仪 | **COM7** | 9600 / 8 / N / 1 | 1 | 高度 / 距离读数 |
| 状态灯 | **COM8** | 9600 / 8 / **E** / 1 | 1 | 6 通道 + 蜂鸣器 |
| 空调 | **COM12** | 9600 / 8 / N / 1 | 1 | FC01 / FC03 / FC05 / FC06，见厂商样例 §7.11 / §7.12 |
| 光谱仪 x4 | USB（虚拟串） | — | — | 序列号 `1706097U4` ~ `1706100U4`，2048 px；SDK 在 `C:\Avantes\AvaSpecX64-DLL_9.14.0.0` |
| — | COM1 ~ COM6 | — | — | 系统占用，勿用 |
| — | COM11 | — | — | 实测无响应，勿用 |

### 6.2 这些端口**为什么不进 `src/`**

- 库只接受"运行时传入哪个口"，已经做到零硬编码（这也是本次重构的目标之一）。
- 端口映射是**站点属性**：换一台工控机、换一个 USB 插口，值就变。写进代码 = 每换机器要重新编译。
- 因此它属于业务层 `config/`，并且要做成**可加载配置文件**，让运维改 JSON/TOML 即可。

### 6.3 配置加载的行为约定

- 文件缺失 / 字段缺失 / 字段非法 → 返回 `ErrorKind::invalid_argument`，**不要静默回落到默认值**（会开错设备）。
- 提供 `lpc-cli config validate <file>` 之类的命令做**离线自检**：不打开任何串口，只校验配置文件本身。
- 启动时再做一次"端口是否被占用"的探测；被占用直接拒绝启动并打印端口名（`serial.cpp` 的 share=0 保证了探测结果可信）。

---

## 7. 远程控制方案（SSH 服务已就绪）

> 本节已按"**工控机侧 OpenSSH Server 已部署完成**"的前提改写：不再需要安装 sshd，
> 只需要确定"**从哪台机器、用什么方式、发什么命令**"。

### 7.1 前置确认：客户端能力与已有配置（已实测）

| 项目 | 结果 |
| --- | --- |
| `ssh.exe` | `C:\Windows\System32\OpenSSH\ssh.exe`（**OpenSSH_for_Windows 9.5p2, LibreSSL 3.8.2**） |
| `ssh-keygen.exe` | `C:\Windows\System32\OpenSSH\ssh-keygen.exe` |
| 私钥 | **`~/.ssh/ipc_key` 已存在**（配套 `.pub`），无需重新生成 |
| `~/.ssh/config` | **已配好工控机别名** `myserver3`（有线）/ `myserver5`（无线），见 §10.4 |
| `plink.exe`（PuTTY） | `C:\Program Files\PuTTY\plink.exe`（备用，功能重叠） |
| 控制端 Python | **已装 Python 3.14.6**（`D:\python\python.exe`），标准库 `json` / `subprocess` 可用 |
| Qt6 可用网络模块 | `Qt6Network`、`Qt6Concurrent`（**无 SSH**） |
| `D:\LIBS\src\third_party` | **不存在**（没有 libssh2 / wolfSSH 等第三方 SSH 库） |
| 工控机 Python | **待装**（到现场装官方 installer，加 PATH） |

结论：**控制端能力已完备 —— SSH 客户端、密钥、配置别名、Python 全部就位，
不需要引入任何新的第三方依赖。** 唯一待办是在工控机上装 Python。

### 7.2 通信方式选型：直接调用 `ssh.exe`

Qt6 Network 只有 TCP/TLS socket，**不含 SSH 客户端**
（`QSSHClient` 是 Qt5 的商业组件，已在 Qt6 中移除）。
但既然系统已经自带 `ssh.exe`，**最省事、最稳的做法是把它当子进程调用**，而不是引入 SSH 库。

| 方案 | 依赖 | 评价 |
| --- | --- | --- |
| **A. 子进程调用系统 `ssh.exe`** | 无（系统自带） | **推荐**：零新增依赖，OpenSSH 的加密/认证/主机密钥校验全部由官方实现负责 |
| B. `libssh2` / `wolfSSH` | 需装到 `src/third_party/` 并自己编译 | 想要"库级异步 / 无子进程"时才值得；本项目没有必要 |
| C. Qt Network 自研 SSH | 不可行 | 没有 SSH 实现，等于自己写密码学 |
| D. PuTTY `plink.exe` | 无（本机已装） | 可作为 C 方案退出时的备份；功能上与 `ssh.exe` 重叠 |

选 A 的理由：
- **不增加构建复杂度** —— 不需要动 `CMakeLists.txt`，不需要编译第三方库。
- **安全属性不打折** —— 主机密钥校验、密钥交换、算法套件都由 OpenSSH 负责。
- **运维熟悉** —— 出问题可以直接在命令行里手敲 `ssh` 复现。
- **可替换** —— 后面若真要换成 B，只需替换 §7.6 里的 `SshTransport` 实现，上层无感。

### 7.3 现场先跑通（部署第一步）

`~/.ssh/config` 里**已经配好两个别名**，直接用别名即可：

```text
Host myserver3      HostName 192.168.101.100   User user   IdentityFile ~/.ssh\ipc_key   # 有线
Host myserver5      HostName 100.82.118.99    User user   IdentityFile ~/.ssh\ipc_key   # 无线
```

现场第一步只需验证链路（不需要新生成密钥，`ipc_key` 已存在）：

```powershell
# 1) 端口可达性
Test-NetConnection 192.168.101.100 -Port 22

# 2) 免密登录 + 拿到主机名
ssh myserver3 "hostname"

# 3) 顺带确认两个待确认项（PowerShell 版本 / Python 是否已装）
ssh myserver3 "powershell -NoProfile -Command \"$PSVersionTable.PSVersion\""
ssh myserver3 "python --version"

# 4) 确认 SDK 目录
ssh myserver3 "dir C:\Avantes\AvaSpecX64-DLL_9.14.0.0"

# 5) 确认设备未被占用（应有 4 个 COM10/7/8/12 空闲）
ssh myserver3 "powershell -NoProfile -Command \"Get-CimInstance Win32_SerialPort | Select-Object DeviceID\""
```

**配置已就绪，所以代码里不需要存 IP / 用户名 / 私钥路径。**
`~/.ssh/config` 已经管了 `HostName` / `User` / `Port` / `IdentityFile` / `IdentitiesOnly`，
`lpc-cli` 直接 `ssh myserver3 "命令"` 即可。详见 §10.4。

> 若现场换了工控机，只需要在 `~/.ssh/config` 加一个新别名，命令行 `--host newalias` 即可。

### 7.4 进程模型：SSH 通了，但独占性问题依然存在

**必须澄清一点：SSH 已部署解决的是"网络可达"，不解决"设备独占"。**
§3 的三条约束一条都没变，因此进程模型仍然是 **守护进程 + 命名管道 + 薄客户端**：

```text
   控制端笔记本
        │  ①  ssh.exe  (22)                  ← 已部署，链路已通
        ▼
   ┌──────────────────────────────────────┐
   │  lpc-cli  （Python，薄）               │  参数解析
        │  ②  ssh host "python lpc_cli.py" │  经 ssh.exe 转发
        ▼
   ┌──────────────────────────────────────┐
   │  工控机                                │
   │   ├─ sshd -> shell -> python 薄壳     │
   │   │        ③ 命名管道 JSON（本地）     │
   │   │             ▼                     │
   │   │  libs_businessd （C++，单例）     │
   │   │   ├─ 独占 COM10/7/8/12 + 4×SDK    │
        │   └─ 独占门禁状态、采集队列         │
   └──────────────────────────────────────┘
```

（实际部署时 `lpc_cli.py` 就在工控机上，SSH 只是把控制端的参数送过去执行；
也可以让 `lpc-cli` 两端各放一份。两种都行，关键是 **它从不碰设备**。）

如果改成"每条 SSH 命令直接开串口"，会同时踩三个坑：

| 坑 | 依据 |
| --- | --- |
| 第二次打开必然失败 | `serial.cpp` share=0 |
| AvaSpec 被占用直接抛异常 | 命名互斥体 `Local\LibsAvaSpecSdkOwner` |
| 阻塞调用卡死会话 | `exchange()` 650 ms、采集 3000 ms 同步阻塞 |

另外还有一个 SSH 特有的坑：**SSH 会话断开时，远端子进程会被杀掉**。
串口句柄随之释放、正在进行的测量中断、门禁状态丢失。
所以"SSH 里直接跑长任务"不可可靠，任务必须在守护进程里排队、会话只负责"提交 + 查询"。

### 7.5 CLI 侧调用封装（Python，标准库即可）

CLI 用 Python，**不需要 `paramiko`**，直接 `subprocess` 调系统 `ssh.exe`。
因为 `~/.ssh/config` 已配好别名，`SshOptions` **只需一个 alias**：

```python
# remote/lpc_cli/ssh_transport.py
import subprocess
from dataclasses import dataclass

@dataclass
class SshOptions:
    alias: str = "myserver3"      # 有线；无线回退用 myserver5
    connect_timeout_s: int = 5

class SshTransport:
    def __init__(self, opts: SshOptions):
        self._opts = opts

    def run(self, remote_command: str, timeout_s: int = 60) -> tuple[int, str, str]:
        """返回 (exit_code, stdout, stderr)；不做任何解析。"""
        cmd = [
            "ssh",
            "-o", "BatchMode=yes",                 # 关键：绝不等待输入
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", f"ConnectTimeout={self._opts.connect_timeout_s}",
            "-o", "ServerAliveInterval=15",
            self._opts.alias,                      # 别名即可，其余 ssh 自己解析
            remote_command,
        ]
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout_s, encoding="utf-8", errors="replace")
        return p.returncode, p.stdout, p.stderr
```

**这就是 `~/.ssh/config` 带来的最大简化**：不需要 `host` / `user` / `key_file` / `port` 四个字段，
一个 alias 全解决。详见 §10.4。

实现要点：

- **stdout / stderr 分开捕获**。混在一起就无法判断是"命令失败"还是"连不上"。
- **必须有超时**。`subprocess.run(timeout=...)` 超时会抛 `TimeoutExpired`；
  如需强杀整棵进程树（`ssh.exe` 会带出远端进程），Windows 上需 `taskkill /T /F /PID`。
- **不要用交互式 shell**。一律 `ssh <alias> "powershell -NoProfile -Command ..."`，
  远端必须加 `-NoProfile`，否则会加载工控机的 PowerShell 配置、拖慢每次调用。
- 远端路径用**绝对路径**并加引号 —— SSH 默认 shell 不保证工作目录。
- 远端 PowerShell 5.1 的 `ConvertTo-Json` 深度有限，复杂对象要显式给 `-Depth`；
  且 5.1 读 stdin 用 `-Command -`，5.1 与 7 行为不同，**建议把请求写成临时文件再读**，避开差异。
- 私钥路径**不写进代码**（由 `~/.ssh/config` 管），私钥内容绝不打印。

> 若将来 CLI 改用 C++（例如要打包成单文件 exe 分发），把上面这个类换成一个 `QProcess`
> 或 `std::system` + 重定向的等价实现即可，管道 JSON 协议完全不变，**业务层与守护进程零改动**。

### 7.6 守护进程与 CLI 的分工（语言选型已定）

一句话：**守护进程是设备的唯一"身体"，CLI 是遥控器。**

| | `libs_businessd`（守护进程） | `lpc-cli`（CLI 客户端） |
| --- | --- | --- |
| **跑在哪** | 工控机上，开机自启，常驻 | 控制端笔记本上，用完就退 |
| **是否碰设备** | 独占 COM7 / COM8 / COM10 / COM12 + 4 台光谱仪 | **完全不碰**，一个串口都不开 |
| **持有状态** | 全部设备对象、门禁状态、采集队列、AvaSpec SDK 句柄 | 无状态，每次调用彼此独立 |
| **生命周期** | 天 / 周 | 秒 |
| **崩溃后果** | 设备掉线，需重启守护进程 | 无影响，下次调用重连即可 |
| **SSH 断开** | **不受影响**，测量继续跑完 | 自己没了，远端任务照常执行 |
| **谁写协议字节** | 唯一持有 `libs` 的地方 | 永远不接触 |
| **语言** | **C++**（需长期稳定持有串口 / SDK 句柄） | **Python**（只做 JSON 管道 + 调 `ssh.exe`） |

**为什么必须拆成两个进程** —— 三个硬理由：

1. **串口只能被一个进程持有**（`serial.cpp` share=0）。若每条 SSH 命令各自开串口，第二个命令必然失败。
2. **SSH 会话断开会杀掉远端子进程**。若测量直接在 SSH 会话里跑，笔记本合盖、断网、切 WiFi 都会让测量作废。
   放进守护进程后，**任务与你的连接解绑**。
3. **状态必须跨会话存活**。"当前是否正在出光""上次的 TriggerPlan""空调现在多少度"不能随每次 CLI 调用一起消失。

**语言分工的依据：**

- **守护进程用 C++**：它要直接链接 `libs_business` / `libs_devices`，需要 ABI 稳定、异常行为可预期、要长年不崩。
- **CLI 用 Python**：它**一次都不需要链接 C++ 库**，只讲 JSON。迭代快、部署简单（拷一个 `.py` 就行），
  出问题改一行不用重编译 —— 这正是分层的红利。

**一次调用的完整链路：**

```text
你敲:  lpc-cli run plan.json
  |  （本地 Python，只解析参数）
  v
ssh.exe  --------------------->  工控机
  |
  v
lpc-cli (工控机上的 Python 脚本，薄)
  |  命名管道 JSON: {"op":"submit","plan":{...}}
  v
libs_businessd (C++)
  |  独占串口 / SDK，真正执行
  |  {"ok":true,"plan_id":"p-001"}
  v
回到本地 CLI 轮询 {"op":"wait"} -> {"op":"query"} -> {"op":"result"}
  |
  v
终端打印结果
```

> 注意这条链路上 **Python 从头到尾没有链接任何 C++ 库**。如果哪天业务逻辑变了、需要改客户端，
> 只需要重写那一个 `.py`；工控机上的 C++ 守护进程一行不动、也不用重新部署。

**部署形态小结：**

| 组件 | 语言 | 位置 | 部署方式 |
| --- | --- | --- | --- |
| `libs_businessd` | C++ | 工控机 | 单个 exe，注册为 Windows 服务 / 计划任务开机自启 |
| `lpc-cli` | Python | 控制端（也可放工控机） | 拷一个 `.py`（或打包 exe），走系统 `ssh.exe` |
| 管道协议 | JSON | 两端约定 | 命名管道 `\\.\pipe\libs-biz-1` |

### 7.7 安全边界

- SSH 仅限**内网或 VPN** 入口，不要映射到公网。
- 建议工控机侧 `PasswordAuthentication no`，只用密钥；`BatchMode=yes` 也强制了这一点。
- 私钥只存在于控制端，**不进代码库**、不进日志、不写进配置文件模板。
- 绝对不要把串口做成 TCP 服务暴露出去（等于把激光器控制权开放到网络）。
- 审计：守护进程把每次命令与结果追加写到本地日志，SSH 侧也能查到 `who`。

## 8. UI 方案

### 8.1 本机 Qt 环境实测结论

| 检查项 | 结果 |
| --- | --- |
| Qt 安装位置 | `D:\Qt6\6.11.2\mingw_64\`（**只有 mingw_64 一个 kit，没有 MSVC kit**） |
| `Qt6Config.cmake` | `D:\Qt6\6.11.2\mingw_64\lib\cmake\Qt6\Qt6Config.cmake` 已存在 |
| `Qt6WidgetsConfig.cmake` | `D:\Qt6\6.11.2\mingw_64\lib\cmake\Qt6Widgets\Qt6WidgetsConfig.cmake` 已存在 |
| `Qt6NetworkConfig.cmake` | `D:\Qt6\6.11.2\mingw_64\lib\cmake\Qt6Network\Qt6NetworkConfig.cmake` 已存在 |
| 编译工具链 | `D:\Qt6\Tools\mingw1310_64\bin\g++.exe` |

**关键约束：本机 Qt 只有 MinGW 版，而驱动库也是 MinGW 构建的，全链路必须统一用 MinGW。**
一旦混入 MSVC 编译的 Qt 或库，会遇到 `CMAKE_CXX_COMPILER_ID` 不匹配以及运行库
（`libstdc++` vs `MSVCP`）混链的链接错误。
CMake 里必须用 `CMAKE_PREFIX_PATH=D:\Qt6\6.11.2\mingw_64`，并把
`CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` 指向 MinGW。

### 8.2 方案对比

| 方案 | 优点 | 缺点 | 适合度 |
| --- | --- | --- | --- |
| **Qt6 Widgets（本机触摸屏）** | 与 C++ 库同工具链；离线可用；工控机常见形态 | 需引入 Qt 依赖 | 推荐 |
| Qt6 QML | 动画 / 现代化界面好 | 引入 QML 引擎，体积大；本机若无 GPU 反而吃亏 | 一般 |
| 本地 HTTP + WebSocket 服务 | 前端可用现有 Web 技术栈 | 需自建 Web 栈；只做本地绑定时勉强可接受 | 可行 |
| WebSocket + 已有的 Web 前端 | 前端复用度最高 | 需要额外 Web 服务器 / 运行时 | 可行 |
| 纯 CLI（无 UI） | 最省事 | 操作员不友好 | 调试期够用 |

### 8.3 选型建议

**推荐：Qt6 Widgets 本机界面 + 复用 `businessd` 的同一套命名管道服务（与 Python CLI 完全对称）。**

- 界面目标如果只是**本机触摸屏**，Widgets 是最省心、最稳的选择：与库同 MinGW 工具链，离线可用。
- 界面**只做两件事**：连接 / 断开、发出指令、显示返回。里面**零设备协议代码**。
- UI 只依赖管道客户端头（走管道与 UI / CLI 完全解耦），**永不依赖 `libs_devices`**。
- 这样以后要换前端（QML / Web / 远程网页），业务层一行不改。

**已确认：UI 需要，但可以延后。** 因此：

| 决策 | 说明 |
| --- | --- |
| 排序 | UI 排在**阶段 7**，在守护进程 + CLI（阶段 4 - 5）全部跑通之后 |
| 不阻塞 | 阶段 1 - 6 完全不依赖 UI，UI 只是**同一个管道协议的另一个客户端** |
| 待定 | UI 跑在**工控机本机**（触摸屏）还是**控制端**？—— 两种都能实现，见下表 |
| 前提 | 无论哪种，UI 与 Python CLI **平级**，各写一个管道客户端即可，业务层不动 |

| 部署位置 | 实现 | 适合 |
| --- | --- | --- |
| 工控机本机 | Qt6 Widgets 程序，读本地命名管道 | 现场触摸屏操作，最常见 |
| 控制端 | Qt6 Widgets 程序，通过 `ssh.exe` + 管道转发 | 在办公室远程操作，与 CLI 并列 |
| 两者都要 | 同一份 Qt 代码，只换"直连管道"还是"走 SSH"两种 transport | 复用度最高 |

> 因为 UI 只是"管道的另一个客户端"，**它天然可延后**：管道协议一旦定下（阶段 4），
> 加 UI 只是加一个客户端，不触碰业务层与守护进程。

## 9. 实施顺序建议（分阶段，每阶段可独立验证）

| 阶段 | 内容 | 语言 | 验收标准 |
| --- | --- | --- | --- |
| 1 | `business/config` + `SiteConfig` 加载 + 离线校验命令 | C++ | 能加载配置并校验端口 / 参数 / 站号 / SDK 路径，不开串口 |
| 2 | `business/session`：独占打开全部设备、健康检查、优雅关闭 | C++ | 设备开关、异常退出后可恢复；同一口被二次打开时明确报错 |
| 3 | `business/services`：门面方法 `devices` / `status` / `plan` / `run` / `stop` | C++ | 上层可完成一次完整测量；库零改动 |
| 4 | `businessd` 守护进程 + 命名管道 JSON 协议 | C++ | 多个客户端能**同时**在线，且设备只被打开一次 |
| 5 | `remote/lpc-cli` 薄客户端（`subprocess` 调 `ssh.exe`） | **Python** | `ssh myserver3 "python lpc_cli.py run plan.json"` 全流程可跑 |
| 6 | `deploy.ps1` + 计划任务（开机自启 + 崩溃重启）+ 工控机装 Python | PowerShell | `pwsh -File deploy.ps1` 一条命令完成"编译→拷贝→重启→验活" |
| 7 | `ui/` Qt6 Widgets 界面（**已确认需要，可延后**） | C++/Qt | 现场触摸屏可操作；能替换为 CLI 而不影响业务 |

> 每阶段结束时都必须保持 `build.ps1` **455 条断言全绿**。新增层不得改动
> `libs_protocols` / `libs_transport` / `libs_devices` 的任何源码。
>
> **阶段 1 - 4 全部在控制端离线开发**（编译 + 455 条测试），不需要设备在场、不需要 SSH；
> 阶段 5 第一次引入 SSH；阶段 6 打通"编译 → 拷贝部署 → 开机自启"；
> 阶段 7（UI）可延后，因为它只是管道的另一个客户端。
>
> **部署模式（已确认，见 §10.5）**：控制端编译，拷贝到工控机运行。
> 工控机零开发环境；`output/` 是部署包目录；`site.json` 刻意不随部署覆盖。

## 10. 决策记录与待确认事项

### 10.1 已确认的决策

| # | 问题 | 结论 |
| --- | --- | --- |
| 1 | 工控机上是否已有程序占用设备？ | **没有程序占用**。串口不被外部程序抢占，守护进程启动不会因 share=0 失败 |
| 2 | 守护进程 / CLI 用什么语言？ | **守护进程 C++，CLI Python**。分工见 §7.6 |
| 3 | SSH 服务端 | 工控机 **sshd 已部署**；控制端调用系统 `ssh.exe`，并已在 `~/.ssh/config` 里配好别名 |
| 4 | 端口映射放哪 | 业务层可加载配置文件，**绝不硬编码进 `src/`** |
| 5 | 业务层位置 | 同一 `src/` 树内的独立 target `libs_business`，位于 `libs` 之上 |
| 6 | 光谱仪 SDK 路径 | **`C:\Avantes\AvaSpecX64-DLL_9.14.0.0`**（工控机实测路径） |
| 7 | UI 是否需要 | **需要，但可以延后** → 排在阶段 7，不阻塞主线 |
| 8 | 是否需要多机组网 | **暂时不需要** → `SiteConfig` 按**单站点**建模即可 |
| 9 | 工控机 SSH 别名 | `myserver3`（有线 `192.168.101.100`）、`myserver5`（无线 `100.82.118.99`），用户 `user`，密钥 `~/.ssh/ipc_key` |
| 10 | 守护进程注册方式 | **计划任务**（可），与 Windows 服务的区别见 §10.3 |
| 11 | Python | 控制端**已装** Python 3.14.6（`D:\python\python.exe`）；工控机**待装** |
| 12 | 守护进程日后要不要改成 Windows 服务？ | **待定，但不阻塞**。当前用计划任务；架构上已按"可服务化"设计（§10.8），切换只需加服务壳 + 换安装脚本，**协议与业务层零改动** |
| 13 | 编译与部署路径 | **控制端编译 + 拷贝部署**（确认）。工控机零开发环境，详见 §10.5 |

**第 1 项的连带影响：**

- 不需要"设备占用检测 / 抢占 / 等待厂商上位机退出"这类逻辑。
- 阶段 2 的健康检查**只做"我能否打开这个口"**，不做"是谁占用的"排查。
- 唯一冲突场景是**本项目自身的两个进程**（调试时忘了关守护进程又手动开串口），
  由 `serial.cpp` 的明确报错兜底，守护进程启动时打印一行"某口已在使用"即可。

**第 6 项的连带影响（SDK 路径）：**

- `SiteConfig` 里的 `spectrometer_dlls` 默认候选值填这个目录，配置文件里允许覆盖。
- 部署检查项：`Test-Path C:\Avantes\AvaSpecX64-DLL_9.14.0.0` —— 路径不存在时守护进程应**明确报错**，
  而不是"枚举到 0 台设备"这种含糊提示。
- SDK 目录是 **x64** 的，守护进程**必须编译成 x64**。控制端同为 x64，`x86_64-w64-mingw32` 目标直接可用，**不需要交叉编译**（见 §10.5）。
  （MinGW 交叉编译时注意 `-m64` 与运行库一致性。）

**第 8 项的连带影响（不做多机组网）：**

- `SiteConfig` 按单站点写，不必引入"站点表 / 多站点选择 / 站点切换"等概念。
- 但仍建议**保留一个 `site_name` 字段**，便于日志区分是哪台机器（将来若要扩展也不用改结构）。
- `lpc-cli` 也不需要"选站点"参数，SSH 别名由 `~/.ssh/config` 承担即可。

**第 9 项的连带影响（SSH 别名已配好）：**

- `lpc-cli` **不必**自己解析 IP / 用户名 / 私钥路径，直接用别名：
  `ssh myserver3 "powershell -NoProfile -Command ..."` 即可，`~/.ssh/config` 已经管了
  `HostName` / `User` / `Port` / `IdentityFile` / `IdentitiesOnly`。
- 这比在代码里存 IP 和私钥路径**更安全**（私钥路径不写在代码/配置模板里）。
- 因此 §7.5 的 `SshOptions` 可以**大幅简化**：只保留 `alias` + 超时，详见 §10.4。
- 两个别名（有线 / 无线）可作为**同一工控机的两条接入路径**：
  有线优先（`myserver3`），无线作为备份（`myserver5`）。建议 CLI 做成
  `--host myserver3` 可覆盖，并在无线网段下自动回退。

**第 12 项的连带影响（控制端编译 + 拷贝部署）：**

- 两台机同为 **x64**，控制端 g++ 目标 `x86_64-w64-mingw32`，与工控机的 x64 SDK 匹配，**无需交叉编译**。
- 工控机**只装运行时**：`businessd.exe` + MinGW 运行库 + `site.json`。源码、测试、Qt、CMake 一律不过去。
- 部署包放 `output/`（当前为空目录，阶段 6 开始填充）。
- 部署脚本刻意**不覆盖 `site.json`** —— 那是工控机的本地站点配置，含本机 COM 口与 SDK 路径。
- 测试（阶段 1 - 3）全在控制端离线跑，**不需要设备在场**；只有阶段 2 之后的联调需要工控机。

### 10.2 仍待确认

1. **工控机 PowerShell 版本**：5.1（系统自带）还是 7？影响远端命令的引号转义写法。
   → **到现场自己确认**：`ssh myserver3 "powershell -NoProfile -Command \"$PSVersionTable.PSVersion\""`
2. **工控机是否已装 Python**：需在工控机上装（建议官方 installer，加 PATH）。
   → 确认方式：`ssh myserver3 "python --version"`（若报"不是内部或外部命令"就是没装）
3. **UI 的具体形态与部署位置**：本机触摸屏还是别的？UI 跑在工控机本机还是控制端？
   （延后到阶段 7 再定，但会影响 §8.2 的方案取舍。）

### 10.3 计划任务 vs Windows 服务

两者都能"开机自动运行"，区别如下：

| 维度 | 计划任务（Task Scheduler） | Windows 服务（Service） |
| --- | --- | --- |
| 启动时机 | **用户登录后**启动 | **开机即启动**，无需登录 |
| 运行身份 | 指定用户（交互式会话） | `LocalSystem` / `LocalService` / 账号 |
| 前台窗口 | **有**（可在工控机屏幕上看到） | 无（纯后台） |
| 出错弹窗 | 有（可见） | 无（只能看日志/事件查看器） |
| 安装复杂度 | 低：图形界面点几下，或 `Register-ScheduledTask` | 中：需 `sc create` + 服务包装器 |
| 崩溃自动重启 | ✅ 任务设置里勾"如果失败则重新启动" | 需在服务代码里自己 `SetServiceStatus` 上报 |
| 标准输入/输出 | 有 | 无 |
| 现场调试 | **方便**（能直接看到报错） | 不方便 |
| 权限要求 | 普通用户即可 | 需管理员权限安装 |

**给本项目的建议：先用计划任务。** 理由：

- 守护进程**大概率在有人登录的工控机上跑**（有触摸屏、要现场调试），"登录后启动"完全够用。
- 现场调试时**能在屏幕上直接看到输出**，这在初期排查串口 / SDK 问题非常有用。
- 崩溃自动重启只需在任务设置里勾一个选项，不用写额外代码。
- 不需要管理员权限，也就不需要"提权安装"这一步的文档和审批。

**什么情况下改成服务：**

- 需要**无人登录**也运行（例如工控机锁屏 / 无人值守运行）。
- 需要"以 `LocalSystem` 身份访问某些受保护资源"（本项目暂不需要 —— 串口和 AvaSpec SDK 普通用户即可）。
- 需要更规范的服务状态上报与日志集成。

**计划任务的注册命令（示例）：**

```powershell
# 触发器：系统启动时；动作：运行 lpc_businessd.exe
# 建议勾选"如果任务失败，按 1 分钟间隔重新启动" + "取消任务计划后停止"
Register-ScheduledTask -TaskName 'LibsBusinessd' `
  -Action (New-ScheduledTaskAction -Execute 'D:\LIBS\output\businessd.exe' `
                                 -WorkingDirectory 'D:\LIBS\output') `
  -Trigger    (New-ScheduledTaskTrigger -AtStartup) `
  -Settings   (New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
                                       -ExecutionTimeLimit ([TimeSpan]::Zero) `
                                       -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries) `
  -Principal  (New-ScheduledTaskPrincipal -UserId 'user' -LogonType Interactive -RunLevel Limited)
```

> 关键点：
> - `ExecutionTimeLimit = 0` 表示**不限制运行时长**（默认 3 天会强杀，对长驻进程是致命的）。
> - `RestartCount` / `RestartInterval` 实现崩溃自动重启。
> - `LogonType Interactive` = 用户登录后以该用户身份运行（可见窗口）。
换认证方式见 §7.2 / §10.4。

> **如果后续想改成 Windows 服务**，见 §10.8 —— 由于守护进程只对外暴露命名管道，
> 换壳成本很低（约 100 行服务壳 + 一个安装脚本），管道协议与业务层零改动。

### 10.4 SSH 配置已就绪带来的简化

`~/.ssh/config` 里已有别名，因此**代码里不需要存 IP / 用户名 / 私钥路径**：

```text
Host myserver3          # 有线
    HostName 192.168.101.100
    User user
    Port 22
    IdentityFile ~/.ssh\ipc_key
    IdentitiesOnly yes

Host myserver5          # 无线（Tailscale 网段 100.82.118.99）
    HostName 100.82.118.99
    User user
    Port 22
    IdentityFile ~/.ssh\ipc_key
    IdentitiesOnly yes
```

- `lpc-cli` 直接 `ssh myserver3 "命令"` 即可，其余由 ssh 客户端自己解析。
- 建议 CLI 支持 `--host <alias>` 覆盖，默认 `myserver3`；**无线网段下回退到 `myserver5`**。
- 私钥**只在 `~/.ssh/` 里**，不进代码库、不进项目配置、不进日志。
- 若将来希望"换一台工控机不改代码"，只需在 `~/.ssh/config` 加一个新别名，
  命令行 `--host newalias` 即可 —— **这也顺便满足"单站点、不多机"的要求**。
- 建议给工控机条目补两项（可选，纯粹为了体验）：
  - `ServerAliveInterval 30` / `ServerAliveCountMax 3` —— 长任务时保持连接（`myserver2` 已有此配置）
  - `ControlMaster auto` / `ControlPath` —— 多条命令复用一条 SSH 连接，省去重复握手

### 10.5 编译与部署路径（已确认：控制端编译 + 拷贝部署）

**决策：在控制端编译，拷贝到工控机运行。工控机零开发环境。**

| 项 | 控制端（编译机） | 工控机（运行机） |
| --- | --- | --- |
| 系统架构 | **x64**（实测 `AMD64`） | x64（SDK 为 `X64` 目录，已确认） |
| 编译器 | `x86_64-w64-mingw32`，g++ 13.1.0 | **不需要** |
| CMake / MinGW | 已装（`D:\Qt6\Tools\`） | **不需要** |
| Qt | MinGW 6.11.2（仅 UI 需要） | **不需要**（除非装 UI） |
| Python | 3.14.6 | 建议装（仅 CLI 需要） |
| 源码 | 唯一副本 | **不放** |
| 产物 | 编译 exe + 运行库 | 只需 `businessd.exe` + 运行库 + 配置 |

两台机同为 x64，MinGW 目标 `x86_64-w64-mingw32` 与工控机的 x64 SDK 匹配，**架构上无冲突**。

**部署包内容**（`output/` 目录，当前为空，阶段 6 开始填充）：

```text
output/
├── businessd.exe                    # 守护进程（C++）
├── site.json                        # 站点配置（端口表 + SDK 路径）
├── libgcc_s_seh-1.dll               # MinGW 运行库
├── libwinpthread-1.dll              # MinGW 运行库
└── remote/                          # Python CLI（若在工控机侧执行）
    └── lpc_cli/
```

**不部署到工控机的东西**：源码、`build/` 临时产物、测试程序、Qt 开发文件、`third_party`。

**工作流：**

```text
控制端                          工控机
─────────────────────          ─────────────────────
改业务代码
  ↓
pwsh -File build.ps1             （只跑 455 条离线测试，不碰硬件）
  ↓
cmake --build build\cmake-nj     （产出 businessd.exe）
  ↓
robocopy output\ myserver3:D:\LIBS\output\ /MIR
                                 计划任务启动 businessd.exe
                                 独占设备，开始服务
```

**为什么这样分：**

- 工控机只装运行时（几十 MB），不装编译环境，也不放源码。
- 构建与部署分离，**CI 可自动产出部署包**。
- 现场改一行业务规则：控制端改 → 重编 → 覆盖一个文件，**不需要登录工控机装任何东西**。
- 测试（阶段 1 - 3）全在控制端离线跑，**不需要设备在场**。

**注意事项：**

| 事项 | 说明 |
| --- | --- |
| 架构一致性 | 已是 x64 → x64，**不需交叉编译**。若将来要出 x86 版，MinGW 需另装 i686 工具链 |
| AvaSpec 运行时 | 由工控机提供（`C:\Avantes\AvaSpecX64-DLL_9.14.0.0`），**不随部署包分发** |
| 运行库缺失 | MinGW 动态链接需 `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`，漏了会在工控机报 `0xc0000135` |
| 配置不进包 | `site.json` 的 SDK 路径是**工控机本地路径**，应在工控机现场生成/修改，不从控制端覆盖 |
| 部署校验 | 部署后 `ssh myserver3 "powershell -NoProfile -Command \"Get-Process businessd\""` 确认进程存活 |

**建议加一个部署脚本**（阶段 6 交付）：

```powershell
# deploy.ps1 —— 编译 + 打包 + 拷贝 + 重启守护进程，一条命令
param([string]$Host_ = "myserver3", [string]$RemoteDir = "D:\LIBS\output")
# 1) build.ps1 跑离线测试
# 2) cmake --build 产出 businessd.exe
# 3) robocopy output\  "$Host_`:$RemoteDir\" /MIR /XF site.json
# 4) ssh $Host_ "schtasks /End /TN LibsBusinessd"；重启任务
# 5) ssh $Host_ 读日志，确认启动成功
```

> `site.json` 故意**不覆盖**（`/XF site.json`）：它是工控机的本地站点配置，
> 里面记着本机的 COM 口和 SDK 路径，从控制端覆盖会毁掉现场配置。

### 10.6 AvaSpec SDK 的链接方式（待现场确认）

现有 `Spectrometer::open(sdk_dll)` 是**运行时加载** `AvaSpec.dll`，因此构建期只依赖头文件与类型定义，
DLL 本身由工控机提供。只要保证下面三点即可：

| 事项 | 要求 |
| --- | --- |
| 运行时路径 | `SiteConfig.spectrometer_dlls` 指向 `C:\Avantes\AvaSpecX64-DLL_9.14.0.0`，**可被配置覆盖** |
| 目录名含版本号 | `9.14.0.0` 会随 SDK 升级变化，**不要把版本号写死在代码里** |
| 架构匹配 | 目录名含 `X64`，说明 SDK 仅 x64 → 守护进程**必须编译为 x64** |
| DLL 搜索路径 | 运行时把 SDK 目录加进进程搜索路径（或用绝对路径 `LoadLibrary`），避免依赖 `PATH` |

**需现场用 `dir` 确认的内容**（补进本节）：

```powershell
ssh myserver3 "dir C:\Avantes\AvaSpecX64-DLL_9.14.0.0"
```

重点看有没有 `.lib` / `.def`：有 `.lib` 才能做链接期检查（只做 import lib 也够，不必静态链）；
只有 `.dll` 也没有关系 —— 现有实现走 `GetProcAddress` 动态解析，不做链接期依赖。

### 10.7 当前网络现状（供现场排查参考）

从控制端实测（2026-09-29）：

| 目标 | 22 端口可达性 | 说明 |
| --- | --- | --- |
| `myserver3` `192.168.101.100`（有线） | **不可达** | 控制端当前不在该网段 / 线未插 |
| `myserver5` `100.82.118.99`（无线） | **不可达** | 同上 |

这不是故障，只是**当前控制端不在现场网络里**。到现场后先跑：

```powershell
Test-NetConnection 192.168.101.100 -Port 22
ssh myserver3 "hostname"
```

### 10.8 可服务化：计划任务 → Windows 服务

**结论：好换。** 难的不是"换壳"，而是"别把业务写死在壳里"。
因为本项目的守护进程**对外只暴露一个命名管道**（§7.4），
启动方式（计划任务 / 服务 / 手工终端）对上层完全不可见，所以换壳的改动量极小。

#### 10.8.1 两种方式的差异其实只有三件事

| 事项 | 计划任务 | Windows 服务 | 换起来难吗 |
| --- | --- | --- | --- |
| 启动时机 | 用户登录后 | 开机即启动（无人登录也行） | 无关代码 |
| 与 SCM 通信 | 不需要 | 需要 `ServiceMain` + 状态上报 | **唯一真正的技术活** |
| 安装权限 | 普通用户即可 | 需管理员 | 无关代码，换个安装脚本 |

管道协议、串口独占、AvaSpec 单实例管理全在服务内部，**换壳时一行都不用动**。

#### 10.8.2 工具链已实测：MinGW 可直接编服务 API

在本机验证过（无需引入任何新依赖）：

- `RegisterServiceCtrlHandlerExW` / `StartServiceCtrlDispatcherW` 编译**通过**（`-ladvapi32`）；
- 需要的 `advapi32` 属系统默认库，**不需要额外 SDK 或 import lib**。

#### 10.8.3 三种做法（从省事到彻底）

**A. NSSM / WinSW 包装（零代码改动）**

```powershell
nssm install LibsBusinessd D:\LIBS\output\businessd.exe
nssm set LibsBusinessd AppStdout D:\LIBS\output\logs\out.log
nssm set LibsBusinessd AppStderr D:\LIBS\output\logs\err.log
nssm set LibsBusinessd Start SERVICE_AUTO_START
```

`businessd.exe` 源码一行不动，由 NSSM 当服务壳。
代价：工控机需装 NSSM（单个 exe，拷过去即可）。**本机当前未安装 NSSM / WinSW。**

**B. 自己在 C++ 里写服务壳（推荐，零外部依赖）**

约 100 行，加在 `businessd/main.cpp`：

```cpp
// businessd/main.cpp
#include <windows.h>

// 真正的业务入口：所有逻辑从这里开始，与"怎么被启动"无关
int RunService();          // 打开设备 -> 服务命名管道 -> 循环 -> 退出清理
int RunConsole();          // 交互式调试模式（带前台窗口、可用 stdout）

namespace {
void WINAPI Handler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        SetEvent(g_stop_event);   // 通知主循环优雅退出
    }
}

DWORD WINAPI ServiceMain(DWORD, LPWSTR*) {
    SERVICE_STATUS_HANDLE h =
        RegisterServiceCtrlHandlerExW(L"LibsBusinessd", Handler, nullptr);
    if (!h) return 1;

    SERVICE_STATUS st{};
    st.dwServiceType            = SERVICE_WIN32_OWN_PROCESS;
    st.dwCurrentState           = SERVICE_START_PENDING;
    st.dwWaitHint               = 5000;   // 打开 4 个串口 + 4 台光谱仪可能要几秒
    st.dwControlsAccepted       = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(h, &st);

    // ---- 真正干活：与计划任务模式共用同一个入口 ----
    RunService();

    st.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(h, &st);
    return 0;
}
} // namespace

int wmain() {
    SERVICE_TABLE_ENTRYW table[] = {
        { L"LibsBusinessd", reinterpret_cast<LPSERVICE_MAIN>(ServiceMain) },
        { nullptr, nullptr }
    };

    // 被 SCM 启动时，这条调用会成功并**接管本进程**（不会返回）
    if (StartServiceCtrlDispatcherW(table)) return 0;

    // 返回 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT 说明不是服务，
    // 即有人在终端里手工运行 -> 走控制台模式
    if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        return RunConsole();

    return 1;
}
```

**关键技巧：同一个 exe 同时支持"服务模式"和"控制台模式"。**
现场调试时仍可在终端直接 `businessd.exe` 看输出、立即看到报错；
正式安装为服务后由 SCM 启动，自动走服务路径。两者复用同一份业务代码。

CMake 侧只需一个 target：

```cmake
add_executable(businessd businessd/main.cpp)
target_link_libraries(businessd PRIVATE libs_business)
target_link_libraries(businessd PRIVATE libs_warnings advapi32)   # advapi32 仅服务模式需要
```

> 注意：如果用 MinGW，通常还要在链接时加上 `-municode`（因为入口是 `wmain`），
> 或把入口改成 `main` 并显式调用 `wmain`。二选一，取决于你其它 target 的现状。

**C. 不换，一直用计划任务**

若工控机总是有人登录、有触摸屏，**计划任务其实已经够用**，不必换。

#### 10.8.4 什么时候才真的需要换成服务

三个具体触发条件，满足其一才换：

1. **需要无人登录也运行** —— 工控机锁屏 / 无人值守采集。
2. **需要开机就绪** —— 计划任务是"登录后启动"；若设备上电后要立刻开始采集、不等人登录，服务才合适。
3. **需要规范的状态与日志管理** —— 工作目录、环境变量、崩溃恢复由系统统一接管。

> 对本项目而言，第 1、2 条要等**现场确认"是否有人长期登录"**之后才判断。
> §10.2 第 1 项（PowerShell 版本）现场确认时可以顺带看一眼工控机是否常驻登录态。

#### 10.8.5 现在就应遵守的三条约定（避免以后返工）

1. **业务逻辑一律写在 `RunService()` 入口之后**。服务壳只做"转发到入口"这一件事，
   不在壳里读写设备、不在壳里加状态机。
2. **日志一律写文件 + `OutputDebugString`，不依赖 stdout/stderr。**
   原因是**服务模式下 stdin/stdout/stderr 全是空的**（`NSSM` 方案下需要显式重定向）。
   这条现在就要注意 —— 计划任务的前台窗口会让人误以为 stdout 可用，切换时会突然"日志全没了"。
3. **不要用标准输入做控制通道**（例如"按 Ctrl+C 停止"）。服务模式下收不到控制台事件，
   停止必须走 `SetEvent(g_stop_event)` 这类显式路径（见 §10.8.3 的 `Handler`）。

#### 10.8.6 切换步骤（真要换时照做）

```powershell
# 1) 停掉计划任务并注销
Disable-ScheduledTask -TaskName 'LibsBusinessd' -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'LibsBusinessd' -Confirm:$false -ErrorAction SilentlyContinue

# 2) 编译带服务壳的 businessd.exe（x64，见 §10.1 第 6 项）
#    MinGW 产出，同时带上 libgcc_s_seh-1.dll / libwinpthread-1.dll

# 3) 覆盖工控机上的 exe（服务未安装前不影响运行）

# 4) 注册服务（需管理员 PowerShell）
sc.exe create LibsBusinessd binPath= "D:\LIBS\output\businessd.exe" start= auto
sc.exe failure LibsBusinessd reset= 86400 actions= restart/60000/restart/60000/restart/60000
sc.exe description LibsBusinessd "LIBS device daemon: serial + AvaSpec owner, named-pipe API"

# 5) 启动并验证
Start-Service LibsBusinessd
Get-Service  LibsBusinessd
```

**验证守护进程真的起来了（无论计划任务还是服务）：**

```powershell
# 命名管道应存在（客户端能连上 = 守护进程活着）
Get-ChildItem \\.\pipe\ | Where-Object Name -match 'libs-biz'
# 串口只被它持有：COM7/8/10/12 应无其他占用
ssh myserver3 "powershell -NoProfile -Command \"Get-CimInstance Win32_SerialPort | Select-Object DeviceID\""
```

> `sc.exe failure ... actions= restart/60000/...` 用来做崩溃自动重启，
> 等价于计划任务里的 `RestartCount` + `RestartInterval`，**不会因为换壳而丢失这项能力**。

#### 10.8.7 切换成本小结

| 项目 | 改动量 |
| --- | --- |
| `businessd/main.cpp` | 加服务壳，约 100 行（§10.8.3 已有完整代码） |
| `CMakeLists.txt` | 链接 `advapi32`，加 `-municode`（若入口为 `wmain`） |
| 安装脚本 | 新增 `install-service.ps1`；计划任务注册脚本退役 |
| **管道协议** | **零改动** |
| **Python CLI** | **零改动**（它只连命名管道） |
| **业务层 `business/`** | **零改动** |
| **UI** | **零改动** |
| 运行时依赖 | 仍然只有 MinGW 运行库 + 工控机自带的 AvaSpec 运行时 |

**所以结论是：架构上已经把成本降到最低了。** 只要守住 §10.8.5 那三条约定，
现在用计划任务、以后改服务，就是"加一个壳 + 换个安装脚本"，不涉及任何协议或业务改动。

## 附录 A：现状中发现的文档陈旧点（仅记录，未擅自修改）

核实磁盘后发现的**文档与实际不一致**之处，列出供你决定是否修正：

- 旧实现目录 **`cpp/` 在磁盘上已不存在**，但 `README.md` 与 `docs/refactor-report.md`
  中仍有多处段落以其存在为前提在描述（例如 README 第 45、268 行；
  refactor-report 第 3、4、13、154、160 - 165、193、207、470、480 行）。
  这些段落应改为指向 `src/` 下的新结构，或标注为"重构前的历史状态"。
- `D:\LIBS` 下**没有 `.git` 目录**，目前无法用版本控制比对或回滚改动。是否需要 `git init`？
