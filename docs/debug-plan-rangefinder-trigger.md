# 测距、前端与激光联动调试方案

本文用于在工控机上复现和定位“测距仪读数 → 前端显示 → 激光器与光谱仪联合触发”链路。
每一级都先确认上一阶段的事实，再进入下一阶段。默认不发激光写指令。

## 1. 启动与版本确认

后端工作目录为 `C:\Users\user\libs`，配置文件为 `site.json`，命名管道为
`\\.\pipe\libs-businessd`。后端应在 Session 0 长驻，前端应在交互用户的 Session 1
运行。启动后先确认：

```powershell
Get-Process businessd,libs-spectrum-ui |
  Select-Object Id,ProcessName,SessionId,Responding,Path
```

再依次调用 `ping`、`health`、`status`。`health.all_required_open` 应为 `true`，
而 `status.laser_running` 必须为 `false`，`status.gate.allowed` 在未布防时应为
`false`。启动后不要直接调用 `laser_fire`。

## 2. 测距仪只读检查

每隔 2–3 秒读取两次 `status`，比较以下字段：

| 字段 | 期望 | 用途 |
| --- | --- | --- |
| `health.rangefinder.opened` | `true` | COM7 已打开 |
| `ore.read_ok` | `true` | 本次 Modbus 读取成功 |
| `ore.sample_counter` | 连续递增 | 测距读取线程仍在工作 |
| `ore.distance_register` | 514、513 或 512 | 实际采用的寄存器 |
| `ore.distance_resolution_mm` | 0.1 或 1.0 | 协议分辨率，不是精度承诺 |
| `ore.confidence_raw` | 设备协议范围内 | 原始置信度 |
| `ore.read_age_ms` | 较小 | 最近一次读数的新鲜度 |

TOF101 的 `0x0200`/`0x0201` 是同一距离的两种兼容表示，`0` 和 `0xffff` 才是
相应的超量程哨兵；不能把这两个寄存器当作应用距离上下限。`0x0202` 存在非零值时
优先采用，单位按协议为 `0.1 mm/LSB`。读取失败必须显示 `read_ok=false`，不能把
占位 `raw=0` 当成零距离。

现场还需用一个已知目标和无目标状态分别记录 `0x0200`–`0x0208` 的原始返回值，
确认设备型号的寄存器布局、哨兵值和单位；这一动作只读串口，不改写测距仪配置。

## 3. 前端显示检查

前端只通过命名管道消费后端状态，不直接打开 COM 口。检查右侧测距卡片：

1. 有效读数显示 `raw`、置信度、寄存器地址、分辨率和读数年龄。
2. 通信失败显示失败原因，不显示一个看似有效的 `0`。
3. raw 无效或超量程时显示警告。
4. `ore_present` 尚未完成现场标定时显示“判定未知”，不能显示“无矿”。
5. 前端重启后，后端不应重启、重复打开串口或改变激光状态。

## 4. 光谱仪自由预览（不出光）

先停止旧流，再确认 `running=false`、`subscribers=0`。需要背景预览时使用：

```json
{"command":"spectrum_stream","args":{"action":"start",
  "follow_site_trigger_mode":false,"min_period_ms":1000}}
```

随后 `subscribe`，循环调用 `latest`。预览帧应明确
`requested_sync=false`，且不能出现激光写指令。结束时调用 `stop`，再退订并确认
`running=false`。预览谱是背景/杂散光，不是 LIBS 测量谱。

## 5. 联合触发验证顺序

只有在前四步稳定、现场确认光路和急停条件后，才进入触发验证：

1. 调用 `{"command":"laser_fire","args":{"precheck_only":true}}`。返回成功只表示
   门禁允许出光，本次不发送任何写指令。
2. 用示波器或逻辑分析仪同时观察激光 TTL、光谱仪 pin 6 和放电计数器，先确认
   线缆极性、公共地、触发边沿和延时。
3. 由有权限的现场人员明确允许后，只执行一发真实 `laser_fire`。不使用前端的
   `detection_start` 代替它：`detection_start` 只是业务信号，不等于已出光。
4. 记录返回的 `fired`、`discharges`、`discharge_counter_before/after`、
   `spectrum_groups`、四路通道点数、`hardware_synchronized` 和本地落盘路径。
5. 出现任何失败时，确认 `q_gate_closed_after=true`、激光器已停机，再读取
   `health/status` 和业务日志。

判定“联合触发成功”至少要同时满足：放电计数递增、光谱仪返回四路非零有效谱、
`hardware_synchronized=true`、结果已落盘。只有看到前端曲线或命令返回成功，不能
单独证明 TTL 联动成立。

## 6. 常见错误定位

| 现象 | 优先检查 |
| --- | --- |
| `serial_not_available` | COM 口被其他进程占用、设备未上电、旧 businessd 未退出 |
| `capture_timeout` | 光谱仪在等待 pin 6；检查触发线、边沿、电平和等待超时 |
| `prepare_failed -5` | AvaSpec 通道激活/准备失败；检查 DLL、USB 通道和重复激活 |
| `laser_not_discharging` | 激光门禁、Q 门、急停、放电计数器或 0x21/0x24 时序 |
| `hardware_synchronized=false` | 只证明请求了同步但硬件未确认，不可由业务层推断为 true |
| 管道无响应 | businessd 是否仍在运行、命名管道是否存在、前后端是否使用同一目录 |

## 7. 当前边界

测距原始读数和前端可观测性已经接通；距离单位、矿石阈值、连续确认次数和滞回仍
需要用现场样本标定。因此当前不会自动把“有矿”推导成激光出光，也不会在 UI 启动时
自动触发激光。完成这些动作前，必须先补齐标定记录和现场安全批准。
