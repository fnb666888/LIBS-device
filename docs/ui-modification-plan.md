# LIBS 现场检测系统 —— UI 重构方案

> 目标：把仍按旧架构（多业务、多设备、连续出光）设计的界面，重构为与重构后后端
> 一致的**两业务、三按钮、一屏主控 + 一页参数**的工控界面。
>
> **基线时间：2026-10-01 17:15。** 后端今天下午仍在改动，本文所有命令名、
> 字段名、行号都对 `D:\LIBS\src` 当前状态逐行核对过。**后端一改，本文即失效** ——
> 动手前先按 §12 的核对清单重新确认一遍。

---

## 0. 一句话结论

后端已经收敛成两条业务，UI 却停在中午。**要改的是 UI，不是配置、不是后端。**

| | 最后修改 | 状态 |
| --- | --- | --- |
| `src/src/business/app/facade.cpp` | 17:11 | 已重构，命令面 14 条 |
| `ui/src/main_window.cpp` | **14:25** | **停在旧架构** |

界面上有**两个按钮必然失败**：它们下发 `laser_fire`，而这条命令已被删除。
后端未知命令分支的原话：

> 「未知命令: …；可用命令见 commands 字段。注意：出光**只有** measurement_start
> 一条路径（原 laser_fire / laser_run / laser_stop 已删除）」

---

## 1. 现状诊断

### 1.1 UI 调用的命令 vs 后端现有命令面

后端 `facade.cpp::handle()` 现有 **14 条**：

```
ping  config_show  status  gate  param_set  param_get  capture
recovery_list  recovery_write  health
measurement_start  measurement_stop  measurement_status
spectrum_stream
```

UI 实际下发的：

| UI 位置 | 命令 | 后端是否有 |
| --- | --- | --- |
| `main_window.cpp:954` `on_precheck_fire` | `laser_fire {precheck_only:true}` | **❌ 已删除** |
| `main_window.cpp:1020` `on_fire_measure` | `laser_fire` | **❌ 已删除** |
| `main_window.cpp:1185` `on_run_continuous` | `measurement_start` | ✅ |
| `main_window.cpp:1223` `on_stop_continuous` | `measurement_stop` | ✅ |
| `main_window.cpp:913` | `gate` | ✅ |
| `main_window.cpp:936/1271` | `param_set` | ✅ |
| `status_poller.cpp` | `status` / `param_get` / `measurement_status` | ✅ |
| `stream_poller.cpp` | `spectrum_stream` | ✅ |

**结论**：UI 侧唯一真正过时的是「预检」和「单发测量」两条路径，都走已删除的
`laser_fire`。其余部分命令名还对得上，但**信息架构**仍然是旧架构。

### 1.2 信息架构层面的 7 个问题

| # | 问题 | 证据 | 严重度 |
| --- | --- | --- | --- |
| 1 | **四个过时槽函数。** `on_precheck_fire` / `on_fire_measure` / `on_run_continuous` / `on_stop_continuous` 对应"预检 + 单发出光 + 连续出光 + 停止"四个旧动作，现在后端只有 `measurement_start` / `measurement_stop` | `main_window.hpp:77-80` | **阻断** |
| 2 | **三个"启动"语义打架。** `start_btn_`（"启动检测"）只做订阅光谱 + `gate arm`；`fire_btn_`（"出光测量"）、`run_btn_`（"开始测量"）才真出光。操作员看到三个都在"启动" | `build_left_column()` :308-405 | 高 |
| 3 | **`start_btn_` 名不副实。** 它的 tooltip 自己写着"只订阅光谱展示通道，不启动任何采集" | `main_window.cpp:314-317` | 高 |
| 4 | **过时的设备状态字段。** `AcState`（空调）、`OreState`（测距/有矿）、`MotionState`（运动标定）、`belt_sequence` / `target_belt` / `ac_interlock_participates` 全部对应已移除的业务 | `machine_state.hpp` vs `device_session.cpp` 的 `health()` | 高 |
| 5 | **参数表与参数库字段名错位。** 界面 `FieldSpec::key` 用**旧字段名**，真正字段名在 `over` 列 | `main_window.cpp:83-105` | **阻断** |
| 6 | **参数卡与出光按钮同屏。** 参数表单在左列滚动区，改参数和出光并排，现场容易在测量中误触 | `build_left_column()` | 中 |
| 7 | **写了一个不存在的参数组。** `on_precheck_fire` 会先 `param_set {group:"trigger"}`，而 `required_groups()` 只有 `{"spectrometer","laser"}` | `main_window.cpp:939` | 中 |

### 1.3 【严重】参数页现在保存一定失败

`FieldSpec::key`（界面实际发出去的字段名）vs 参数库真实字段名：

| 界面 `key`（发出去的） | `over` 列（真实名） | 参数库真实字段名 | 后果 |
| --- | --- | --- | --- |
| `discharge_current` | `discharge_current_a` | `discharge_current_a` | **保存被拒** |
| `discharge_pulse_width` | `pulse_width_us` | `pulse_width_us` | **保存被拒** |
| `discharge_frequency` | `frequency_hz` | `frequency_hz` | **保存被拒** |
| `discharge_delay` | `discharge_delay_us` | `discharge_delay_us` | **保存被拒** |
| `q_delay` | `q_delay_us` | `q_delay_us` | **保存被拒** |
| `integration_ms` | `integration_ms` | `integration_ms` | ✓ |
| `timeout_ms` | `capture_timeout_ms` | `capture_timeout_ms` | **保存被拒** |

`ParameterService::apply_laser` 用 `take_uint(values, "discharge_current_a", ...)` 取值，
取不到就返回 `parameter_field_bad`（"laser.discharge_current_a 缺失或不是非负整数"）。
**现在点「应用到 laser」100% 失败。**

同一个错名还让 `sync_params_from_snapshot()`（`main_window.cpp:1317` 的 `vals.value(it.key)`）
永远取不到值，界面只能显示「`param_get 未返回该组`」。

`parameter_service.cpp:12` 的注释早已把这个坑写明：

> 「名字必须与配置结构对齐，否则参数库里存的是 "discharge_current"、
> 出光读的是 "discharge_current_a"，又变成两份来源，界面调了不生效。」

### 1.4 关于「设备预检」：后端现状要说清楚

写这份方案时后端正在改动，前后状态都核对了，如实记录以免下次误判：

- **17:11 之前的某个版本**：`precheck` 命令**已实现**（arm → 判定 → 刷灯，失败也带
  完整报告），`commands()` 15 条，4 个测试用例。**实测跑过，全绿。**
- **17:11 当前版本**：`precheck` 又**不在**命令面里了，命令面回到 14 条。

所以「设备预检」按钮的实现方式取决于后端最终形态。本文按**两版都给**处理：

- **若后端保留 `precheck` 命令** → UI 直接调用它，这是最干净的形态。
  注意失败时报告在 **`payload.report`**（`failure_with_payload` 把报告放子对象，
  避免顶层 `error` 键覆盖同名诊断字段）。
- **若后端不提供 `precheck` 命令**（当前状态） → 用 `gate {action:"arm"}` +
  `gate {action:"refresh"}` 两条命令实现，但必须在按钮 tooltip 里**如实写明**
  「本版本后端未接预检服务，不会刷新状态灯」。

**两种情况都绝不允许 UI 自己拼装"预检通过了"。** 预检结论只能来自后端返回值。

---

## 2. 目标信息架构

```
后端业务                界面落点
─────────────────────────────────────────────────────────
业务一 设备预检          按钮①「设备预检」+ 顶部门禁指示灯
  (readiness)           结论一律来自后端，UI 只翻译

业务二 LIBS 全流程       按钮②「开始检测」/ 按钮③「停止检测」
  (measurement)         + 显示区实时光谱 + 底部设备状态

参数管理                 独立页面「测量参数」+「保存」按钮
  (ParameterService)
```

界面收敛为 **两页**：

- **主页面** — 操作面。三按钮 + 实时光谱 + 底部设备状态条。
- **参数页** — 配置面。两组共 12 个字段 + 保存 + 放弃修改。

**不设第三页。** 不加流程向导 / 设备管理 / 日志中心 —— 后端没有对应命令面，
做出来只能是死的。

### 2.1 顶栏（常驻，两页共用）

| 元素 | 数据来源 | 说明 |
| --- | --- | --- |
| 标题 | 常量 | |
| 守护进程连接点 | `ping` | 断开时**整个操作区禁用**（此时所有命令都发不出去） |
| 门禁指示灯 | `status.gate` | 四态见 §5.3 |
| 门禁说明文字 | `status.gate.detail` + `blocked_by` | 原因码 → 人话，见 §5.4 |
| 参数页入口 | 常量 | 切到参数页 |
| 时钟 | 本机时间 | 只作参考，不与 `status` 耦合 |

顶栏**不放**出光按钮 —— 出光是主页面上的事，不要在任何页面都能一键触发。

---

## 3. 主页面布局

```
┌──────────────────────────────────────────────────────────────────────────┐
│ LIBS 现场检测系统  ● 已连接   门禁 ● 待机   上次判定：未布防     [测量参数] │
├──────────────────────────────────────────────────────────────────────────┤
│  门禁说明：未布防，请先执行设备预检                                        │
├──────────────────────────────────────────────────────────────────────────┤
│   ┌────────────┐    ┌────────────┐    ┌────────────┐                     │
│   │  设备预检   │    │  开始检测   │    │  停止检测   │                     │
│   │  60×60     │    │  60×60     │    │  60×60     │                     │
│   │            │    │ (主按钮)    │    │ (危险色)    │                     │
│   └────────────┘    └────────────┘    └────────────┘                     │
│    只读检测          发 measurement_start    发 measurement_stop        │
│    不出光            成功即已开光            把剩余帧写完再返回           │
├──────────────────────────────────────────────────────────────────────────┤
│  实时光谱                                                                │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │      ┌─ CH1 ●───                                            │    │
│  │      │      ┌─ CH2 ●──┐                                     │    │
│  │  ────┴──────┴─────────┴─────────────                       │    │
│  │  缺席：CH4（通道未采到：超时）                                 │    │
│  └──────────────────────────────────────────────────────────────────┘    │
│  第 128 帧 · 14:22:07 · 4 通道采到 3                                      │
│  ⚠ 本帧采集失败（capture_timeout）：曲线为上一帧，已置灰                  │
├──────────────────────────────────────────────────────────────────────────┤
│  设备状态                                                                │
│  ┌──────────┬──────────┬──────────┐                                       │
│  │ 激光器    │ 状态灯    │ 光谱仪    │                                       │
│  │ ● 已打开 │ ● 打开失败│ ● 已打开 │                                       │
│  │ COM10    │ COM11    │ SN1234   │                                       │
│  └──────────┴──────────┴──────────┘                                       │
│  测量：采到 128 · 失败 0 · 落盘 128 · 失败 0 · 积压 0                     │
└──────────────────────────────────────────────────────────────────────────┘
```

### 3.1 显示区规格

| 项 | 规格 |
| --- | --- |
| 控件 | 沿用现有自绘 `SpectrumPlot`（`ui/src/spectrum_plot.*`），**不引入 Qwt**，不换 QtCharts（见 §8.1） |
| 通道 | 4 路，颜色按 `serial` 配对，**不按数组下标** |
| 缺失通道 | `state==absent` → **不画线**，图例旁写「缺席：<reason>」。禁止补零画平线 |
| 整帧失败 | `ok==false` → 保留上一帧曲线 + **置灰** + 顶部红色横幅写 `code` |
| 归一化 | 若做，**只在 UI 侧做**（`frame.hpp` 明文纪律：帧里不含显示加工） |
| 帧信息行 | 帧序号、时间戳、通道采到数，来自 `spectrum_stream latest` |
| 免责声明 | 图内常驻小字：帧为激光触发实测谱 |

### 3.2 显示区数据流（一条纪律，不变）

`spectrum_stream` **没有 start/stop**（`spectrum_stream_start_and_stop_are_now_refused_with_a_pointer`
这条测试钉死了）。采集的唯一来源是 `measurement_start`。

所以显示区改成**全程订阅、随会话清屏**：

- 界面启动时 `spectrum_stream {action:"subscribe"}` 一次，之后一直订阅；
- `measurement_start` 成功 → **清屏**、开始刷新；
- `measurement_stop` 返回后 → 冻结最后一帧（不清屏、不退订）；
- 这样"没在测量时图是空的"成为一条**不需要解释的视觉事实**，
  消除了现在"图在动但没出光"这个最贵的误解。

现有 `StreamPoller` 的"短请求 `latest`、不占连接"策略**必须保留** ——
守护进程 accept 一次只服务一个客户端，长轮询会把别的客户端挤掉。

---

## 4. 三个按钮的精确语义

| 按钮 | 触发命令 | 超时 | 成功意味着什么 | 失败时 |
| --- | --- | --- | --- | --- |
| **设备预检** | 后端有 `precheck` → 调它<br>否则 → `gate {action:"arm"}` + `gate {action:"refresh"}` | 20000 ms | 门禁已布防、判定已刷新、状态灯已按结论刷新 | 文案转红"预检未通过"，横幅写 `detail`，**不弹模态** |
| **开始检测** | `measurement_start` | **60000 ms** | **已开光**。判定在 `job->start()` 内执行，成功即出光 | 横幅写 `code` + `message`；**一条写指令都不会发过激光器** |
| **停止检测** | `measurement_stop` | **30000 ms** | 已收光，剩余落盘队列已写完 | 横幅「停止未确认，请人工确认光闸」 |

超时值取自现有 UI 调用（`main_window.cpp:1185` 用 60000，`:1223` 用 30000），不新造。

### 4.1 红线：UI 绝不先 gate 再 start

正确形态是 `measurement_start` **要么成功并已开光，要么失败并说明原因**，
一个原子结果。判定真正的执行点仍然只有 `MeasurementJob::start()`
（`job.cpp:78` 的 `readiness_.evaluate_now()` 在任何 `laser_` 调用**之前**）。

所以：

- **禁止**「点开始检测 → 先 `gate refresh` → 看到 allowed 才发 start」；
- **禁止**「必须先点设备预检，否则拒绝发 start」。

理由不是洁癖：判定一旦在 UI 侧也跑一次，判定就出现第二个执行点。两个执行点之间
现场状态可能已经变了，届时 `allowed` 与实际安全状态不一致。

**UI 只把 `allowed` / `blocked_by` 翻译成人话显示，不自己决定能不能开始。**

`gate` 命令仍保留用途：只读查询与诊断，不作为 `measurement_start` 的前置条件。

### 4.2 按钮可用性（由后端状态决定，不由 UI 推理）

| 按钮 | 可点条件 | 禁用理由文案 |
| --- | --- | --- |
| 设备预检 | 守护进程已连接 | 守护进程未连接 |
| 开始检测 | `!status.laser_running` **且** 已连接 | 已有出光会话在进行 |
| 停止检测 | `status.laser_running` **且** 已连接 | 当前没有出光会话 |

`laser_running` 是**唯一**的按钮状态来源（`status` 字段，语义 = 是否有出光会话在进行）。
**不**用"我刚才点过开始所以应该在跑"这种本地推理。

### 4.3 出光确认

业务层**刻意不做**操作员确认（物理接线确认划给现场），所以确认落点就是
「开始检测」这一个按钮：

- 点击后弹一次模态确认，列出实际流程（写参数 → 工作使能 → 等放电 → 开光闸 → 收光 → 采集 → 落盘）；
- 确认框里**不要**写"门禁已放行"这类由 UI 判断的措辞，只写
  「若条件不满足，业务层不会发出任何写指令」；
- 「停止检测」**不做**确认弹窗 —— 停止是安全方向的动作，让人点两次才能收光是危险设计。
---

## 5. 底部设备状态条

### 5.1 设备就是三台

`DeviceSession::health()` 只账户三台：`laser` / `indicator` / `spectrometer`，
三者 `required` 均为 `true`（`enabled=true` 即 required）。空调与测距仪已移除，
**底部不再出现它们的位置**。

### 5.2 单台设备的状态点

`status.health.<name>` = `{configured, opened, endpoint, model, note}`。

| `configured` | `opened` | 显示 | 状态点 | 归类 |
| --- | --- | --- | --- | --- |
| `false` | 任意 | 「未配置」+ `note` | 灰 | **正常**，不告警 |
| `true` | `true` | 「已打开」+ `endpoint` | 绿 | 正常 |
| `true` | `false` | 「打开失败」+ `note` | **红** | **故障**：现场让你接的，没接上 |

三态必须分开：`configured=false` 是"这台机器本来就不接它"，
`configured=true && opened=false` 才是真故障。合并显示会让现场误判。

现有 `DeviceHealth::ok()` / `relevant()` 正好覆盖这个判定，直接用。

### 5.3 门禁指示灯四态

`status.gate` = `{allowed, blocked_by[], detail, state, armed, laser_readable}`。

| `state` | 显示 | 灯色 |
| --- | --- | --- |
| `standby` | 待机（未布防） | 黄 |
| `ready` + `armed` + `allowed=true` | 就绪（可出光） | 绿 |
| `ready` + `armed` + `allowed=false` | 已布防但条件不满足 | 橙 |
| `fault` | 故障 | 红 |
| 取不到（管道断） | 状态未知 | 灰 + "数据已过期" |

两条补充：

- `laser_readable=false` 时**额外**显示「读不到激光器状态」，
  区分"是没读到"与"读到了但不安全" —— 这两个的现场处置完全不同。
- `gate` 是**只读缓存**（`gate_snapshot_json` 明确不重跑判定），
  所以顶栏灯是"最近一次判定结论"，不是"此刻一定安全"。
  文案写「上次判定：…」，**不要写"安全"**。

### 5.4 `blocked_by` 原因码 → 人话

原因码来自 `readiness::to_string(GateReason)`。UI 侧维护映射表，
**未识别的码原样显示，不要吞掉** —— 吞掉原因码 = 现场拿不到排障方向。

现有 `main_window.cpp:131` 起的映射表可以复用，补齐缺项即可：

| 码 | 人话 |
| --- | --- |
| `laser_disabled` | 激光器在配置中未启用 |
| `laser_unavailable` | 读不到激光器状态，按不安全处理 |
| `laser_fault` | 激光器报故障（已有映射，:135） |
| `not_armed` | 门禁未布防，请先执行设备预检 |
| `parameters_incomplete` | 测量参数未齐备，请到参数页补齐（并列出缺哪些） |
| `indicator_unavailable` | 状态灯不可用（**灯是门禁之上的纯函数，不影响 allowed**，文案要说清） |

### 5.5 测量计数行

来自 `measurement_status`：`shots_captured` / `shots_failed` / `disk_writes_ok` /
`disk_writes_failed` / `disk_backlog` / `last_error_code` / `last_error_message`。

两条纪律：

- **光在出 ≠ 数据已落盘。** 落盘是另一条线程的事实，两者可以不一致。
  屏幕必须**分开显示**，不能因为"图上有曲线"就暗示"数据保存了"。
- **`disk_backlog` 是"写盘跟不上采集"的唯一信号。** `backlog>0` 时计数行标黄并写
  「写盘积压 N 帧」，不要静默。

---

## 6. 测量参数页

### 6.1 页结构

```
┌──────────────────────────────────────────────────────────────────────────┐
│  ‹ 返回主页面                    参数齐备 ● 缺 3 项                       │
├──────────────────────────────────────────────────────────────────────────┤
│  参数齐备状态：缺 q_warmup_s、settle_timeout_ms、emission_hold_ms         │
├──────────────────────────────────────────────────────────────────────────┤
│  激光器参数                                                              │
│  ┌──────────────────────────┬──────────┬──────────────┐                   │
│  │ 放电电流      (A)        │[ 80.0   ]│ 范围 15~100  │                   │
│  │ 放电脉宽      (μs)       │[ 200    ]│ 范围 50~300  │                   │
│  │ 放电频率      (Hz)       │[   1    ]│ 范围 1~10    │                   │
│  │ 放电延时      (μs)       │[   0    ]│ 范围 0~500   │                   │
│  │ Q 延时        (μs)       │[ 210    ]│ 范围 2~500   │                   │
│  │ Q 预热        (s)        │[   —    ]│              │                   │
│  │ 稳定等待超时  (ms)       │[   —    ]│              │                   │
│  │ 出光保持      (ms)       │[   —    ]│              │                   │
│  │ 最小放电次数             │[   —    ]│              │                   │
│  └──────────────────────────┴──────────┴──────────────┘                   │
│                                                                          │
│  光谱仪参数                                                              │
│  ┌──────────────────────────┬──────────┬──────────────┐                   │
│  │ 积分时间      (ms)       │[10.000  ]│ 0.002~60000  │                   │
│  │ 采集超时      (ms)       │[3000    ]│ 1~600000     │                   │
│  │ 积分延时      (ns)       │[5000    ]│ ≥0           │                   │
│  └──────────────────────────┴──────────┴──────────────┘                   │
│                                                                          │
│        ┌──────────────────────┐    ┌────────────┐                        │
│        │  放弃修改（重新读取） │    │   保存     │                        │
│        └──────────────────────┘    └────────────┘                        │
│  保存结果：● 已接受（laser）· 3 个字段被拒绝：…                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 6.2 字段表（唯一依据：`parameter_service.cpp`）

**`laser` 组（9 字段，全部 required）**

| 字段名（发 `param_set` 用这个） | 中文 | 校验 | 默认来源 |
| --- | --- | --- | --- |
| `discharge_current_a` | 放电电流 | 非负整数 | `SiteConfig::laser_fire` |
| `pulse_width_us` | 放电脉宽 | 非负整数 | 同上 |
| `frequency_hz` | 放电频率 | 非负整数 | 同上 |
| `discharge_delay_us` | 放电延时 | 非负整数 | 同上 |
| `q_delay_us` | Q 延时 | 非负整数 | 同上 |
| `q_warmup_s` | Q 预热 | 非负整数 | 同上 |
| `settle_timeout_ms` | 稳定等待超时 | 非负整数 | 同上 |
| `emission_hold_ms` | 出光保持 | 非负整数 | 同上 |
| `min_discharges` | 最小放电次数 | 非负整数 | 同上 |

**`spectrometer` 组（3 字段，全部 required）**

| 字段名 | 中文 | 硬校验 | 出厂默认 |
| --- | --- | --- | --- |
| `integration_ms` | 积分时间 | 数字，**0.002 ~ 60000 ms** | 10.0 |
| `capture_timeout_ms` | 采集超时 | **1 ~ 600000 的整数毫秒** | 3000.0 |
| `integration_delay_ns` | 积分延时 | 非负纳秒数 | 5000.0 |

`required_groups()` = `{"spectrometer", "laser"}` —— **只有这两组**，
界面上不出现第三组（现在那个 `trigger` 组要删掉）。

### 6.3 【必须修】`FieldSpec` 字段名

见 §1.3。改法：

- 删掉 `over` 列（出光覆盖参数这条路已随 `laser_fire` 删除）；
- `key` 直接填 §6.2 表里的真实字段名；
- **`key` 是唯一真值来源** —— 界面不得再维护第二份字段名映射。

改完后 `sync_params_from_snapshot()` 的 `vals.value(it.key)` 自然就能对上，
「`param_get 未返回该组`」那句提示会消失。

### 6.4 保存按钮的语义

「保存」= 对当前有改动的分组各发一次 `param_set`：

```json
{"group":"laser","values":{"discharge_current_a":80,"pulse_width_us":200}}
```

**四条纪律：**

1. **只发改动过的字段**，不整组覆盖。`param_set` 是**立即生效**的 ——
   测量进行中整组覆盖会直接改掉下一次出光的行为。
2. **必须检查 `completed`。** `completed=false` 表示"收到请求但未完成"，
   **不能当成功**（后端 `note` 字段原话：「调用方必须检查它，不得把非 completed
   当成成功」）。
3. **必须逐字段报 `failed_fields`。** 失败字段标红并**保留操作员输入** ——
   一次改 9 个字段错 1 个，整页回滚等于让人重填 9 个。
4. **`readiness.ready` 驱动门禁，但 `param_set` 不重跑判定。**
   保存成功后提示「参数已更新，下次判定生效」，**不要**说"现在可以出光了"。

**「参数立即生效」要在界面上说清楚**（`at_next_idle` 暂存机制已被删除，
曾导致"界面改了参数，这次采集还是用旧值"的真 bug）：

- 测量进行中保存参数，**下一帧**按新参数采集（`MeasurementJob` 每帧重读参数）；
- 但**正在出光的那一次**用的是它启动时读到的值。

### 6.5 「放弃修改」按钮

回读 `param_get` 覆盖输入框。理由：参数可能在别的终端被改过，
界面不能拿自己开窗时的值当真值。

---

## 7. 参数页可用范围（`gate.parameters_ready` 的现实）

参数齐备与否是**启动时就能确定的事实**，后端已在 `Runtime::start()` 里一次性注入
（`monitor_->set_parameters_ready(...)`），不依赖某个客户端先来问。

`param_set` 返回的 `readiness` = `{ready, summary}`。UI 用它驱动参数页顶部的
「参数齐备 / 缺 N 项」指示，缺哪些字段从 `param_get` 的各组 `complete` 标志取。

**`ready=false` 时的正确行为**：允许用户编辑并保存（补齐参数正是要做的），
但在主页面顶栏显示「参数未齐备 · 去参数页」并把 `parameters_incomplete` 翻译出来。
**不要**把「开始检测」按钮直接禁用 —— 那是 UI 自己做安全判定，违反 §4.1；
参数不齐时的拦截是 `measurement_start` 自己的事，它会返回 `gate_blocked` 并说明原因。

---

## 8. 文件级改动清单

### 8.1 保留（接口与纪律不动）

| 文件 | 保留理由 |
| --- | --- |
| `ui/src/spectrum_plot.hpp/.cpp` | 自绘方案是对的：4 通道 × 2048 点用 `QLineSeries` 每帧要拷 8192 次点，显示跟不上采集。不引入 Qwt，不换 QtCharts |
| `ui/src/stream_poller.hpp/.cpp` | "短请求 `latest`、不占连接"是硬约束 |
| `ui/src/status_poller.hpp/.cpp` | **必须在独立线程**：`status`/`param_get` 要过 `device_mutex`，串口卡住会冻住整个界面 |
| `ui/src/pipe_client.hpp/.cpp` | 命名管道客户端 |
| `ui/src/frame_model.hpp/.cpp` | UI 自己的 `FrameData`，**刻意不复用业务层 `SpectrumFrame`** —— 复用就得链接 `libs_business`，进程边界会被打破 |
| `ui/src/widgets.hpp/.cpp` | `StatusDot` / `Card` / `Metric` / `color_for(Level)` |
| `ui/src/theme.hpp/.cpp` | 主题 |

### 8.2 修改

| 文件 | 改动 |
| --- | --- |
| `ui/src/main_window.hpp/.cpp` | 删 4 个过时槽、3 个出光按钮成员、`fire_*` / `run_*` / `belt_*` / `seq_*` / `ac_*` / `motion_*` / `ore_*` 一批标签；参数表单整体移出；新增 `ParameterPage` |
| `ui/src/machine_state.hpp/.cpp` | 清理字段（见 §8.3） |
| **新增** `ui/src/parameter_page.hpp/.cpp` | 两组 12 字段 + 保存 + 放弃修改 |
| `ui/CMakeLists.txt` | 加 `parameter_page.cpp`；**继续只链接 Qt6 Core/Gui/Widgets，绝不链接 `libs_*`** |

### 8.3 `machine_state` 清理清单

**删除：**

| 成员 | 理由 |
| --- | --- |
| `AcState ac` | 空调已从 `DeviceSession` 移除（`reset()` 只留 laser/indicator/spectrometer） |
| `OreState ore` | 测距仪已移除 |
| `MotionState motion` | 运动/矿场业务已不在后端 |
| `belt_sequence`、`target_belt` | 无对应后端字段 |
| `ac_interlock_participates` | 空调互锁已不在门禁里 |
| `GateState::ac_interlock_ok`、`laser_enabled` | `status.gate` 不返回这两个（`gate_snapshot_json` 只输出 7 个字段） |
| `MachineSnapshot::stage` | `status` 不返回 `stage` |

**保留并强化：**

- `DeviceHealth::ok()` / `relevant()` —— §5.2 的三态判定直接用；
- `measurement_*` 全部字段 —— §5.5 的计数行直接用；
- `param_values` / `readiness_*` —— §6.4 的保存结果直接用；
- **三态原则**：每个可能取不到的量都带 `*_known` + 原因串。删字段时不要顺手把
  `*_known` 删成"默认 false" —— 那是"拿不到"被压成"是/否"，是现场排障时最贵的撒谎。

---

## 9. 必须保留的 UI 纪律

这 8 条是从业务层搬过来的语义，**不是 UI 自己的发明**，改版时一条都不能丢：

1. **通道 absent → 不画线**，图例旁写「缺席：<原因>」。补零画平线会让"没接"
   看起来像"信号为 0"。
2. **整次采集失败（`ok=false`）→ 保留上一帧曲线但置灰 + 红色横幅**写错误码。
   不静默停在旧数据上让人以为是新数据。
3. **永久免责声明**：帧是激光触发实测谱。**不引入自由触发预览** ——
   预览拍到的是背景谱，操作员分不清哪条曲线对应哪一发激光，实际判读极易出错。
4. **UI 不做安全判定。** 启动许可全部以 `measurement_start` 的返回码为准。
5. **UI 不复用业务层结构体。** 跨进程只走 JSON。
6. **UI 只用官方 Qt 模块。** 不引入 Qwt。
7. **归一化放 UI 不放业务层。** 显示口径写死在业务层，改口径就得改库。
8. **`StatusPoller` 必须在独立线程。** 一次 `status` 往返的耗时取决于现场设备有多慢，
   不取决于本机。UI 线程同步轮询 = 串口一卡整个界面冻住。

补充两条本版新增的：

9. **不造写不出去的控件。** 一个点了没反应的下拉框，比没有这个下拉框更伤现场。
   界面上的每个控件都必须能对上一条真实命令和一个真实字段。
10. **"不知道"必须是第三态。** 通信失败不压成 `0` / `false` / 空串。

---

## 10. 实施顺序

| 步 | 内容 | 依赖 | 可验收 |
| --- | --- | --- | --- |
| 1 | 修 `FieldSpec` 字段名（§6.3） | 无 | **点「保存」能被后端接受**（`completed=true`），刷新后回显一致 |
| 2 | 清理 `machine_state` 过时字段（§8.3） | 无 | 编译运行；界面不再出现空调/测距/矿带字样 |
| 3 | 拆出 `ParameterPage`（§6） | 步 1 | 两组 12 字段齐全；`failed_fields` 逐字段标红；`readiness.ready` 正确翻转 |
| 4 | 主窗口收敛为三按钮 + 光谱 + 设备条（§3/§4/§5） | 步 2、3 | 见下 |
| 5 | 预检按钮按后端最终形态接 `precheck` 或 `gate`（§1.4） | 后端定稿 | 见 §1.4 两版验收 |
| 6 | 清理过时槽与成员（§8.2） | 步 4 | 源码搜不到 `on_fire_measure` / `on_run_continuous` / `on_stop_continuous` / `on_precheck_fire` |
| 7 | 显示区改「全程订阅 + 随会话清屏」（§3.2） | 步 4 | 不测量时显示区为空屏 |

### 验收标准（步 4 完成）

- 主页面**只有**「设备预检」「开始检测」「停止检测」三个操作按钮；
- 光谱显示区显示的是 `measurement_start` 期间的实测帧；
- 底部设备条只显示激光器 / 状态灯 / 光谱仪三台，状态点三态分明；
- 三个按钮的可用性完全由 `status.laser_running` 与连接状态决定；
- 源码中**不存在**"先 `gate` 再 `start`"的调用序列；
- 参数页 12 个字段全部可改可存，保存失败能逐字段指出原因；
- 界面**不出现**空调、测距仪、运动、矿带、选矿等旧业务字样；
- **后端源码零改动** —— 本方案不要求后端改任何一行。

---

## 11. 本方案明确**不做**的事

- 不加"自由触发预览"（背景谱会被误读成测量谱）；
- 不加"连续出光"独立按钮（那是旧架构的第三条业务，已并入 `measurement_start`）；
- 不加流程向导 / 设备管理页 / 日志中心（后端无对应命令面，做出来是死的）；
- 不在业务层加显示归一化；
- 不把 `spectrum_stream` 的 start/stop 捡回来；
- 不动判定逻辑（`ReadinessMonitor::evaluate` / `GateVerdict` /
  `MeasurementJob::start` 的判定位置一个字都不改）；
- **不改任何配置文件**（`laser.enabled` 等按现场实际情况配，与 UI 重构无关）。

---

## 12. 动手前的核对清单

后端今天仍在改动。**开始写代码前先跑这一遍**，本文任何与代码不符之处以代码为准：

```powershell
# 1. 命令面（确认 14 或 15 条，以及 precheck 在不在）
Select-String -Path src\src\business\app\facade.cpp -Pattern 'if \(command =='

# 2. 参数库真实字段名（12 个）
Select-String -Path src\src\business\app\parameter_service.cpp -Pattern 'constexpr const char\* kField'

# 3. 后端测试是否全绿
cd src; pwsh -File build.ps1

# 4. UI 侧最后修改时间（确认后端比 UI 新）
Get-Item ui\src\main_window.cpp, src\src\business\app\facade.cpp | Select LastWriteTime, Name
```

核对结果若与本文不符，改本文而不是改代码 —— 方案的价值在于它是**当时**后端的
准确镜像。