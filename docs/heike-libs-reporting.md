# 海关边端报文与 LIBS 数据边界

附件中的三份文档描述了三条不同的数据链路，不能把它们合并成一个“光谱上传接口”。

## 协议方向

| 链路 | 方向 | 路由/事件 | 当前后端处理 |
| --- | --- | --- | --- |
| 设备监管 | 工控机 → 智能设备接入管理边端 | `HEARTBEAT`、`EVENT`、`OPENINIT`、`ALARM` | 已加入纯 JSON 构造与校验，可 dry-run |
| 模型结果 | 模型服务 → 边端 | `MODEL001`～`MODEL004` | 只允许透传真实模型结果，不自动生成 |
| 检验检疫接收 | 新查异系统 → 检验检疫应用 | `PISYS001`～`PISYS008`、`/sampling/hepsService/postMessage` | 暂不接入；这是另一方向的入站接口 |

设备监管报文是裸 JSON，例如 `HEARTBEAT`：

```json
{
  "equipmentID": "<设备编号>",
  "equipmentType": "LIBS",
  "customs": "<直属海关>",
  "timeseries": "2026-10-03 12:00:00"
}
```

模型报文带公共外层。附件表格把部分 `eventContent` 标成 JSON 字符串，但示例又直接放对象；当前 `model` 预览入口兼容对象或可解析为对象的 JSON 字符串，正式发送前仍需按目标边端版本确认输出形态。

```json
{
  "bizId": "<报关单号>",
  "contentSize": "<eventContent 的 UTF-8 字节数>",
  "contentType": "application/json",
  "customsCode": "<处置关区代码>",
  "eventCode": "MODEL001",
  "eventContent": {},
  "eventSource": "LIBS-device",
  "messageId": "<UUID>"
}
```

附件中的 IP、产品 ID 和 Bearer token 是示例或敏感配置，不能写入代码、日志或仓库。

## LIBS 和近红外不能混报

当前项目采集的是激光诱导击穿光谱（LIBS），真实结果是：

```json
{
  "spectrum_source": "LIBS",
  "spectrum_format": "wavelength_nm+counts",
  "scan_groups": [
    {
      "shot_id": 1,
      "complete": true,
      "hardware_synchronized": true,
      "arm_order": ["MASTER"],
      "channels": [
        {
          "serial": "MASTER",
          "pixels": 2048,
          "device_time_ticks": 0,
          "wavelength_nm": [300.0, 300.1],
          "counts": [123, 456]
        }
      ]
    }
  ]
}
```

这组数据不能直接填入附件的 `PISYS001.COMPO_*`，也不能直接填入 `MODEL003.nirSimlarityRESULT`：

- `PISYS001` 要求的是近红外/检化验形成的十项成分结果（Al2O3、CaO、FeO、MgO、MnO、P2O5、S、SiO2、TFe、TiO2），以及可选图片/文件字段。
- `MODEL003` 要求的是已经由模型计算出的相似度、固废名称和判定结果。
- LIBS 原始 `wavelength_nm + counts` 需要甲方确认一个独立的原始谱事件代码，或者先经过模型/算法转换为对方正式定义的结果字段。

后端因此在本地采集载荷中明确写入 `spectrum_source=LIBS`，并提供 `LIBS_RAW_V1` 形式的预览构造器；当前安全边界要求原始谱事件使用 `LIBS_` 命名空间，并拒绝 `PISYS*`/`MODEL*` 事件。正式 LIBS 事件代码仍需甲方确认，若对方使用其他命名空间需在确认后改动该校验。

## 后端接口

新增命令只构造报文，不访问外部网络：

```json
{
  "command": "heike_report_preview",
  "args": {
    "kind": "heartbeat",
    "equipment_id": "DEV-001",
    "equipment_type": "LIBS",
    "customs": "黄埔海关",
    "timeseries": "2026-10-03 12:00:00"
  }
}
```

支持 `heartbeat`、`event`、`openinit`、`alarm`、`libs_raw`、`model`。响应含 `transport=dry_run` 和 `sent=false`。模型报文必须由调用方传入真实的对象型 `eventContent`（也兼容可解析为对象的 JSON 字符串）；当前构造器校验事件名、UUID、对象形态并拒绝 LIBS 原始谱，但各 MODEL001～004 的业务字段完整性仍需按最终版本 schema 补充。

站点配置新增 `gateway` 字段：

```json
"gateway": {
  "enabled": false,
  "base_url": "http://<边端地址>:<端口>",
  "product_id": "<边端下发>",
  "device_id": "<边端下发>",
  "token": "<运行时凭证>",
  "equipment_id": "<设备编号>",
  "equipment_type": "LIBS",
  "customs": "<直属海关>",
  "timeout_ms": 5000
}
```

`config_show` 会返回 `token_configured`，不会返回 token。`gateway.enabled` 仍为 false 时，后端不会建立任何外部连接。

## 待确认项

1. LIBS 原始谱的正式事件代码、URL 路由和 `eventContent` 结构。
2. `eventContent` 在目标物模型中到底是 object 还是 JSON 字符串。
3. `BELT_CODE` 的类型、`OPENINIT.onlinePeriod` 的类型、`ALARM.data` 是单个 Map 还是数组，以及 `PISYS002` 缺失字段的最终 schema。
4. 设备监管事件与 `/properties/report` 属性上报是否属于同一版物模型。
5. 报关单号、皮带号、边端代码、设备编号等业务上下文的真实来源。

确认这些项目后，再实现 WinHTTP 出站客户端、启动/心跳/每日统计调度和断网 outbox；在此之前不向附件中的示例地址发包。
