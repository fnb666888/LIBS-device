# 手动调试台接口

调试台通过业务守护进程的命名管道发送一行 JSON。每次请求只执行一个动作，
不会把检测启动、出光、采集、算法和上报串成自动流程。

## 通用报文

请求：

```json
{"command":"<command>","args":{}}
```

成功响应的外层为 `{"ok":true,"payload":{...}}`；失败响应仍返回 JSON，外层为
`ok=false`，并带 `error`、`error_kind`、`error_code`。UI 保留完整失败报文，便于现场
复制和保存。

## 可用单步接口

| command | 主要输入 | 输出/用途 |
|---|---|---|
| `ping` | 无 | 守护进程、版本和命令列表 |
| `status` / `health` / `config_show` | 无 | 当前状态、设备健康和站点配置 |
| `laser_fire` | 现有出光参数，或 `precheck_only=true` | 经过后台门禁的预检/单次出光 |
| `capture` | `from_config=true` 或采集节奏 | 单次原始 LIBS 采集，保存本地原始数据 |
| `motor` | `action=status/forward/reverse/jog_forward/jog_reverse/stop/reset_fault`；运动时提供 `frequency_percent`、`duration_ms` | SU300 状态或有界运动；停止动作可抢占 |
| `device_debug` | 见 [device-debug-api.md](device-debug-api.md) | 激光只读、测距读寄存器、空调/指示灯单步读写 |
| `param_get` / `param_set` | 参数组、键值、`timing` | 参数快照或显式保存结果 |
| `algorithm` | `action=contract/validate/analyze`，`input` 见 [algorithm-contract.md](algorithm-contract.md) | 契约、输入校验或算法占位结果 |
| `debug_context` | `action=get/set`，带号、序号、工作频率和确认标记 | 手工调试上下文；不会启动编排 |
| `heike_report_preview` | 报文类型和内容 | `transport=dry_run` 的离线预览，不联网 |
| `recovery_list` / `recovery_write` | 恢复快照参数 | 只读/写快照，不自动重放设备动作 |

### `debug_context` 示例

```json
{
  "command":"debug_context",
  "args":{
    "action":"set",
    "target_belt":"B-01",
    "belt_sequence":2,
    "frequency_hz":10.0,
    "unit_confirmed":true,
    "calibrated":true
  }
}
```

返回中包含 `manual_only=true` 和 `orchestration_started=false`。频率没有确认单位或
标定时，采集节奏仍会按后端规则保持未配置，不会猜默认值。

### `debug_contract`

```json
{"command":"debug_contract","args":{"action":"protocol_contract"}}
```

返回 `schema_version=libs.debug.v1`、调试命令清单和联动关闭声明，供 UI 或脚本发现
能力。设备写操作仍由后端设备锁、驱动协议和回读确认控制，UI 不能绕过门禁发送激光裸写。
