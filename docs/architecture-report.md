# LIBS 架构报告

> 生成日期：2026-10-01
> 对应代码状态：`D:\LIBS\src`，`build.ps1` 全绿（12 套测试 / 1084 断言）
> 本文是**现状描述**，不是设计提案。凡与代码不符，以代码为准。

---

## 1. 总体形态

系统 = **两个进程 + 三层后端**。

```text
+---------------------------------------------------+
|  ui/libs-spectrum-ui.exe                          |  Qt6 Widgets + Charts
|  实时光谱预览 / 参数下发 / 出光按钮 / 门禁指示     |  只链接 Qt6
+---------------------+-----------------------------+
                      |  命名管道 \\.\pipe\libs-businessd
                      |  JSON Lines（1 请求 = 1 行，1 响应 = 1 行）
                      v
+---------------------------------------------------+
|  src/businessd.exe                                 |  守护进程，独占设备
|  facade 命令面（14 条）-> Runtime -> 两条业务       |  <- 判断权全部在这里
+---------------------+-----------------------------+
                      v
      business/   readiness（就绪判定）+ measurement（激光触发光谱检测）
          |       + shared（共同底座）+ app（装配）
          v
      devices/    每台设备一个公开控制器
                  | 设备动作、时序、读回确认；protocols/ 编入本层
                  v
      transport/  Link 抽象 + Win32 COM 串口收发与超时
```

`ui/` 与 `src/` 是**两个独立的 CMake 工程**，彼此不链接。UI 与后端唯一的接口就是命名管道上的 JSON 行。

### 1.1 依赖方向

收敛为一条直线，**反向零引用**：

```text
business/  ──>  devices/  ──>  transport/
```

这不是文档约定，而是构建约束：

- `build.ps1` 把源文件划成 `libs_transport` / `libs_devices` / `libs_business` 三组，
  顺序即依赖顺序，逐层静态库链接。
- `build.ps1` 里有一条 grep 守卫：`business/shared/` 下的源文件一旦出现
  `business/(readiness|measurement)/` 的 include，**构建直接 throw**。
  实测输出 `shared/ 反向依赖检查通过（7 个文件）`。

第四条线 `businessd` 只做进程边界，不含业务逻辑。

### 1.2 有意不做的部分

| 不做 | 处理方式 |
| --- | --- |
| SSH 隧道封装 | 传输交给系统 `ssh.exe` |
| 光谱 -> 成分算法 | 接口就绪，无算法时如实返回 `unavailable`，不返回假结果 |
| 硬编码端口映射 | 全部来自 `site.json` |
| 静默回落配置默认值 | 缺失即 `invalid_argument` 报错 |
| 空调 / 测距仪的运行时持有 | 曾打开但全库无业务读过（空调联锁阈值始终未标定），已从 `DeviceSession` 移除 |

---

## 2. 逐文件职责

`xxx.hpp` 在 `src/include/libs/`，`xxx.cpp` 在 `src/src/`。

### 2.1 公共基础（2 对）

| 文件 | 作用 |
| --- | --- |
| `error.hpp` / `error.cpp` | 全库统一 `Error{kind, code, message, raw}` + `Result<T>` + `VoidResult`。8 种 `ErrorKind`（invalid_argument / unsupported / protocol / transport / device / timeout / internal / none）。**不用异常做控制流**，只有编程错误才抛。`raw` 保留出错原始字节便于事后复盘。 |
| `hex.hpp` / `hex.cpp` | `Bytes` 别名（`vector<uint8_t>`）+ 大写空格分隔 hex 串。 |

`Result<T>` 有两个反直觉但正确的设计：

- **默认构造是"成功"**，不是失败。因为全库用 `return VoidResult();` 表达
  "无返回值且成功"。若默认是失败，那些位置会静默地把成功变成 `kind==none`
  的失败，而调用方看 `ok()` 又会走到取值一侧。
- 对没有默认构造的 `T`，该构造**故意不可用**——这是想要的信号：
  要么给值，要么给 error，不要靠默认构造。

### 2.2 transport（1 对）

| 文件 | 作用 |
| --- | --- |
| `transport/serial.hpp` / `serial.cpp` | 抽象接口 `Link::exchange(request, timeout_ms, idle_ms)`，返回 `Reply{bytes, elapsed_ms, receive_errors, last_receive_tick, timed_out}`。`open_serial()` 开 Windows COM 口（`share=0` 独占），`list_serial_ports()` 只列名字不打开。只搬字节，不理解任何设备业务。 |

`Link` 是**所有串口设备离线测试的注入边界**——测试里的 `FakeLink` 就是实现这个接口，
因此整套设备层测试不需要真实 COM 口。串口互斥（`Runtime::device_mutex`）
是进程级的，不属于传输层。

### 2.3 protocols（5 对）—— 纯编解码，零 I/O、零 sleep

`protocols/` 保留原目录名，但**已不是独立公开层**：编入 `libs_devices`，
业务层不允许直接 `#include "libs/protocols/..."`。

| 文件 | 作用 |
| --- | --- |
| `protocols/modbus_rtu.hpp/.cpp` | `crc16`、组帧、**`locate_reply()`**。后者是协议层最有价值的函数，处理真实世界的三种串口噪声：请求回显（485 转换器常把请求原样送回）、前后垃圾字节、CRC 错。`reject_request_echo` 必须由调用方按协议决定：空调 FC05/FC06 的写响应按 Modbus 规定就是把请求原样送回，**传 true 会把真响应当回显丢掉**。 |
| `protocols/laser.hpp/.cpp` | 激光器 `5A A5` 私有帧的 CRC、组帧、解码、字段提取。`find_frame()` 在含噪声的流里扫帧，返回 `complete` / `partial`（继续等）/ `invalid`（放弃）三态。`Field::raw32` 单列，因为放电计数器是 4 字节，塞进 `uint16` 会被静默截断（曾发生过）。 |
| `protocols/rangefinder.hpp/.cpp` | TOF101 保持寄存器（FC03）。**只交出 raw 值**，不做 mm/cm 换算、不判超量程哨兵——那是业务的事。 |
| `protocols/indicator.hpp/.cpp` | 科隆威三色灯。查询地址空间（红1/绿2/蓝3/黄4/白5/蜂鸣6）与写线圈地址空间（红黄绿蜂鸣=1/2/3/4，K50 黄绿互换）**故意分开不混用**。`ModelTraits` 登记型号能力：未登记的型号返回错误，**一个字节都不生成**。 |
| `protocols/air_conditioner.hpp/.cpp` | 空调 V2。API 一律用**文档地址**（10001/30001/40001），偏移换算封装在内部。 |

空调协议有一处**厂商文档笔误**已在代码里纠正并用测试钉死：
手册 §4.5 说 FC05 的 `sReg = 寄存器号 - 00001`，但厂商自己的样本 §7.11/§7.12
写开关机用的偏移是 `0x0029 = 41 = 10042 - 10001`。按 00001 换算得到 10041，设备不认。
**本实现以实测样本为准。**

另有两个设备行为（本库只转达，不实现）：
1. 模式控制（30026）必须**持续下发**，连续 100 s 收不到会自动切回自动运行。
2. `10042` 开关机位在回读里是**低有效**（0 = 开机），与 FC05 写线圈
   `0xFF00` = 开的约定方向相反。协议层如实上报，不做反相。

### 2.4 devices（5 对）—— 时序与单事务编排

`devices` 是唯一公开给业务层的设备入口层。每台设备只有一个控制器类。

| 文件 | 作用 |
| --- | --- |
| `devices/laser.hpp/.cpp` | 风冷激光器。`readActual` / `readSettings` / `readTec` / `resetFault` / `clearCounter` / `prepareContinuous` / `openQGate` / `startContinuous` / `stopContinuous` / `fireOnce`。出光时序全在这里。`LaserActual` **保留设备原始极性**（`q_gate=1` 是光闸开），不翻译成"安全/危险"——那是 readiness 的事。`LaserOperationTrace` 留逐帧请求与 before/after 证据。**安全禁区**：0x15/0x17/0x18 无公开写入口，0x31 不进自动流程，0x34 不作出光路径，TEC 只读。 |
| `devices/spectrometer.hpp/.cpp` | AvaSpec SDK **动态加载**（32.8 KB，设备层最大文件）。`open` / `enumerate` / `capture` / `stream` / `close` 生命周期，`close()` 必须在析构前显式调用。含 `wait_for_laser_trigger` + `integration_delay_ns` 的外触发路径（手册 §4.9 接法二，光谱仪从 pin 6 收激光 TTL）。`cycles_to_nanos` 保留 FPGA 计数的 -1 偏置换算。`sdk_abi()` 用于在日志里核对结构体布局与厂商头一致。 |
| `devices/indicator.hpp/.cpp` | 状态灯。`apply` / `applyAll` / `queryAll` / `probeModel`。协议要求相邻命令间隔 > 50 ms，控制器内部 `enforce_gap()` 强制执行——**业务层只传灯色意图，不操心时序**。 |
| `devices/rangefinder.hpp/.cpp` | 极薄（1.4 KB）：`readRawRegisters` / `observeOnce`，只读原始寄存器。 |
| `devices/air_conditioner.hpp/.cpp` | 空调读写。`held_mode_` / `last_mode_sent_ms_` 只是**如实记录上次何时下发过**，不替业务做保活策略。 |

### 2.5 business/shared（7 对）—— 跨两条业务的底座

**关键纪律：`shared/` 绝不能 include `business/readiness/` 或 `business/measurement/`**
（build.ps1 grep 守卫）。它是共同底座，不属于任何一个业务。

| 文件 | 作用 |
| --- | --- |
| `shared/time_util.hpp/.cpp` | 两种时间**分开不混**：`wall_clock_ms()`（对齐日志，会被校时改）vs `monotonic_ms()`（只算间隔，不受校时影响）。跨进程序列化成确定的整数。 |
| `shared/json/json.hpp/.cpp` | 自写的最小 JSON。**不用第三方是为了跨进程接口不引入 ABI 风险**（服务从第一天起就是跨进程可调的）。数值一律 double；解析严格，多余字符/坏转义/尾逗号一律报错，不做"尽力解析"。不是通用库，不要往里加注释/格式化/模式校验。 |
| `shared/config/site_config.hpp/.cpp` | **装机配置 = 硬件事实**（错了机器接不上）。端口/波特/超时/使能全部来自文件，**不硬编码**。字段非法返回 `invalid_argument`，**禁止静默回落默认值**（回落意味着开错设备、连错口）。`config validate` 不开任何串口。刻意**不**保留任何积分时间/超时——那些是测量参数。 |
| `shared/config/parameter_store.hpp/.cpp` | **测量参数 = 工艺事实**（错了只是这次测不准），装在 `parameters.json`。核心是"参数设置已完成"的两态语义：收到设置请求 != 设置完成，落盘失败/校验失败**全部**算未完成。 |
| `shared/logging/run_log.hpp/.cpp` | 运行日志（JSON Lines 追加写，**不缓冲到内存等程序退出**——现场断电是常态，未落盘的日志等于没有）+ 恢复上下文快照。快照仓库只有"写入 + 查询"，**没有 `auto_resume()`**，这是刻意留的缺口。 |
| `shared/session/device_session.hpp/.cpp` | **进程内唯一持有设备对象的地方**，业务层别处只拿引用。把"设备独占"从物理约束变成可测试的代码约束。`DeviceState::enabled=false`（配置上没接）与"打开失败"是两个语义，必须分开，否则调用方会把配置问题当成故障。 |
| `shared/data_delivery.hpp/.cpp` | 交付层。`LocalFileSink` 落盘成功才算 `local_saved`；`UnsupportedGatewaySink` 返回 `unsupported` + 前置条件清单。`local_saved` 与 `gateway_delivered` 是**两个独立状态位，不是三态枚举**——本地存了不等于海关收到了。 |

### 2.6 business/readiness（5 对）—— 业务一：就绪判定

内部是一条流水线：

```text
设备事实(Laser B2) ──poll──> ReadinessMonitor ──> evaluate() 纯函数
                                                        │
                                            GateVerdict{allowed, blocked_by, detail}
                                                        │
                                                        v
                                             lamp_states() 纯函数 ──> apply_lamp() 唯一写灯点
                                              （由 PrecheckService 编排）
```

| 文件 | 作用 |
| --- | --- |
| `readiness/facts.hpp/.cpp` | **只承载事实，一个判断都不做**。`parse_actual()` 是 B2 -> 安全位的**唯一转换点**，语义统一为 `true = 安全`（协议里 0/1 极性是反的，逐位取反，**取反不是笔误**）。`available=false` 按不安全处理，绝不当"没事"。`ReadinessFacts` 抽成结构体而非五个 bool 入参，是为了让 `evaluate()` 变成可离线穷举的纯函数。 |
| `readiness/verdict.hpp/.cpp` | `evaluate()` 是**零设备 I/O 的纯函数**。`GateVerdict` 不只是过/不过，还要能回答**为什么不过**（`blocked_by` 逐条列出）。 |
| `readiness/lamp.hpp/.cpp` | `lamp_states()` 状态->灯色**纯函数**；`apply_lamp()` **全库唯一灯下发点**。三条纪律：灯是纯函数输出、**不得反向参与门禁判定**；灯写失败**不改变 verdict 也不阻断返回值**（灯是辅助指示，不是判据）；**失败路径也要刷灯**（下发在返回错误之前，否则操作员只看到"点了没反应"）。蜂鸣器恒 off。 |
| `readiness/monitor.hpp/.cpp` | 回答**何时判**。`FactSource` 用回调而非持设备指针——本类不该知道设备怎么开关。`start_continuous()` / `stop_continuous()` 声明留着、暂不实现，**接口先定死才能保证将来不返工**。`collect_facts()` 对 `armed` 强制归位（防事实源覆掉布防状态），其余配置事实允许覆写。 |
| `readiness/precheck.hpp/.cpp` | **唯一**把"门禁结论"变成"物理动作（写灯）"的地方。出光服务自己不刷灯，UI 也不刷灯。**预检与亮灯不拆成两个**（拆开会造出"预检说过了、灯还没亮"的无意义中间态）。另有 `force_offline()` 只给关机路径用——软件都要关了，留一圈灯在假装系统还活着，下次开机前现场看到一片黄的机器会误以为它还可用。 |

### 2.7 business/measurement（5 对）—— 业务二：激光触发光谱检测

流程：**连续出光 → 每次出光被光谱仪采到 → 落盘 → 实时显示**。

| 文件 | 作用 |
| --- | --- |
| `measurement/frame.hpp/.cpp` | **纯数据结构 + 序列化，零行为**（无线程、无设备、无队列）。依赖刻意收到最窄：只 include json 与 time_util。`SpectrumFrame.laser_shot` 是关键字段：`true` = 对应一次真实出光（LIBS 测量谱），`false` = 自由触发拍到的背景谱（**诚实的观测值但不是测量值**）。单看 `wavelength_nm + counts` 分不出这两者，所以它必须随帧携带。`ChannelState::absent` 是**第三态，不补零**——补零画出来是条平线，那是谎报。帧里不做显示加工（无归一化、无基线扣除）。 |
| `measurement/frame_bus.hpp/.cpp` | 纯内存发布订阅，**不碰设备、不起线程、不持 `device_mutex`**。所有权在 **Runtime 而不是 MeasurementJob**——UI 必须在 `measurement_start` 之前就能订阅，作业起不来时 UI 也要能看到"通道是空的"这件事。背压策略 = **丢最旧帧**，如实计 `dropped_frames`，绝不让慢客户端拖住采集。订阅者队列不属于任何 GUI 线程。帧号由采集侧单调自增，与丢弃计数**完全无关**（早先算成 `dropped + captured`，漏一帧会让后续帧号全部错位）。 |
| `measurement/capture_plan.hpp/.cpp` | **纯函数**，把字段归属钉死成两条：装机配置供"接了哪几台/主从/SYNC"，参数库供"积分时间/超时/积分延时/扫描次数"。`wait_for_laser_trigger` **硬编码 true 不可配置**——触发源就是激光器，这是接法决定的物理事实。 |
| `measurement/writer.hpp/.cpp` | 独立线程 + 有界队列（limit 64，满丢最旧并计数）。**安全路径与数据路径解耦**：收光那条命令不能排在磁盘后面，所以写盘在独立线程且不在任何设备锁内。磁盘卡住也不能拖住出光节奏。`stop()` 置位后把队列剩余帧写完才退出——**停机不是丢数据的理由**。 |
| `measurement/job.hpp/.cpp` | **作业主体，全库唯一同时碰激光器和光谱仪的对象**。19 KB 实现 + 41 KB 测试。七条纪律写在文件头，见 §4。 |

**为什么光闸一直开着（方案 A）**：`prepareContinuous` 到设备进入
"正在工作且放电"要 5~6 秒（实测），这个自适应过程不可能每发都重走。
所以改成：激光按设定频率自己打，每打一发出一个 TTL，光谱仪在 pin 6 收到就采一次。
于是**每一帧天然对应一次出光**，不需要软件数拍子，频率调快也不会漏
（`capture` 是阻塞的，来不及就等下一个 TTL）。

### 2.8 business/app（3 对）—— 装配与命令面

| 文件 | 作用 |
| --- | --- |
| `app/runtime.hpp/.cpp` | 依赖装配 + 生命周期，**不含任何业务逻辑**。铁律写在文件头：**本文件里不允许出现任何判定或出光决策**。具体到代码：判定只有 `evaluate_now()` 一处，出光只有 `MeasurementJob` 一处。`device_mutex()` 留在 Runtime，因为串口互斥是**进程级**的，不属于任何一个业务。`MeasurementJob` 用 `unique_ptr` 延迟到 `start()` 构造——它的依赖（激光器句柄、光谱仪句柄、落盘 writer、门禁）全都是 `start()` 里才拿到的，在成员初始化列表里构造就只能传一堆 nullptr 再补绑。还负责出光会话互斥（一个布尔 + 一把锁，拦的是"重复出光"）。 |
| `app/parameter_service.hpp/.cpp` | **测量参数的唯一来源**。`fire_parameters()` 组装出光参数并做量程校验（参数不合法返回 `invalid_argument`，**不会带着坏参数去动设备**）。`spectrometer_measurement()` 用 `value_or()` 回落，专供预览——否则"激光器还没标定"会让**预览也起不来**，而预览只是拍照，与能不能出光无关。 |
| `app/facade.hpp/.cpp` | 命令面（38.8 KB，业务层最大）。**每个方法对应一条 CLI 子命令**，`lpc.py` 只是薄壳。坏 JSON / 缺字段 / 未知命令都必须返回 JSON 错误，**不抛异常也不静默成功**。 |

`runtime.cpp` 把 `ParameterService` 放 `app/` 而不是 `shared/`，
是因为它要引用 `measurement::FireParameters` / `SpectrumMeasurement`——
放 `shared/` 会直接违反 grep 守卫。

### 2.9 进程边界（2 对 + 1 脚本）

| 文件 | 作用 |
| --- | --- |
| `businessd/named_pipe_server.hpp/.cpp` | Win32 命名管道 + ACL 限当前用户（**设备独占，管道也应如此**）。协议是 JSON Lines：一次请求一行，一次响应一行。每连接一线程，但**业务层的设备访问串行化**（`Runtime::device_mutex`），因为驱动是同步阻塞的，多线程交错事务会串帧。`handle_line()` **公开就是为了让离线测试能直接调用**，不必真的开管道。 |
| `businessd/main.cpp` | 守护进程入口。`--config` / `--data-dir` / `--pipe` / `--no-devices` / `--print-default-config`。注册 Console Ctrl handler，**只置位标志**，真正的关闭在 `run()` 返回后。 |
| `remote/lpc.py` | Python 薄客户端，零第三方依赖（只用 `ctypes`）。 |

---

## 3. 横切关注点

| 关注点 | 落点 | 关键设计 |
| --- | --- | --- |
| **错误模型** | `error.hpp` | 协议/传输/设备三层共用一套分类，调用方只看一个 `ok()`，不必区分错误来自编解码还是串口 |
| **进程间协议** | `businessd` | JSON Lines，简单、可跨进程，Python 端十几行就能实现。**安全边界 = 管道 ACL 限当前用户** |
| **设备独占** | `serial.cpp` `share=0` + AvaSpec 命名互斥体 + Runtime 单点持有 | 物理约束变成代码约束 |
| **时间** | `time_util` | 墙钟与单调钟**分开不混**：前者会被校时改动，后者只算间隔 |
| **配置** | `site_config` + `parameter_store` | 按判据二分：**装机配置 = 硬件事实**（错了接不上），**测量参数 = 工艺事实**（错了只是测不准）。判据不同，缺一策略也不同 |
| **异步落盘** | `Writer` | 有界队列 + 丢最旧；安全路径（收光）与数据路径（写盘）解耦 |
| **测试替身** | `tests/fake_avaspec_sdk.cpp` | 编译出**替身 DLL** 替代厂家 SDK，路径可由参数或 `LIBS_FAKE_AVASPEC_DLL` 给出。设备层用各测试文件内的 `FakeLink` |
| **链接线看守** | `build.ps1` | `test_frame_queue.cpp` 只链 4 个纯内存文件——往 `frame_bus` 加任何设备调用会**直接链接失败**，而不是靠 code review 发现 |
| **无设备自检** | `--no-devices` | 全套业务测试不碰串口、不加载 SDK |

---

## 4. 硬约束（违反即失效，不只是风格问题）

这五条是这个架构真正的承重墙。

### 约束 1：判定只在 `MeasurementJob` 调用栈内执行

UI **不得**"先问门禁、拿到绿灯再自己决定要不要开始"——那会让判定出现**第二个执行点**。
UI 只读 `monitor()->latest()`；收到 `measurement_start` 的失败就当成"没开光"，原因看返回码。

> 反例的代价：`laser_run` / `laser_stop` / `laser_fire` 三条命令与 `LaserFireService`
> 已全部删除，全库 `startContinuous` 只允许出现在 `MeasurementJob` 的调用栈内。

### 约束 2：`readiness::evaluate()` 是零设备 I/O 的纯函数

安全逻辑**绝不能重写**。它必须能被离线穷举——给定一份 facts 就能算出所有结论，
不需要设备、不需要串口、不需要线程。

### 约束 3：`business/shared/` 不得反向依赖任何业务

由 `build.ps1` grep 守卫强制。`shared/` 是两条业务的共同底座。

### 约束 4：判定的执行方与收光的执行方必须是同一个对象

`start()` 那次门禁判定是**快照**，只反映 start() 那一瞬间的事实。连续出光可能
打到你手动停止，中间过了几十分钟。所以采集循环里**周期性重判**（500 ms）。

中途失锁要**自己收光**（`stopContinuous` + 置 `gate_lost_mid_run`）。为什么不能靠别的机制：

- 不能靠 UI——可能崩/被关/卡住；
- 不能靠互锁自己——互锁是"事实变了"，它不负责"去把光关掉"。

### 约束 5：停止随时可调、无条件执行

哪怕 `start()` 从没成功过、哪怕中途失败，`stop()` 都要尽力把光闸关掉。
**关串口不会让激光停下来**——设备自己保持着"工作"状态，守护进程崩了光还在打。

### `MeasurementJob` 七条纪律（`job.hpp` 文件头）

1. 门禁是唯一判定入口，不放行时**一条写指令都不发**
2. 运行期重判（500 ms），中途失锁自己收光
3. `wait_for_laser_trigger` 恒为 true（硬编码在 `capture_plan.cpp`）
4. 落盘异步，绝不阻塞采集节奏
5. 停止随时可调、无条件执行
6. **失败不伪造**：采集失败如实报错、帧里带 error code，绝不用零值填充出一条"平的谱"冒充数据；且采集失败**不终止循环**
7. 出光会话互斥由 Runtime 记账，本类不碰

---

## 5. 测试布局

`D:\LIBS\src\tests\`，12 套，**1084 个断言全部通过**，不碰真实 COM 口、不加载真实 AvaSpec DLL、不发光。

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
| `test_business_ipc` | 31 | 管道线路协议（`handle_line()` 公开，不真的建管道） |
| `test_frame_queue` | 68 | **保序、丢最旧帧背压、订阅者互不拖累、丢帧不移生产者帧号、订阅生命周期** |
| `test_measurement_job` | 91 | **门禁拦截每一条、每发必被采到、慢盘不反压采集、失败计数、停止幂等且确认收光** |

`test_measurement_job.cpp`（41 KB）是最大的一套，直接对应 §4 的约束 1/4/5——
比如 `gate_blocked_sends_no_write_frames`、`gate_lost_mid_run_shuts_the_laser_off_by_itself`、
`stop_confirms_the_laser_is_shut_down`。

---

## 6. 这个架构最值得注意的三个判断

### 6.1 "事实"和"判断"在代码上分得比通常更细

`facts.cpp` 一个判断都不做，只做极性取反；`verdict.cpp` 一个设备都不碰。
好处是安全逻辑可以离线穷举。

这不是洁癖：项目里已经真的发生过两处各写一遍 `raw == 1`，把危险读成安全。
所以 `parse_actual()` 被单独抽出来当**唯一转换点**并加离线测试。

### 6.2 重复 bug 被当作架构缺陷来防

头文件里保留了大量"曾经同时存在两份来源"的记录：

- `integration_delay_ns` 在 `SpectrometerConfig` 和参数库各存一份——界面改了参数库，实际生效的是配置里的另一个值
- 字段名 -> 工程单位的映射表，界面上的单位本来就是前端自己写的，同一个单位有两个来源
- `ApplyTiming::at_next_idle` **全库没有任何提交点**，暂存区永远不 commit，于是操作员调完参数、界面显示"已暂存"，而出光时读的仍是旧值

修法不是打补丁，是**消灭结构**：把字段归属收敛到唯一入口，必要时整条机制连根删掉
（`ApplyTiming` 连同 staged 机制一起删除，现在参数一律 apply 即写即生效）。

理由很实在：**沉默的"改了不生效"比报错更坏。**

### 6.3 C 类能力做成带前置条件的 TODO，而不是返回 0/false/空的桩

`UnsupportedGatewaySink`（网关交付）、`start_continuous()`（常驻判定线程）、
光谱 -> 成分算法都是这个处理。它们返回 `unsupported` + 需要补齐什么，
**绝不返回成功或空数据的假象**。

理由：假成功会让调用方以为事情办了，比明确失败坏得多。

---

## 7. 现状与文档的已知偏差

以下为**已核实**的事实差异，记录在此以免误判：

| # | 偏差 | 影响 |
| --- | --- | --- |
| 1 | `SerialPortConfig::baud` 默认 **9600**，而现场约定激光器是 **38400** | 配置若省略 `laser.baud` 会静默取到错值，与"缺配置不猜"的纪律相悖。**务必在 `site.json` 显式写 `"baud": 38400`** |
| 2 | `lpc.py` 注册的 `detection_start` / `laser_fire` / `composition` 三条命令在 facade 里**已不存在**；而 `measurement_start` / `measurement_stop` / `measurement_status` / `spectrum_stream` 四条**没有**注册 | 命令行调不到连续测量，且调旧命令会拿到 `unknown_command`。UI 走 `PipeClient` 不受影响 |
| 3 | TEC 写指令 `0x80`~`0x89` 未实现，`kReadCommands` 只有 `0xB1/0xB2/0xBC` | TEC 只读，与 `devices/laser.hpp` 声明的安全禁区一致 |
| 4 | `payload_length(0xBC)` 返回 **20**，手册 §4.2 记的是 **22** 个数据字节，且 `0xBC` 没有解码测试 | **以真机实测为准，不要据代码推断** |
| 5 | `ui/tests/stream_probe.cpp` 没有进 `ui/CMakeLists.txt` 或 `ui/build.ps1` | 需要手工编译才能跑 |

---

## 8. 相关文档

| 文档 | 内容 |
| --- | --- |
| [business-layer-two-businesses-execution-plan.md](business-layer-two-businesses-execution-plan.md) | 两条业务（readiness + measurement）的执行方案 |
| [business-layer-scope-report.md](business-layer-scope-report.md) | 12 项能力的范围裁剪与 A/B/C 分级 |
| [device-layer-consolidation-implementation-report.md](device-layer-consolidation-implementation-report.md) | 设备层收敛：单一控制器边界、迁移步骤、安全禁区、验收标准 |
| [device-gaps-and-continuous-fire-plan.md](device-gaps-and-continuous-fire-plan.md) | 设备缺口与连续出光方案 |
| [refactor-report.md](refactor-report.md) | 分层重构：依赖约束、已发现缺陷、已知限制 |