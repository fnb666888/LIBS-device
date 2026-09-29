# 实时光谱流（`spectrum_stream`）设计与前端说明

> 本文档描述 `D:\LIBS` 当前代码的实际状态，不是设计愿望书。
> 撰写日期：2026-09-29

---

## 1. 它是什么

给前端看的**背景/杂散光谱实时预览**。

```text
UI (Qt6 Widgets + Qt6 Charts)
  │  命名管道 JSON Lines，纯客户端
  ▼
businessd  →  services::SpectrumStream  →  CaptureCoordinator  →  libs::devices::Spectrometer
```

> **它不是 LIBS 测量谱。** 真实测量谱必须走 `laser_fire`：
> 先过安全门禁、真实出光，再采集。预览流**不发任何激光写指令**，
> 只是让操作员在出光前确认"视野里有没有东西"。

---

## 2. 五条纪律（改这个模块前先读这五条）

### 纪律 1：采集租约（`AcquisitionLease`）

预览和出光抢同一条光谱仪的 TTL。`AcquisitionLease` 是 RAII 的独占令牌：
`laser_fire` 与 `cmd_capture` 在动手前先持租约，流拿不到就**跳过这一帧**，
而不是排队等、更不是抢。租约在析构时自动释放，异常路径也不会漏。

### 纪律 2：通道三态，不是补零

一个通道有三种状态：`present` / `absent`。
**缺失通道标 `absent` + `reason`，绝不补零凑成一条平线。**
补零画出来是"这条谱是平的"，会让操作员误判成"这条谱线真的没有信号"。

### 纪律 3：背压丢最旧，绝不拖慢采集

每个订阅者队列上限 4 帧（`subscriber_queue_frames`）。
写不进去时丢**最旧**的，`dropped_frames` 如实上报。
宁可 UI 少看几帧，也不能让一个慢客户端把采集节奏拖住。

### 纪律 4：同步事实原样透传

`hardware_synchronized` 由 SDK 判定，**业务层绝不推断、绝不升级成 true**。
`requested_sync` 与 `hardware_synchronized` 分两个字段上送，
现场才能分清"我请求了同步"和"硬件真的同步了"。

### 纪律 5：自由触发 + 永久免责

预览默认 `follow_site_trigger_mode = false`：
只决定光谱仪要不要等 pin 6 的 TTL，**碰不到激光器一根线**。
UI 顶部的免责横幅常驻，不可关闭。

---

## 3. 管道命令面

管道：`\\.\pipe\libs-businessd`，JSON Lines（一行请求、一行响应）。

```json
{"command":"spectrum_stream","args":{"action":"status"}}
```

| action | 说明 |
| --- | --- |
| `start` / `stop` | 起停采集线程。**可反复起停**（见 §6 修过的坑） |
| `status` | running / frame_index / dropped / last_error / lease_held |
| `subscribe` | 返回 subscriber 句柄 |
| `unsubscribe` | 退订 |
| `latest` | 非阻塞取一帧。队列空时返回 `ok=true, has_frame=false`，**不是错误** |

帧结构（通道在 `channels` 数组里，每个通道自带 `state`）：

```json
{"frame_index":20,"ok":true,"hardware_synchronized":false,"requested_sync":false,
 "period_ms":200,"timeout_ms":3000,
 "channels":[
   {"serial":"MASTER","state":"present","pixels":4,"wavelength_nm":[300,301,302,303],"counts":[10,11,12,13]},
   {"serial":"SLAVE1","state":"present","pixels":7,"wavelength_nm":[...],"counts":[...]}
 ]}
```

采集失败时顶层 `ok=false` + `capture_error_code/message`，旧曲线在 UI 置灰并注明"保留上一帧"。

### 显示口径归 UI

业务层只传**原始波长 + 原始 counts**，不做归一化 / 基线扣除 / 平均。
归一化是显示口径，写死在 UI 里——写进业务层就等于把显示决策焊死在协议上。

---

## 4. 分层红线

```text
libs_protocols → libs_transport → libs_devices → libs_business
```

**UI 绝不能链接 `libs_devices` 或 `libs_business`。**
C++ 的 PUBLIC 依赖会传染到下游链接命令行，UI 一旦链上就等于绕过门禁直连设备。
UI 只是命名管道上的纯 JSON 客户端，连 JSON 解析都用 Qt 自带的 `QJsonDocument`，
不复用业务层 JSON——那也是反向依赖。

---

## 5. UI 构建与运行

UI 选型：**Qt6 Widgets + Qt6 Charts**（官方公开库，非私有依赖）。

```powershell
$env:PATH = "D:\Qt6\Tools\mingw1310_64\bin;$env:PATH"
cd D:\LIBS\src; pwsh -File build.ps1     # 守护进程
cd D:\LIBS\ui;  pwsh -File build.ps1     # UI
```

产物：`D:\LIBS\ui\build\libs-spectrum-ui.exe`，
Qt6 DLL 与平台插件由 `windeployqt` 拷进 `build\qt\`。
`windeployqt` 提示找不到 `dxcompiler.dll` / `dxil.dll` 属正常
（除非用 Direct3D 12，否则不加载）。

---

## 6. 踩过的坑（都已修，都有回归测试钉住）

### 6.1 管道 accept 循环必须每连接一个线程

原实现严格串行：上一个客户端断开到服务端下次 `CreateNamedPipe` 之间存在窗口。
UI 每 100ms 轮询会踩满，报 `ERROR_FILE_NOT_FOUND(2)` / `ERROR_PIPE_BUSY(231)`。
改成每连接一线程后，40 次零间隔 back-to-back 全部成功。

### 6.2 驱动激活必须幂等（`capture_timeout` 根因）

`Spectrometer::Impl::devices` 曾是**只追加、从不清理**的数组，
而 `enumerate()`（health 报告需要）和 `capture()` **都会**走激活。
于是在守护进程里，同一个序列号被激活两次、两次拿到**同一个句柄**：

```text
devices = [MASTER, SLAVE1, SLAVE2, MASTER, SLAVE1, SLAVE2]
```

后果不是"多了一路数据"，而是那一路被数了两遍：
`AVS_PollScan` 第一次就把该通道的 pending 取走，第二条同名记录永远等不到。
每帧必然以 `timed out waiting for MASTER, SLAVE1, SLAVE2` 收场，且**每帧递增**。
单独跑一个不枚举的诊断程序却完全正常——这个反差就是线索。

修法两条，缺一不可：
1. `activate()` 按序列号**幂等**，同一 serial 只激活一次、复用句柄；
2. `capture()` 只动**本次选中**的通道（`active` 下标表），
   不把 `enumerate()` 留下的通道一起 arm。

> 回归测试：`test_spectrometer.cpp` 的
> `capture_after_enumerate_does_not_double_activate_a_channel` 与
> `capture_only_touches_the_channels_it_was_asked_for`。
> 把修复回退后，第一个用例会复现原话
> `timed out waiting for MASTER, SLAVE1, SLAVE2`，
> 第二次采集会变成六路同名；第二个用例会复现 `timed out waiting for SLAVE1`。

### 6.3 流必须能反复起停

`stop()` 置 `stopping_ = true` 却从不复位，而 `start()` 原先判 `if (!stopping_)`，
于是**流只能启动一次**。第二次点"开始预览"：`start()` 返回成功、不报任何错，
但 `status` 里 `running` 一直是 false、一帧没有——按钮像是坏的。
`stop()` 侧"没在跑就直接返回"的短路把这个不对称盖住了。
现已在 `start()` 里复位 `stopping_`。

> 回归测试：`test_spectrum_stream.cpp` 的
> `stream_can_be_restarted_after_stop` 与 `start_while_running_is_a_noop`。

### 6.4 `configure()` 必须在 `devices_.open()` 之后

`devices_.spectrometer()` 返回的是打开之后才拿到的对象指针。
放在 `open()` 之前拿到的是 `nullptr`，于是 `health` 说"光谱仪: 已打开"、
流却一直报 `spectrometer_not_open`——一个说开了、一个说没开，
这种自相矛盾最难排障。

> 回归测试：`test_business_facade.cpp` 的
> `spectrum_stream_agrees_with_health_about_the_spectrometer`。

---

## 7. 排障速查

| 现象 | 先查 |
| --- | --- |
| `start` 成功但 `running=false`、无数据 | §6.3，租约是否被占（`lease_held`） |
| 每帧 `capture_timeout` 且缺失通道**逐帧递增** | §6.2，枚举与采集的顺序 |
| `spectrometer_open` 与 `health` 矛盾 | §6.4，`configure()` 的位置 |
| 管道 `ERROR_PIPE_BUSY` / `FILE_NOT_FOUND` | §6.1，accept 循环是否每连接一线程 |
| **UI 报「守护进程未连接」，但命令行/脚本连同一条管道正常** | §6.5，管道 ACL 是否真的生效（提权守护进程 + 普通桌面客户端） |
| 某通道画成一条平线 | 查该通道 `state` 是不是 `absent`（纪律 2） |
| `dropped_frames` 持续增长 | 背压在丢最旧帧（纪律 3）；UI 消费太慢，正常 |

### 6.5 管道 ACL：静默回落曾把普通桌面客户端挡在门外

现场现象：守护进程从**提权的 SSH 会话**启动，桌面上跑的 UI 报「守护进程未连接」，
而同一台机器上的脚本（也在提权会话里）连同一条管道完全正常。同一个用户、同一个 SID。

根因不在 UI，也不在 ACL 的写法，而在**那段 ACL 从来没执行过**：

```cpp
// 第一趟传空指针是"问需要多大缓冲"。这次调用**按设计就返回 FALSE**
// （失败 + ERROR_INSUFFICIENT_BUFFER），只需要它填出来的 size。
if (GetTokenInformation(token, TokenUser, nullptr, 0, &size) && size != 0)
//   ↑ 这个条件永远为假：取 SID 的整段代码一次都没跑过
```

于是安全描述符始终为空，`CreateNamedPipe` 用上了**令牌默认 ACL** ——
里面只有 SYSTEM 与 Administrators，**没有当前用户**。提权进程的令牌里
Administrators 是启用的，所以能连；普通桌面进程的令牌里那个组是
「仅用于拒绝」，于是被拒。

两条修法（都已落地）：

1. 第一趟 `GetTokenInformation` **不判返回值**，只看它填出来的 `size`。
2. 构造不出安全描述符就**拒绝启动**，不再带着默认 ACL 静默跑起来 ——
   这类"静默回落"正是本项目最忌讳的"未知当正常"。

同时给管道显式打了**中等完整性的强制标签**（`S:(ML;;NW;;;ME)`）：
守护进程将来若以 Windows 服务（SYSTEM、高完整性）方式运行，没有这条标签
同样会把中等完整性的桌面客户端挡在门外。
