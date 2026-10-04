# LIBS 算法调用契约

当前算法模型为空，代码先提供稳定的输入、校验和输出接口。实现位于
`src/include/libs/business/services/algorithm_contract.hpp` 与对应 `.cpp`；门面路由
由上层接入 `command=algorithm`，动作是 `contract`、`validate`、`analyze`。

## 输入

`args.input` 是一个对象，核心结构如下：

```json
{
  "schema_version": "libs.algorithm.v1",
  "request_id": "a-request-id",
  "context": {},
  "spectrum": {
    "spectrum_source": "LIBS",
    "spectrum_format": "wavelength_nm+counts",
    "scan_groups": [
      {
        "shot_id": 1,
        "complete": true,
        "hardware_synchronized": false,
        "arm_order": ["1706097U4"],
        "channels": [
          {
            "serial": "1706097U4",
            "pixels": 2048,
            "device_time_ticks": 42,
            "wavelength_nm": [400.0, 400.2],
            "counts": [123.0, 125.0]
          }
        ]
      }
    ]
  },
  "parameters": {},
  "model": {}
}
```

`spectrum` 的内容就是采集服务的 `wavelength_nm+counts` 原始 LIBS 数据。旧调用者可以
把同一对象放在 `raw_capture`，但仍必须满足相同的 `LIBS` 来源和格式校验。空模板、空
`scan_groups`、空通道或空数组都会被拒绝。波长与强度长度必须相同，数值必须有限；同一
组内序列号必须唯一。校验不会断言物理同步、光谱质量或模型精度。

## 输出

`analyze` 先执行输入校验。校验通过时仍返回 `availability=algorithm_missing` 和
`executed=false`，因为模型尚未接入。成分字段固定采用海科/近红外反馈的十个键：
`COMPO_AL2O3`、`COMPO_CAO`、`COMPO_FEO`、`COMPO_MGO`、`COMPO_MNO`、`COMPO_P2O5`、
`COMPO_S`、`COMPO_SIO2`、`COMPO_TFE`、`COMPO_TIO2`。所有值保持 `null`，不会把元素
`P` 自动换算成 `P2O5`。

分类和相似度输出保留稳定结构，但标签、置信度、分数和参考 ID 均为 `null`，匹配/类别
数组为空；质量状态为 `not_evaluated`。输出中的 `unit=percent` 仅表示未来接口的候选
单位，百分比还是质量分数、标定范围及小数精度必须由模型和现场标定方案确认，不代表真实
结果。

`contract` 返回字段说明、可供 UI 加载的输入模板和输出模板。输入模板中的空谱只是结构
示例，不能直接提交给 `validate` 或 `analyze`。
