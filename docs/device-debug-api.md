# `device_debug` 手动设备调试接口

`device_debug` 是现场逐台联调入口。它绕过自动检测流程，但不绕过设备会话、串口互斥和驱动协议校验。所有请求在业务设备锁内串行执行。

```json
{"command":"device_debug","args":{"device":"laser","action":"status"}}
```

设备未打开、串口事务失败或回读不一致时，响应为 `ok=false`，不会把写入回显当成设备已执行。

## 激光器

只支持只读：

```json
{"device":"laser","action":"status"}
```

返回 `actual`（B2 实际状态）、`settings`（B1 设置）及各自的 `*_transaction`。字段同时给出 `raw/raw32/wide/scaled/value/unit`；协议未确认换算的字段 `value` 为 `null`。不提供裸寄存器写入、单发、开光闸或 EEPROM 操作；出光必须走 `laser_fire`/`laser_run` 门禁流程。

## 测距仪

默认读 `0x0200..0x0208`：

```json
{"device":"rangefinder","action":"read"}
```

也可以指定有界范围，`start` 必须在 `0x0200..0x0208`，`count` 必须在 `1..9`，且不得越界：

```json
{"device":"rangefinder","action":"read","start":514,"count":3}
```

返回寄存器 `raw/name/unit/valid` 和原始事务。`valid` 是协议哨兵解释，不等于已完成有矿判定。

## 空调

只读状态与参数：

```json
{"device":"air_conditioner","action":"status"}
```

单次开关机：

```json
{"device":"air_conditioner","action":"set_power","on":true}
```

单次模式写入（不会启动 100 秒模式保持循环）：

```json
{"device":"air_conditioner","action":"set_mode","mode":"cooling"}
```

模式可选 `standby/cooling/heating/dehumidify`。设定点只允许以下白名单：`cooling_on`、`cooling_off`、`heating_on`、`heating_off`、`indoor_high_alarm`、`indoor_low_alarm`：

```json
{"device":"air_conditioner","action":"set_setpoint","parameter":"cooling_on","value":25.0}
```

写入成功后必须回读确认；响应包含 `command_ack=true`、`confirmed=true`、事务和回读结果。不会通过此接口改变联锁策略。

## 指示灯

查询：

```json
{"device":"indicator","action":"query","channel":"red"}
```

写入：

```json
{"device":"indicator","action":"write","channel":"red","state":"on"}
```

通道为 `red/yellow/green/blue/white/buzzer`，状态为 `off/on/blink_1hz/blink_2hz`。型号只取站点配置的 `indicator_model`，不会由请求指定；写入后查询确认目标状态。该接口只服务人工调试，不自动把门禁状态映射到物理灯。
