# 海关边端报文与 LIBS 数据边界

附件中的三份文档描述了三条不同的数据链路；LIBS 与近红外的分析结果可以共用结果报文，但设备状态、结果接收和本地原始谱诊断仍是不同链路。

## 协议方向

| 链路 | 方向 | 路由/事件 | 当前后端处理 |
| --- | --- | --- | --- |
| 设备监管 | 工控机 → 智能设备接入管理边端 | `HEARTBEAT`、`EVENT`、`OPENINIT`、`ALARM` | 已加入纯 JSON 构造与校验，可 dry-run |
| 模型结果 | 模型服务 → 边端 | `MODEL001`～`MODEL004` | 只允许透传真实模型结果，不自动生成 |
| 检验检疫接收 | 工控机/算法结果 → 检验检疫应用 | `PISYS001`～`PISYS008`、`/sampling/hepsService/postMessage` | LIBS 的已计算结果与近红外复用同一报文结构；原始谱不直接填结果字段 |

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

## LIBS 与近红外共用反馈结构

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

上面的原始数据不能直接填入附件的 `PISYS001.COMPO_*`，也不能直接填入 `MODEL003.nirSimlarityRESULT`。但 LIBS 完成定量或图谱算法后，反馈报文仍然使用近红外相同的字段和事件代码：

- `PISYS001` 使用十项成分结果（Al2O3、CaO、FeO、MgO、MnO、P2O5、S、SiO2、TFe、TiO2）以及可选图片/文件字段。LIBS 算法提供真实结果后，直接按此结构发送。
- `MODEL002`/`MODEL003` 继续使用附件定义的阈值结果/相似度结果结构。LIBS 可以作为算法来源，但不能把 `wavelength_nm + counts` 数组改名后冒充结果。
- `DATA_SOURCE` 目前只定义 `01=近红外`、`02=检化验`，附件没有给 LIBS 编码，因此不能自动填 `01` 或 `02`；缺少正式映射时保留为空，并在本地诊断信息中标记 LIBS。
- 十项成分、阈值判定、相似度和固废类别都必须来自真实 LIBS 标定/模型；缺失值保留 `null`，不能补零或填“正常”。

后端因此把 `spectrum_source=LIBS` 仅保留在本地采集和诊断数据中。`composition` 预览会输出 `eventCode=PISYS001`，并将 `eventContent` 固定为协议要求的 JSON 字符串；`model` 预览可输出 `MODEL002`/`MODEL003`，但会去除内部来源标记。原始谱诊断入口仍拒绝 `PISYS*`/`MODEL*` 路由，防止未经算法处理的数据进入结果接口。

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

支持 `heartbeat`、`event`、`openinit`、`alarm`、`composition`、`libs_raw`、`model`。`composition` 对应共用的 `PISYS001` 十项成分结构，响应还会给出本地 `measurement_method=LIBS`、缺失字段列表和完整性状态；这些诊断字段不进入上游报文。响应含 `transport=dry_run` 和 `sent=false`。模型报文必须由调用方传入真实结果，当前构造器拒绝原始谱结构，但各 MODEL001～004 的业务字段完整性仍需按最终版本 schema 补充。

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

1. LIBS 结果正式走 `PISYS001`/`MODEL002`/`MODEL003` 时，算法服务与数据源登记方式；`DATA_SOURCE` 是否新增正式 LIBS 编码。
2. `eventContent` 在目标物模型中到底是 object 还是 JSON 字符串（PISYS001 当前按字符串输出）。
3. `BELT_CODE` 的类型、`OPENINIT.onlinePeriod` 的类型、`ALARM.data` 是单个 Map 还是数组，以及 `PISYS002` 缺失字段的最终 schema。
4. 设备监管事件与 `/properties/report` 属性上报是否属于同一版物模型。
5. 报关单号、皮带号、边端代码、设备编号等业务上下文的真实来源。

确认这些项目后，再实现 WinHTTP 出站客户端、启动/心跳/每日统计调度和断网 outbox；在此之前不向附件中的示例地址发包。
